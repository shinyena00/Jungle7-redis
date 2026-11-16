<<<<<<< HEAD
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "resp.h"

=======
/*
 * server.c — 스레드 하나 이벤트 루프 (epoll, level-triggered)
 *
 * 소켓을 아는 건 이 파일뿐이다. resp.c·command.c·store.c는 바이트와
 * 단어 목록만 받는다.
 *
 *   main ─┬─ read_listen_address   MINI_REDIS_BIND·MINI_REDIS_PORT (Q2)
 *         ├─ ignore_sigpipe        끊긴 소켓에 send해도 안 죽게 (E37)
 *         ├─ start_server          받는 소켓 + epoll + store
 *         └─ run_event_loop        epoll_wait → 소켓 이벤트 → 명령 대기열
 *             └─ handle_ready_event
 *                 ├─ 받는 소켓: accept_connections → register_connection
 *                 └─ 연결 소켓: handle_connection_event
 *                     ├─ receive_commands  recv 한 번 → 명령 대기열
 *                     └─ finish_output     보낼 수 있는 만큼 send
 *             └─ run_pending_commands      연결마다 최대 100개 실행
 *                 ├─ run_commands         파싱 → 명령 실행 → 응답 저장
 *                 └─ finish_output        send → EPOLLIN / EPOLLOUT 갱신
 *
 * 버퍼 안을 가리키는 포인터 두 가지 때문에 순서가 정해져 있다.
 *  - args는 입력 버퍼 안을 가리킨다 → 입력 앞당기기는 명령을 다 처리한 뒤
 *  - BULK 응답은 store 안을 가리킨다 → purge 뒤 재시도할 때 다시 조회한다.
 */
#include <arpa/inet.h>
#include <ctype.h>
>>>>>>> 96db220 (feat: server.c)
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

<<<<<<< HEAD
#define DEFAULT_PORT 6380
#define LISTEN_BACKLOG 128
#define EVENT_COUNT 64
#define READ_BUFFER_SIZE 4096
#define STORE_BUCKET_COUNT 8

typedef struct {
    char * data;
    size_t length;
    size_t capacity;
} buffer_t;
=======
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

/* 저장소가 처음 쓰는 버킷 수. */
#define STORE_BUCKETS        8

/* 한 번에 받아 올 준비된 소켓 수. 넘치는 건 다음 epoll_wait에 나온다. */
#define READY_EVENT_CAPACITY 64

/* 유휴 타임아웃을 1초마다 확인하려고 -1(무한) 대신 1초만 기다린다. */
#define EVENT_WAIT_MS        1000

/* 대기열에 있는 연결은 먼저 남은 요청을 처리한 뒤 다시 읽는다. */
#define RECV_CHUNK           16384

/* 버퍼를 처음 만들 때 크기. 모자라면 두 배씩 늘린다. */
#define INITIAL_BUFFER_SIZE  4096

#ifndef COMMANDS_PER_ROUND
#define COMMANDS_PER_ROUND    100
#endif

#ifndef IDLE_TIMEOUT_MS
#define IDLE_TIMEOUT_MS       (5 * 60 * 1000)
#endif

#define OUTPUT_LIMIT         (4 * 1024 * 1024)
#define OUTPUT_LIMIT_ERROR \
    "-ERR 출력 버퍼 한도를 초과했습니다\r\n"

_Static_assert(COMMANDS_PER_ROUND > 0, "COMMANDS_PER_ROUND must be positive");
_Static_assert(IDLE_TIMEOUT_MS > 0, "IDLE_TIMEOUT_MS must be positive");

typedef enum {
    CLOSE_NONE,
    CLOSE_AFTER_REPLY,
    CLOSE_AFTER_OUTPUT_LIMIT,
} close_reason_t;

typedef struct connection_link {
    struct connection_link * previous;
    struct connection_link * next;
    struct connection * connection;
} connection_link_t;

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

    /* 한도 오류를 따로 보내면 출력 버퍼를 더 늘릴 필요가 없다. */
    size_t output_error_sent;
    close_reason_t close_reason;

    int64_t last_input_ms;
    connection_link_t pending_link;
    connection_link_t idle_link;

    /* 지금 epoll에 걸어 둔 조건. 같으면 epoll_ctl을 부르지 않는다. */
    uint32_t watched_events;
} connection_t;

typedef struct {
    int listen_fd;
    int epoll_fd;
    store_t * store;
    connection_link_t pending;
    connection_link_t idle;
    int64_t next_idle_check_ms;
    /* -1이면 받는 소켓을 감시 중이다. */
    int64_t listen_retry_ms;
    int failed;
} server_t;
>>>>>>> 96db220 (feat: server.c)

typedef struct {
    int descriptor;
    buffer_t input;
    buffer_t output;
    size_t sent_length;
    bool close_after_reply;
} client_t;

<<<<<<< HEAD
static int find_port(void);
static int open_listener(int port);
static int run_event_loop(int listener, store_t * store);
static void dispatch_events(int listener, int poller,
                            const struct epoll_event * events, int count,
                            store_t * store);
static void accept_client(int listener, int poller);
static void destroy_client(client_t * client);
static void handle_client_event(int poller, struct epoll_event event,
                                store_t * store);
static void write_client(int poller, client_t * client, store_t * store);
static bool is_retryable_io_error(void);
static void process_request(int poller, client_t * client, store_t * store);
static void update_client_interest(int poller, client_t * client,
                                   uint32_t events);
static buffer_t * copy_reply(buffer_t * output, const char * message);
static buffer_t * reserve_buffer(buffer_t * buffer, size_t length);
static int64_t read_now_ms(void);
static buffer_t * encode_reply(buffer_t * output, command_reply_t reply);
static buffer_t * encode_frame(buffer_t * output, const char * prefix,
                               command_reply_t reply);
static void read_client(int poller, client_t * client, store_t * store);
=======
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
static void resume_accepting (server_t * server, int64_t now_ms);
static void close_idle_connections (server_t * server, int64_t now_ms);
static int find_wait_ms (const server_t * server, int64_t now_ms);
static void handle_ready_event (server_t * server,
                                const struct epoll_event * event);
static void accept_connections (server_t * server);
static void handle_accept_error (server_t * server, int error);
static void pause_accepting (server_t * server);
static void register_connection (server_t * server, int fd);
static connection_t * create_connection (int fd);
static void destroy_connection (connection_t * connection);
static int is_transient_accept_error (int error);
static int is_would_block (int error);
static void handle_connection_event (server_t * server,
                                     connection_t * connection,
                                     uint32_t events);
static connection_t * receive_commands (server_t * server,
                                        connection_t * connection);
static void record_input_activity (server_t * server,
                                   connection_t * connection);
static void run_pending_commands (server_t * server);
static void finish_output (server_t * server, connection_t * connection);
static void close_connection (server_t * server, connection_t * connection);
static ssize_t receive_once (connection_t * connection);
static char * ensure_input_space (connection_t * connection, size_t size);
static char * grow_buffer (char ** buffer, size_t * capacity, size_t needed);
static int is_connection_lost (ssize_t received);
static connection_t * run_commands (server_t * server,
                                    connection_t * connection);
static connection_t * run_next_command (store_t * store,
                                        connection_t * connection,
                                        size_t * offset);
static int64_t read_now_ms (void);
static connection_t * append_reply_with_purge (store_t * store,
                                               connection_t * connection,
                                               reply_t * reply, int argc,
                                               const command_arg_t * args,
                                               int64_t now_ms);
static int is_output_limit_exceeded (const connection_t * connection,
                                     const reply_t * reply);
static connection_t * append_reply (connection_t * connection,
                                    const reply_t * reply);
static char * ensure_output_space (connection_t * connection, size_t size);
static void discard_sent_output (connection_t * connection);
static void compact_input (connection_t * connection, size_t offset);
static connection_t * flush_output (connection_t * connection);
static connection_t * send_bytes (connection_t * connection, const char * data,
                                  size_t length, size_t * offset);
static int has_unsent_output (const connection_t * connection);
static int is_done_closing (const connection_t * connection);
static int update_watch (int epoll_fd, connection_t * connection);
static uint32_t decide_watch_events (const connection_t * connection);
static void stop_server (server_t * server);
static void initialize_list (connection_link_t * list);
static int is_list_empty (const connection_link_t * list);
static void push_back_link (connection_link_t * list,
                            connection_link_t * link);
static void remove_link (connection_link_t * link);
>>>>>>> 96db220 (feat: server.c)

/* ── 공개 함수 ── */

int main (void) {
<<<<<<< HEAD
    int port = find_port();
    if (port < 0) {
        fputs("MINI_REDIS_PORT는 1~65535 사이여야 합니다.\n", stderr);
        return EXIT_FAILURE;
=======
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
    stop_server(&server);
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
>>>>>>> 96db220 (feat: server.c)
    }

    int listener = open_listener(port);
    if (listener < 0) {
        perror("listen");
        return EXIT_FAILURE;
    }

    store_t * store = store_new(STORE_BUCKET_COUNT);
    if (store == NULL) {
        close(listener);
        fputs("저장소를 만들 메모리가 부족합니다.\n", stderr);
        return EXIT_FAILURE;
    }

<<<<<<< HEAD
    int result = run_event_loop(listener, store);
    store_free(store);
    close(listener);
    return result;
=======
    server->listen_fd = listen_fd;
    server->epoll_fd = epoll_fd;
    server->store = store;
    initialize_list(&server->pending);
    initialize_list(&server->idle);
    server->next_idle_check_ms = read_now_ms() + EVENT_WAIT_MS;
    server->listen_retry_ms = -1;
    server->failed = 0;
    return server;
>>>>>>> 96db220 (feat: server.c)
}

static int find_port (void) {
    const char * setting = getenv("MINI_REDIS_PORT");
    if (setting == NULL) {
        return DEFAULT_PORT;
    }

    char * end;
    long port = strtol(setting, &end, 10);
    if (end == setting || *end != '\0' || port < 1 || port > UINT16_MAX) {
        return -1;
    }
    return (int)port;
}

static int open_listener (int port) {
    int descriptor = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                            0);
    if (descriptor < 0) {
        return -1;
    }

    int enabled = 1;
<<<<<<< HEAD
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR,
                   &enabled, sizeof(enabled)) < 0 ||
        bind(descriptor, (const struct sockaddr *)&address,
             sizeof(address)) < 0 ||
        listen(descriptor, LISTEN_BACKLOG) < 0) {
        int error = errno;
        close(descriptor);
        errno = error;
=======
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
    while (!server->failed) {
        int64_t now_ms = read_now_ms();
        resume_accepting(server, now_ms);
        close_idle_connections(server, now_ms);
        if (server->failed) {
            return;
        }
        int count = epoll_wait(server->epoll_fd, ready, READY_EVENT_CAPACITY,
                               find_wait_ms(server, now_ms));
        if (count == -1 && errno != EINTR) {
            perror("epoll_wait");
            return;
        }
        for (int i = 0; i < count; i++) {
            handle_ready_event(server, &ready[i]);
        }
        run_pending_commands(server);
    }
}

static void resume_accepting (server_t * server, int64_t now_ms) {
    if (server->listen_retry_ms < 0 || now_ms < server->listen_retry_ms) {
        return;
    }
    if (watch_readable(server->epoll_fd, server->listen_fd, NULL) == -1) {
        perror("epoll_ctl(받는 소켓 재등록)");
        server->failed = 1;
        return;
    }
    server->listen_retry_ms = -1;
}

/* 오래된 연결이 앞에 있으므로 살아 있는 연결을 만나면 검사를 끝낸다. */
static void close_idle_connections (server_t * server, int64_t now_ms) {
    if (now_ms < server->next_idle_check_ms) {
        return;
    }
    server->next_idle_check_ms = now_ms + EVENT_WAIT_MS;
    while (!is_list_empty(&server->idle)) {
        connection_t * connection = server->idle.next->connection;
        if (now_ms - connection->last_input_ms < IDLE_TIMEOUT_MS) {
            return;
        }
        close_connection(server, connection);
    }
}

static int find_wait_ms (const server_t * server, int64_t now_ms) {
    if (!is_list_empty(&server->pending)) {
        return 0;
    }
    int64_t deadline_ms = server->next_idle_check_ms;
    if (server->listen_retry_ms >= 0 && server->listen_retry_ms < deadline_ms) {
        deadline_ms = server->listen_retry_ms;
    }
    if (deadline_ms <= now_ms) {
        return 0;
    }
    return (int) (deadline_ms - now_ms);
}

static void handle_ready_event (server_t * server,
                                const struct epoll_event * event) {
    if (event->data.ptr == NULL) {
        accept_connections(server);
        return;
    }
    handle_connection_event(server, event->data.ptr, event->events);
}

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
        handle_accept_error(server, errno);
        return;
    }
}

static void handle_accept_error (server_t * server, int error) {
    if (is_would_block(error)) {
        return;
    }
    if (error == EMFILE || error == ENFILE) {
        pause_accepting(server);
        return;
    }
    perror("accept4");
}

/* fd가 찬 동안 LT 이벤트가 같은 수신 소켓을 계속 깨우지 않게 한다. */
static void pause_accepting (server_t * server) {
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, server->listen_fd,
                  NULL) == -1) {
        perror("epoll_ctl(받는 소켓 일시 해제)");
        server->failed = 1;
        return;
    }
    server->listen_retry_ms = read_now_ms() + EVENT_WAIT_MS;
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
    record_input_activity(server, connection);
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
    connection->pending_link.connection = connection;
    connection->idle_link.connection = connection;
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

/* 이미 읽은 명령이 밀려 있으면 다음 recv는 그 뒤로 미룬다. */
static void handle_connection_event (server_t * server,
                                     connection_t * connection,
                                     uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) {
        close_connection(server, connection);
        return;
    }
    if ((events & EPOLLIN) && connection->close_reason == CLOSE_NONE
        && connection->pending_link.next == NULL
        && receive_commands(server, connection) == NULL) {
        close_connection(server, connection);
        return;
    }
    if (events & EPOLLOUT) {
        finish_output(server, connection);
    }
}

static connection_t * receive_commands (server_t * server,
                                        connection_t * connection) {
    ssize_t received = receive_once(connection);
    if (is_connection_lost(received)) {
        return NULL;
    }
    if (received > 0) {
        record_input_activity(server, connection);
        push_back_link(&server->pending, &connection->pending_link);
    }
    return connection;
}

static void record_input_activity (server_t * server,
                                   connection_t * connection) {
    connection->last_input_ms = read_now_ms();
    remove_link(&connection->idle_link);
    push_back_link(&server->idle, &connection->idle_link);
}

/* 처음 줄 서 있던 연결까지만 처리해야 epoll이 새 소켓도 볼 수 있다. */
static void run_pending_commands (server_t * server) {
    connection_link_t * last = server->pending.previous;
    while (!is_list_empty(&server->pending)) {
        connection_link_t * next = server->pending.next;
        int is_last = next == last;
        connection_t * connection = next->connection;
        remove_link(next);
        if (run_commands(server, connection) == NULL) {
            close_connection(server, connection);
        } else {
            finish_output(server, connection);
        }
        if (is_last) {
            return;
        }
    }
}

static void finish_output (server_t * server, connection_t * connection) {
    if (flush_output(connection) == NULL || is_done_closing(connection)) {
        close_connection(server, connection);
        return;
    }
    if (update_watch(server->epoll_fd, connection) == -1) {
        perror("epoll_ctl(감시 조건 변경)");
        close_connection(server, connection);
    }
}

static void close_connection (server_t * server, connection_t * connection) {
    remove_link(&connection->pending_link);
    remove_link(&connection->idle_link);
    epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, connection->fd, NULL);
    destroy_connection(connection);
    if (server->listen_retry_ms >= 0) {
        server->listen_retry_ms = read_now_ms();
    }
}

/* 반환: recv 그대로. 입력 버퍼를 못 늘리면 errno = ENOMEM으로 -1 */
static ssize_t receive_once (connection_t * connection) {
    char * space = ensure_input_space(connection, RECV_CHUNK);
    if (space == NULL) {
        errno = ENOMEM;
>>>>>>> 96db220 (feat: server.c)
        return -1;
    }
    return descriptor;
}

static int run_event_loop (int listener, store_t * store) {
    int poller = epoll_create1(EPOLL_CLOEXEC);
    if (poller < 0) {
        perror("epoll_create1");
        return EXIT_FAILURE;
    }

    struct epoll_event interest = {.events = EPOLLIN, .data.ptr = NULL};
    if (epoll_ctl(poller, EPOLL_CTL_ADD, listener, &interest) < 0) {
        perror("epoll_ctl");
        close(poller);
        return EXIT_FAILURE;
    }

<<<<<<< HEAD
    struct epoll_event events[EVENT_COUNT];
    for (;;) {
        int count = epoll_wait(poller, events, EVENT_COUNT, -1);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            perror("epoll_wait");
            break;
        }
        dispatch_events(listener, poller, events, count, store);
    }
    close(poller);
    return EXIT_FAILURE;
}

static void dispatch_events (int listener, int poller,
                             const struct epoll_event * events, int count,
                             store_t * store) {
    for (int i = 0; i < count; i++) {
        if (events[i].data.ptr == NULL) {
            accept_client(listener, poller);
        } else {
            handle_client_event(poller, events[i], store);
        }
    }
}

static void accept_client (int listener, int poller) {
    int descriptor = accept4(listener, NULL, NULL,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (descriptor < 0) {
        return;
    }

    client_t * client = calloc(1, sizeof(*client));
    if (client == NULL) {
        close(descriptor);
        return;
    }
    client->descriptor = descriptor;

    struct epoll_event interest = {.events = EPOLLIN, .data.ptr = client};
    if (epoll_ctl(poller, EPOLL_CTL_ADD, descriptor, &interest) < 0) {
        destroy_client(client);
    }
}

static void destroy_client (client_t * client) {
    close(client->descriptor);
    free(client->input.data);
    free(client->output.data);
    free(client);
}

static void handle_client_event (int poller, struct epoll_event event,
                                 store_t * store) {
    client_t * client = event.data.ptr;
    if (event.events & (EPOLLERR | EPOLLHUP)) {
        destroy_client(client);
        return;
    }
    if (event.events & EPOLLOUT) {
        write_client(poller, client, store);
        return;
    }
    if (event.events & EPOLLIN) {
        read_client(poller, client, store);
    }
}

static void write_client (int poller, client_t * client, store_t * store) {
    const char * data = client->output.data + client->sent_length;
    size_t remaining = client->output.length - client->sent_length;
    ssize_t count = send(client->descriptor, data, remaining, MSG_NOSIGNAL);
    if (count < 0 && is_retryable_io_error()) {
        return;
    }
    if (count <= 0) {
        destroy_client(client);
        return;
    }

    client->sent_length += (size_t)count;
    if (client->sent_length < client->output.length) {
        return;
    }
    if (client->close_after_reply) {
        destroy_client(client);
        return;
    }

    client->sent_length = 0;
    client->output.length = 0;
    process_request(poller, client, store);
}

static bool is_retryable_io_error (void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static void process_request (int poller, client_t * client, store_t * store) {
    buffer_t * input = &client->input;
    resp_request_t request;
    resp_status_t status = parse_resp_request(input->data, input->length,
                                              &request);
    if (status == RESP_INCOMPLETE) {
        update_client_interest(poller, client, EPOLLIN);
        return;
    }
    if (status != RESP_COMPLETE) {
        const char * message = find_resp_error_reply(status);
        if (copy_reply(&client->output, message) == NULL) {
            destroy_client(client);
            return;
        }
        client->close_after_reply = true;
        update_client_interest(poller, client, EPOLLOUT);
        return;
    }

    int64_t now_ms = read_now_ms();
    if (now_ms < 0) {
        destroy_client(client);
        return;
    }
    command_reply_t reply = execute_command(store, request.arguments,
                                            request.argument_count, now_ms);
    /* 조회 결과는 다음 저장소 명령 전에 송신 버퍼에 복사한다. */
    if (encode_reply(&client->output, reply) == NULL) {
        destroy_client(client);
        return;
    }

    input->length -= request.consumed_length;
    memmove(input->data, input->data + request.consumed_length, input->length);
    update_client_interest(poller, client, EPOLLOUT);
}

static void update_client_interest (int poller, client_t * client,
                                    uint32_t events) {
    struct epoll_event interest = {.events = events, .data.ptr = client};
    if (epoll_ctl(poller, EPOLL_CTL_MOD, client->descriptor, &interest) < 0) {
        destroy_client(client);
    }
}

static buffer_t * copy_reply (buffer_t * output, const char * message) {
    size_t length = strlen(message);
    if (reserve_buffer(output, length) == NULL) {
        return NULL;
    }
    memcpy(output->data, message, length);
    output->length = length;
    return output;
}

static buffer_t * reserve_buffer (buffer_t * buffer, size_t length) {
    if (length <= buffer->capacity) {
        return buffer;
    }

    size_t capacity = buffer->capacity;
    if (capacity < READ_BUFFER_SIZE) {
        capacity = READ_BUFFER_SIZE;
    }
    while (capacity < length && capacity <= SIZE_MAX / 2) {
        capacity *= 2;
    }
    if (capacity < length) {
        capacity = length;
    }

    char * data = realloc(buffer->data, capacity);
    if (data == NULL) {
        return NULL;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return buffer;
}

static int64_t read_now_ms (void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
=======
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

/* 반환: 출력 버퍼를 못 늘리면 NULL */
static connection_t * run_commands (server_t * server,
                                    connection_t * connection) {
    size_t offset = 0;
    for (int count = 0; count < COMMANDS_PER_ROUND; count++) {
        if (offset == connection->input_length
            || connection->close_reason != CLOSE_NONE) {
            break;
        }
        size_t previous_offset = offset;
        if (run_next_command(server->store, connection, &offset) == NULL) {
            return NULL;
        }
        if (offset == previous_offset) {
            compact_input(connection, offset);
            return connection;
        }
    }
    compact_input(connection, offset);
    if (connection->input_length > 0
        && connection->close_reason == CLOSE_NONE) {
        push_back_link(&server->pending, &connection->pending_link);
    }
    return connection;
}

/* 미완성이면 offset을 그대로 둬서 다음 recv까지 기다리게 한다. */
static connection_t * run_next_command (store_t * store,
                                        connection_t * connection,
                                        size_t * offset) {
    int argc = 0;
    command_arg_t args[RESP_MAX_ARGS];
    size_t consumed = 0;
    parse_result_t result = resp_parse(connection->input + *offset,
                                       connection->input_length - *offset,
                                       &argc, args, &consumed);
    if (result == PARSE_INCOMPLETE) {
        return connection;
    }

    int64_t now_ms = read_now_ms();
    reply_t reply;
    if (result == PARSE_DONE) {
        command_execute(store, argc, args, now_ms, &reply);
        *offset += consumed;
    } else {
        resp_fill_error_reply(result, &reply);
        connection->close_reason = CLOSE_AFTER_REPLY;
        *offset = connection->input_length;
    }
    return append_reply_with_purge(store, connection, &reply, argc, args,
                                   now_ms);
}

/* 만료는 상대 시각(지금 + 초 × 1000)이라, 시스템 시계를 바꿔도
 * 흔들리지 않는 MONOTONIC을 쓴다. 0 이상이다 (command.c 요구). */
static int64_t read_now_ms (void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static connection_t * append_reply_with_purge (store_t * store,
                                               connection_t * connection,
                                               reply_t * reply, int argc,
                                               const command_arg_t * args,
                                               int64_t now_ms) {
    if (is_output_limit_exceeded(connection, reply)) {
        if (flush_output(connection) == NULL) {
            return NULL;
        }
        if (is_output_limit_exceeded(connection, reply)) {
            connection->close_reason = CLOSE_AFTER_OUTPUT_LIMIT;
            return connection;
        }
    }
    if (append_reply(connection, reply) != NULL) {
        return connection;
    }

    store_purge_expired(store, now_ms);
    /* purge 전의 값 포인터 대신 같은 시각에 GET으로 다시 빌린다. */
    if (reply->kind == REPLY_BULK) {
        command_execute(store, argc, args, now_ms, reply);
    }
    return append_reply(connection, reply);
}

static int is_output_limit_exceeded (const connection_t * connection,
                                     const reply_t * reply) {
    size_t unsent = connection->output_length - connection->output_sent;
    return resp_reply_size(reply) > OUTPUT_LIMIT - unsent;
}

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

/* 앞선 응답을 다 보낸 뒤에 한도 오류를 붙여 RESP 응답 순서를 지킨다. */
static connection_t * flush_output (connection_t * connection) {
    if (has_unsent_output(connection)
        && send_bytes(connection, connection->output, connection->output_length,
                      &connection->output_sent) == NULL) {
        return NULL;
    }
    if (has_unsent_output(connection)) {
        return connection;
    }
    connection->output_length = 0;
    connection->output_sent = 0;
    if (connection->close_reason == CLOSE_AFTER_OUTPUT_LIMIT) {
        return send_bytes(connection, OUTPUT_LIMIT_ERROR,
                          sizeof(OUTPUT_LIMIT_ERROR) - 1,
                          &connection->output_error_sent);
    }
    return connection;
}

/* EAGAIN이면 offset부터 다음에 이어 보낸다. 치명적 오류는 NULL. */
static connection_t * send_bytes (connection_t * connection, const char * data,
                                  size_t length, size_t * offset) {
    while (*offset < length) {
        ssize_t sent = send(connection->fd, data + *offset,
                            length - *offset, MSG_NOSIGNAL);
        if (sent > 0) {
            *offset += (size_t) sent;
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
    return connection;
}

static int has_unsent_output (const connection_t * connection) {
    return connection->output_sent < connection->output_length;
}

static int is_done_closing (const connection_t * connection) {
    if (connection->close_reason == CLOSE_AFTER_OUTPUT_LIMIT) {
        return connection->output_error_sent == sizeof(OUTPUT_LIMIT_ERROR) - 1;
    }
    return connection->close_reason == CLOSE_AFTER_REPLY
        && !has_unsent_output(connection);
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
>>>>>>> 96db220 (feat: server.c)
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

<<<<<<< HEAD
static buffer_t * encode_reply (buffer_t * output, command_reply_t reply) {
    switch (reply.kind) {
    case COMMAND_REPLY_OK:
        return copy_reply(output, "+OK\r\n");
    case COMMAND_REPLY_NIL:
        return copy_reply(output, "$-1\r\n");
    case COMMAND_REPLY_BULK: {
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "$%zu\r\n", reply.length);
        return encode_frame(output, prefix, reply);
=======
/* 닫는 중엔 EPOLLIN을 뺀다. 안 읽는 입력 때문에 level-triggered 알림이
 * 끝없이 와서 CPU를 다 쓰지 않게 한다. */
static uint32_t decide_watch_events (const connection_t * connection) {
    if (connection->close_reason != CLOSE_NONE) {
        return EPOLLOUT;
>>>>>>> 96db220 (feat: server.c)
    }
    case COMMAND_REPLY_ERROR:
        return encode_frame(output, "-", reply);
    case COMMAND_REPLY_OUT_OF_MEMORY:
        return copy_reply(output, "-ERR 메모리가 부족합니다\r\n");
    }
    return NULL;
}

static buffer_t * encode_frame (buffer_t * output, const char * prefix,
                                command_reply_t reply) {
    size_t prefix_length = strlen(prefix);
    size_t length = prefix_length + reply.length + 2;
    if (reserve_buffer(output, length) == NULL) {
        return NULL;
    }
    memcpy(output->data, prefix, prefix_length);
    memcpy(output->data + prefix_length, reply.data, reply.length);
    memcpy(output->data + length - 2, "\r\n", 2);
    output->length = length;
    return output;
}

static void read_client (int poller, client_t * client, store_t * store) {
    char bytes[READ_BUFFER_SIZE];
    ssize_t count = recv(client->descriptor, bytes, sizeof(bytes), 0);
    if (count < 0 && is_retryable_io_error()) {
        return;
    }
    if (count <= 0) {
        destroy_client(client);
        return;
    }

    buffer_t * input = &client->input;
    if ((size_t)count > SIZE_MAX - input->length ||
        reserve_buffer(input, input->length + (size_t)count) == NULL) {
        destroy_client(client);
        return;
    }
    memcpy(input->data + input->length, bytes, (size_t)count);
    input->length += (size_t)count;
    process_request(poller, client, store);
}

static void stop_server (server_t * server) {
    while (!is_list_empty(&server->idle)) {
        close_connection(server, server->idle.next->connection);
    }
    close(server->listen_fd);
    close(server->epoll_fd);
    store_free(server->store);
}

static void initialize_list (connection_link_t * list) {
    list->next = list;
    list->previous = list;
    list->connection = NULL;
}

static int is_list_empty (const connection_link_t * list) {
    return list->next == list;
}

static void push_back_link (connection_link_t * list,
                            connection_link_t * link) {
    link->previous = list->previous;
    link->next = list;
    list->previous->next = link;
    list->previous = link;
}

static void remove_link (connection_link_t * link) {
    if (link->next == NULL) {
        return;
    }
    link->previous->next = link->next;
    link->next->previous = link->previous;
    link->previous = NULL;
    link->next = NULL;
}
