/*
 * server.c — 스레드 하나 이벤트 루프 (epoll, level-triggered)
 *
 * 소켓을 아는 건 이 파일뿐이다. resp.c·command.c·store.c는 바이트와
 * 단어 목록만 받는다.
 *
 *   main ─┬─ read_listen_address   MINI_REDIS_BIND·MINI_REDIS_PORT (Q2)
 *         ├─ ignore_sigpipe        끊긴 소켓에 send해도 안 죽게 (E37)
 *         ├─ start_server          받는 소켓 + epoll + store
 *         └─ run_event_loop        epoll_wait → 깨어난 소켓마다
 *             └─ handle_ready_event
 *                 ├─ 받는 소켓: accept_connections → register_connection
 *                 └─ 연결 소켓: handle_connection_event
 *                     ├─ receive_and_run   recv 한 번 → run_commands
 *                     │   └─ run_commands  resp_parse → command_execute
 *                     │                    → append_reply (바로 다음에)
 *                     ├─ flush_output      보낼 수 있는 만큼 send
 *                     └─ update_watch      EPOLLIN / EPOLLOUT 켜고 끄기
 *
 * 버퍼 안을 가리키는 포인터 두 가지 때문에 순서가 정해져 있다.
 *  - args는 입력 버퍼 안을 가리킨다 → 입력 앞당기기는 명령을 다 처리한 뒤
 *  - BULK 응답은 store 안을 가리킨다 → command_execute 바로 다음에
 *    다른 store_* 호출 없이 출력 버퍼에 쓴다
 *
 * 2단계: 버퍼를 못 늘리면 바로 닫는다. purge 후 다시 시도는 4단계.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "command.h"
#include "resp.h"
#include "store.h"

/* ── 자료 구조 ─────────────────────────────────────────────── */

/* 비밀번호가 없어서 기본은 같은 컴퓨터에서만 받는다.
 * 노트북 두 대 데모 때만 MINI_REDIS_BIND=0.0.0.0으로 바꾼다. (11/14 결정) */
#define BIND_ENV             "MINI_REDIS_BIND"
#define DEFAULT_BIND_ADDRESS "127.0.0.1"

/* 외부 테스트(tests/e2e)와 같은 변수를 쓴다. */
#define PORT_ENV             "MINI_REDIS_PORT"
#define DEFAULT_PORT         "6380"
#define MAX_PORT             65535

/* 조각 1은 8칸 고정 (R1) */
#define STORE_BUCKETS        8

/* 한 번에 받아 올 준비된 소켓 수. 넘치는 건 다음 epoll_wait에 나온다. */
#define READY_EVENT_CAPACITY 64

/* 유휴 타임아웃을 1초마다 확인하려고 -1(무한) 대신 1초만 기다린다. */
#define EVENT_WAIT_MS        1000

/* recv 한 번에 받는 최대 바이트. 입력 버퍼는 "명령 하나 최대 + 이만큼"을
 * 넘지 않는다. (11/14 결정 다) */
#define RECV_CHUNK           16384

/* 버퍼를 처음 만들 때 크기. 모자라면 두 배씩 늘린다. */
#define INITIAL_BUFFER_SIZE  4096

/* 클라이언트 하나 = 연결 소켓 하나 = connection_t 하나 */
typedef struct connection {
    int fd;

    /* 아직 처리 안 한 요청 바이트. 덜 온 명령의 앞부분이 여기서 모인다. */
    char * input;
    size_t input_length;
    size_t input_capacity;

    /* output_sent까지는 이미 보냈다. 다 보내면 둘 다 0으로 되돌린다. */
    char * output;
    size_t output_length;
    size_t output_sent;
    size_t output_capacity;

    /* 형식 에러·너무 큼을 답한 뒤 끊는 중. 읽기를 멈추고 다 보내면 닫는다. */
    int is_closing;

    /* 지금 epoll에 걸어 둔 조건. 같으면 epoll_ctl을 부르지 않는다. */
    uint32_t watched_events;
} connection_t;

typedef struct {
    int listen_fd;
    int epoll_fd;
    store_t * store;
} server_t;

/* ── 도우미 함수 원형 ──────────────────────────────────────── */

static struct sockaddr_in * read_listen_address (struct sockaddr_in * address);
static const char * read_env_or_default (const char * name,
                                         const char * fallback);
static uint16_t * parse_port (const char * text, uint16_t * port);
static int is_all_digits (const char * text);
static void ignore_sigpipe (void);
static server_t * start_server (server_t * server,
                                const struct sockaddr_in * address);
static int create_listen_socket (const struct sockaddr_in * address);
static int close_with_error (int fd, const char * step);
static int watch_readable (int epoll_fd, int fd, void * owner);
static void print_listening (const struct sockaddr_in * address);
static void run_event_loop (server_t * server);
static void handle_ready_event (server_t * server,
                                const struct epoll_event * event);
static void accept_connections (server_t * server);
static void register_connection (server_t * server, int fd);
static connection_t * create_connection (int fd);
static void destroy_connection (connection_t * connection);
static int is_transient_accept_error (int error);
static int is_would_block (int error);
static void handle_connection_event (server_t * server,
                                     connection_t * connection,
                                     uint32_t events);
static void close_connection (server_t * server, connection_t * connection);
static connection_t * receive_and_run (store_t * store,
                                       connection_t * connection);
static ssize_t receive_once (connection_t * connection);
static char * ensure_input_space (connection_t * connection, size_t size);
static char * grow_buffer (char ** buffer, size_t * capacity, size_t needed);
static int is_connection_lost (ssize_t received);
static connection_t * run_commands (store_t * store,
                                    connection_t * connection);
static int64_t read_now_ms (void);
static connection_t * append_reply (connection_t * connection,
                                    const reply_t * reply);
static char * ensure_output_space (connection_t * connection, size_t size);
static void discard_sent_output (connection_t * connection);
static void compact_input (connection_t * connection, size_t offset);
static connection_t * flush_output (connection_t * connection);
static int has_unsent_output (const connection_t * connection);
static int is_done_closing (const connection_t * connection);
static int update_watch (int epoll_fd, connection_t * connection);
static uint32_t decide_watch_events (const connection_t * connection);

/* ── main ──────────────────────────────────────────────────── */

int main (void) {
    struct sockaddr_in address;
    if (read_listen_address(&address) == NULL) {
        return 1;
    }
    ignore_sigpipe();

    server_t server;
    if (start_server(&server, &address) == NULL) {
        return 1;
    }
    print_listening(&address);

    /* epoll_wait가 복구할 수 없게 실패할 때만 돌아온다. */
    run_event_loop(&server);
    return 1;
}

/* ── 도우미 함수 (처음 불리는 순서) ────────────────────────── */

/* 잘못된 값이면 이유를 출력하고 NULL. 서버를 띄우지 않는다. (11/14 결정) */
static struct sockaddr_in * read_listen_address (struct sockaddr_in * address) {
    const char * bind_text = read_env_or_default(BIND_ENV,
                                                 DEFAULT_BIND_ADDRESS);
    const char * port_text = read_env_or_default(PORT_ENV, DEFAULT_PORT);

    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    if (inet_pton(AF_INET, bind_text, &address->sin_addr) != 1) {
        fprintf(stderr, "%s가 잘못됐습니다: \"%s\" (IPv4 주소, 예: %s)\n",
                BIND_ENV, bind_text, DEFAULT_BIND_ADDRESS);
        return NULL;
    }

    uint16_t port;
    if (parse_port(port_text, &port) == NULL) {
        fprintf(stderr, "%s가 잘못됐습니다: \"%s\" (1~%d)\n",
                PORT_ENV, port_text, MAX_PORT);
        return NULL;
    }
    address->sin_port = htons(port);
    return address;
}

/* 변수가 아예 없을 때만 기본값. 빈 문자열은 잘못된 값으로 걸러지게 둔다. */
static const char * read_env_or_default (const char * name,
                                         const char * fallback) {
    const char * value = getenv(name);
    if (value == NULL) {
        return fallback;
    }
    return value;
}

/* atoi는 "80x"를 80으로 읽어 실수를 숨기고, strtoul은 앞의 공백·부호를
 * 받아 준다. 그래서 전부 숫자인지 먼저 본다. 실패하면 NULL */
static uint16_t * parse_port (const char * text, uint16_t * port) {
    if (!is_all_digits(text)) {
        return NULL;
    }
    errno = 0;
    unsigned long value = strtoul(text, NULL, 10);
    if (errno == ERANGE || value < 1 || value > MAX_PORT) {
        return NULL;
    }
    *port = (uint16_t) value;
    return port;
}

/* 빈 문자열은 거짓 */
static int is_all_digits (const char * text) {
    if (*text == '\0') {
        return 0;
    }
    for (const char * cursor = text; *cursor != '\0'; cursor++) {
        if (!isdigit((unsigned char) *cursor)) {
            return 0;
        }
    }
    return 1;
}

/* 끊긴 소켓에 send하면 기본 동작은 프로세스 종료다. 무시하면 send가
 * -1(EPIPE)을 돌려주고 그 연결만 닫으면 된다. send의 MSG_NOSIGNAL과
 * 이중으로 막는다. (E37) */
static void ignore_sigpipe (void) {
    signal(SIGPIPE, SIG_IGN);
}

/* 실패하면 이유를 출력하고, 만든 것을 다 정리하고 NULL */
static server_t * start_server (server_t * server,
                                const struct sockaddr_in * address) {
    int listen_fd = create_listen_socket(address);
    if (listen_fd == -1) {
        return NULL;
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        close_with_error(listen_fd, "epoll_create1");
        return NULL;
    }

    /* 받는 소켓은 owner NULL로 연결 소켓과 구분한다. */
    if (watch_readable(epoll_fd, listen_fd, NULL) == -1) {
        close_with_error(listen_fd, "epoll_ctl(받는 소켓)");
        close(epoll_fd);
        return NULL;
    }

    store_t * store = store_new(STORE_BUCKETS);
    if (store == NULL) {
        fprintf(stderr, "store_new 실패: 메모리가 부족합니다\n");
        close(listen_fd);
        close(epoll_fd);
        return NULL;
    }

    server->listen_fd = listen_fd;
    server->epoll_fd = epoll_fd;
    server->store = store;
    return server;
}

/* 반환: 받는 소켓 fd. 실패하면 이유를 출력하고 -1 (fd라 NULL을 쓸 수 없음) */
static int create_listen_socket (const struct sockaddr_in * address) {
    /* 스레드가 하나라 accept에서 멈추면 모든 클라이언트가 같이 멈춘다. */
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd == -1) {
        perror("socket");
        return -1;
    }

    /* 재시작 직후 TIME_WAIT 때문에 bind가 실패하지 않게 한다. */
    int enabled = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &enabled, sizeof(enabled)) == -1) {
        return close_with_error(listen_fd, "setsockopt(SO_REUSEADDR)");
    }
    if (bind(listen_fd, (const struct sockaddr *) address,
             sizeof(*address)) == -1) {
        return close_with_error(listen_fd, "bind");
    }
    /* accept 전까지 커널이 handshake를 끝낸 접속을 줄 세워 둔다. */
    if (listen(listen_fd, SOMAXCONN) == -1) {
        return close_with_error(listen_fd, "listen");
    }
    return listen_fd;
}

/* errno가 close로 바뀌기 전에 출력한다. 반환: 항상 -1 (바로 return 하려고) */
static int close_with_error (int fd, const char * step) {
    fprintf(stderr, "%s 실패: %s\n", step, strerror(errno));
    close(fd);
    return -1;
}

/* owner: epoll_wait가 돌려줄 표시. 반환: 0 성공, -1 실패 (errno 그대로) */
static int watch_readable (int epoll_fd, int fd, void * owner) {
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.ptr = owner;
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
}

static void print_listening (const struct sockaddr_in * address) {
    char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
    printf("%s:%d에서 받는 중\n", text, ntohs(address->sin_port));
    fflush(stdout);
}

static void run_event_loop (server_t * server) {
    struct epoll_event ready[READY_EVENT_CAPACITY];
    for (;;) {
        int count = epoll_wait(server->epoll_fd, ready,
                               READY_EVENT_CAPACITY, EVENT_WAIT_MS);
        if (count == -1 && errno != EINTR) {
            perror("epoll_wait");
            return;
        }
        for (int i = 0; i < count; i++) {
            handle_ready_event(server, &ready[i]);
        }
    }
}

static void handle_ready_event (server_t * server,
                                const struct epoll_event * event) {
    if (event->data.ptr == NULL) {
        accept_connections(server);
        return;
    }
    handle_connection_event(server, event->data.ptr, event->events);
}

/* level-triggered라 줄 선 접속을 EAGAIN이 나올 때까지 다 받는다.
 * EMFILE(fd 꽉 참)이면 받는 소켓이 계속 깨어난다. 6단계에서 처리. */
static void accept_connections (server_t * server) {
    for (;;) {
        int fd = accept4(server->listen_fd, NULL, NULL, SOCK_NONBLOCK);
        if (fd != -1) {
            register_connection(server, fd);
            continue;
        }
        if (is_transient_accept_error(errno)) {
            continue;
        }
        if (!is_would_block(errno)) {
            perror("accept4");
        }
        return;
    }
}

/* fd: 넘겨받는다. 실패하면 여기서 닫는다. */
static void register_connection (server_t * server, int fd) {
    connection_t * connection = create_connection(fd);
    if (connection == NULL) {
        close(fd);
        return;
    }
    if (watch_readable(server->epoll_fd, fd, connection) == -1) {
        perror("epoll_ctl(연결 소켓)");
        destroy_connection(connection);
        return;
    }
}

/* fd: 성공하면 넘겨받는다 (destroy_connection이 닫음).
 *     실패하면 호출자가 닫는다.
 * 버퍼는 처음 쓸 때 만든다. 반환: 실패하면 NULL */
static connection_t * create_connection (int fd) {
    connection_t * connection = malloc(sizeof(*connection));
    if (connection == NULL) {
        return NULL;
    }
    memset(connection, 0, sizeof(*connection));
    connection->fd = fd;
    connection->watched_events = EPOLLIN;
    return connection;
}

/* epoll 등록은 건드리지 않는다. 등록된 연결은 close_connection으로 닫는다. */
static void destroy_connection (connection_t * connection) {
    close(connection->fd);
    free(connection->input);
    free(connection->output);
    free(connection);
}

/* 그 접속 하나만 실패한 경우. 줄에 남은 다음 접속은 계속 받는다. */
static int is_transient_accept_error (int error) {
    return error == ECONNABORTED || error == EINTR || error == EPROTO;
}

static int is_would_block (int error) {
    return error == EAGAIN || error == EWOULDBLOCK;
}

/* 이벤트 하나에 recv도 한 번 (11/14 결정 다).
 * 응답은 EPOLLOUT을 기다리지 않고 바로 보내 본다. 못 다 보낸 것만
 * EPOLLOUT으로 이어 보낸다. */
static void handle_connection_event (server_t * server,
                                     connection_t * connection,
                                     uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) {
        close_connection(server, connection);
        return;
    }
    if ((events & EPOLLIN) && !connection->is_closing
        && receive_and_run(server->store, connection) == NULL) {
        close_connection(server, connection);
        return;
    }
    if (flush_output(connection) == NULL) {
        close_connection(server, connection);
        return;
    }
    if (is_done_closing(connection)) {
        close_connection(server, connection);
        return;
    }
    if (update_watch(server->epoll_fd, connection) == -1) {
        perror("epoll_ctl(감시 조건 변경)");
        close_connection(server, connection);
        return;
    }
}

/* close만 해도 epoll에서 빠지지만, 닫는 순서를 한 곳에 모아 둔다.
 * 나중 단계에서 유휴 목록·대기 목록에서 빼는 일도 여기에 붙는다. */
static void close_connection (server_t * server, connection_t * connection) {
    epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, connection->fd, NULL);
    destroy_connection(connection);
}

/* 반환: 닫아야 하면 NULL (상대가 끊음, recv 에러, 버퍼 메모리 부족) */
static connection_t * receive_and_run (store_t * store,
                                       connection_t * connection) {
    ssize_t received = receive_once(connection);
    if (is_connection_lost(received)) {
        return NULL;
    }
    if (received < 0) {
        return connection;
    }
    return run_commands(store, connection);
}

/* 반환: recv 그대로. 입력 버퍼를 못 늘리면 errno = ENOMEM으로 -1 */
static ssize_t receive_once (connection_t * connection) {
    char * space = ensure_input_space(connection, RECV_CHUNK);
    if (space == NULL) {
        errno = ENOMEM;
        return -1;
    }
    ssize_t received = recv(connection->fd, space, RECV_CHUNK, 0);
    if (received > 0) {
        connection->input_length += (size_t) received;
    }
    return received;
}

/* 반환: 입력 끝(쓸 자리). 못 늘리면 NULL (입력 버퍼는 그대로) */
static char * ensure_input_space (connection_t * connection, size_t size) {
    size_t needed = connection->input_length + size;
    if (needed > connection->input_capacity
        && grow_buffer(&connection->input, &connection->input_capacity,
                       needed) == NULL) {
        return NULL;
    }
    return connection->input + connection->input_length;
}

/* needed 이상이 될 때까지 두 배씩 늘린다.
 * 성공하면 *buffer·*capacity를 바꾸고 새 버퍼를 돌려준다.
 * 실패하면 NULL이고 *buffer·*capacity는 그대로 (realloc은 원래 것을 둠) */
static char * grow_buffer (char ** buffer, size_t * capacity, size_t needed) {
    size_t new_capacity = *capacity;
    if (new_capacity == 0) {
        new_capacity = INITIAL_BUFFER_SIZE;
    }
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    char * grown = realloc(*buffer, new_capacity);
    if (grown == NULL) {
        return NULL;
    }
    *buffer = grown;
    *capacity = new_capacity;
    return grown;
}

/* received: recv의 반환값. 0이면 상대가 끊음, -1이면 EAGAIN·EINTR만 괜찮다.
 * 상대가 끊으면 못 보낸 응답도 버린다 (실제 Redis와 같음). */
static int is_connection_lost (ssize_t received) {
    if (received > 0) {
        return 0;
    }
    if (received == 0) {
        return 1;
    }
    return !is_would_block(errno) && errno != EINTR;
}

/* 입력 버퍼의 완성된 명령을 앞에서부터 전부 처리한다.
 * 형식 에러·너무 큼이면 에러를 답하고 닫는 중으로 바꾼다. 그 뒤의 입력은
 * 읽지 않고 버린다. (E24, E31)
 * 반환: 출력 버퍼를 못 늘리면 NULL */
static connection_t * run_commands (store_t * store,
                                    connection_t * connection) {
    size_t offset = 0;
    while (!connection->is_closing) {
        int argc = 0;
        command_arg_t args[RESP_MAX_ARGS];
        size_t consumed = 0;
        parse_result_t result = resp_parse(connection->input + offset,
                                           connection->input_length - offset,
                                           &argc, args, &consumed);
        if (result == PARSE_INCOMPLETE) {
            break;
        }

        reply_t reply;
        if (result == PARSE_DONE) {
            int64_t now_ms = read_now_ms();
            command_execute(store, argc, args, now_ms, &reply);
            offset += consumed;
        } else {
            resp_fill_error_reply(result, &reply);
            connection->is_closing = 1;
            offset = connection->input_length;
        }
        if (append_reply(connection, &reply) == NULL) {
            return NULL;
        }
    }
    compact_input(connection, offset);
    return connection;
}

/* 만료는 상대 시각(지금 + 초 × 1000)이라, 시스템 시계를 바꿔도
 * 흔들리지 않는 MONOTONIC을 쓴다. 0 이상이다 (command.c 요구). */
static int64_t read_now_ms (void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* reply가 BULK면 store 안을 가리키므로 command_execute 바로 다음에,
 * 다른 store_* 호출 없이 부른다. 반환: 출력 버퍼를 못 늘리면 NULL */
static connection_t * append_reply (connection_t * connection,
                                    const reply_t * reply) {
    size_t size = resp_reply_size(reply);
    char * space = ensure_output_space(connection, size);
    if (space == NULL) {
        return NULL;
    }
    connection->output_length += resp_write_reply(reply, space);
    return connection;
}

/* 자리가 모자라면 이미 보낸 앞부분부터 비우고, 그래도 모자라면 늘린다.
 * 반환: 출력 끝(쓸 자리). 못 늘리면 NULL (안 보낸 응답은 그대로) */
static char * ensure_output_space (connection_t * connection, size_t size) {
    if (connection->output_length + size > connection->output_capacity) {
        discard_sent_output(connection);
    }
    size_t needed = connection->output_length + size;
    if (needed > connection->output_capacity
        && grow_buffer(&connection->output, &connection->output_capacity,
                       needed) == NULL) {
        return NULL;
    }
    return connection->output + connection->output_length;
}

static void discard_sent_output (connection_t * connection) {
    if (connection->output_sent == 0) {
        return;
    }
    size_t unsent = connection->output_length - connection->output_sent;
    memmove(connection->output, connection->output + connection->output_sent,
            unsent);
    connection->output_length = unsent;
    connection->output_sent = 0;
}

/* 처리한 앞부분(offset까지)을 지우고 덜 온 명령을 맨 앞으로 당긴다.
 * args가 입력 버퍼 안을 가리키므로 응답을 다 쓴 뒤에만 부른다. */
static void compact_input (connection_t * connection, size_t offset) {
    if (offset == 0) {
        return;
    }
    size_t remaining = connection->input_length - offset;
    memmove(connection->input, connection->input + offset, remaining);
    connection->input_length = remaining;
}

/* 송신 버퍼가 찰 때까지(EAGAIN) 보낸다.
 * 반환: send 에러(상대가 끊음 등)면 NULL */
static connection_t * flush_output (connection_t * connection) {
    while (has_unsent_output(connection)) {
        ssize_t sent = send(connection->fd,
                            connection->output + connection->output_sent,
                            connection->output_length - connection->output_sent,
                            MSG_NOSIGNAL);
        if (sent > 0) {
            connection->output_sent += (size_t) sent;
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        if (sent == -1 && is_would_block(errno)) {
            return connection;
        }
        return NULL;
    }
    connection->output_length = 0;
    connection->output_sent = 0;
    return connection;
}

static int has_unsent_output (const connection_t * connection) {
    return connection->output_sent < connection->output_length;
}

/* 에러 응답까지 다 보냈으면 참 ("에러를 답한 뒤 끊는다", resp.h) */
static int is_done_closing (const connection_t * connection) {
    return connection->is_closing && !has_unsent_output(connection);
}

/* 반환: 0 성공, -1 실패 (errno 그대로) */
static int update_watch (int epoll_fd, connection_t * connection) {
    uint32_t wanted = decide_watch_events(connection);
    if (wanted == connection->watched_events) {
        return 0;
    }
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = wanted;
    event.data.ptr = connection;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, connection->fd, &event) == -1) {
        return -1;
    }
    connection->watched_events = wanted;
    return 0;
}

/* 닫는 중엔 EPOLLIN을 뺀다. 안 읽는 입력 때문에 level-triggered 알림이
 * 끝없이 와서 CPU를 다 쓰지 않게 한다. */
static uint32_t decide_watch_events (const connection_t * connection) {
    if (connection->is_closing) {
        return EPOLLOUT;
    }
    if (has_unsent_output(connection)) {
        return EPOLLIN | EPOLLOUT;
    }
    return EPOLLIN;
}
