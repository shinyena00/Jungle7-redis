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
 *             ├─ handle_ready_event
 *             │   ├─ 받는 소켓: accept_connections → register_connection
 *             │   │              fd가 꽉 차면 pause_accepting
 *             │   └─ 연결 소켓: handle_connection_event → serve_connection
 *             │       ├─ receive_and_run   완성된 명령이 없을 때만 recv 한 번
 *             │       │   └─ run_commands  한 바퀴 최대 100개
 *             │       │       resp_parse → command_execute
 *             │       │       → append_reply (바로 다음에)
 *             │       │       → 출력 4MB 넘으면 send 한 번 → 그래도 넘으면 끊기
 *             │       ├─ flush_output      보낼 수 있는 만큼 send
 *             │       ├─ touch_idle        활동이 있었으면 유휴 목록 맨 뒤로
 *             │       ├─ push_pending      완성된 명령이 남았으면 대기 목록
 *             │       └─ update_watch      EPOLLIN / EPOLLOUT 켜고 끄기
 *             ├─ run_pending_connections  지난 바퀴 대기 목록 → serve_connection
 *             └─ close_idle_connections   1초마다, 120초 조용한 연결 닫기
 *
 * 공정성 (11/14 결정 다): 한 연결은 한 바퀴에 명령을 최대 100개만 처리한다.
 * 남은 명령이 있으면 recv하지 않고 대기 목록에 넣는다. 그래서 입력 버퍼는
 * "명령 하나 최대 + recv 한 번"을 넘지 않고, 그동안 TCP가 클라이언트를
 * 늦춘다. 대기 목록이 비어 있지 않으면 epoll_wait는 기다리지 않는다.
 *
 * 버퍼 안을 가리키는 포인터 두 가지 때문에 순서가 정해져 있다.
 *  - args는 입력 버퍼 안을 가리킨다 → 입력 앞당기기는 명령을 다 처리한 뒤
 *  - BULK 응답은 store 안을 가리킨다 → command_execute 바로 다음에
 *    다른 store_* 호출 없이 출력 버퍼에 쓴다
 *    (예외: 출력 버퍼를 늘리다 실패하면 같은 now_ms로 purge 한 번. 🔒)
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

/* 한 연결이 한 바퀴에 처리하는 최대 명령 수. 명령을 많이 붙여 보내는
 * 클라이언트가 다른 클라이언트를 기다리게 하지 않게 한다. (11/14 결정)
 * 확인용으로 빌드할 때 바꿀 수 있다: -DCOMMANDS_PER_ROUND=2 */
#ifndef COMMANDS_PER_ROUND
#define COMMANDS_PER_ROUND   100
#endif

/* 안 보낸 응답이 이보다 많으면 끊는다. 응답을 안 읽어서 쌓인 것이라
 * 기다려도 안 닿는다. (11/14 결정) 확인용: -DOUTPUT_LIMIT=... */
#ifndef OUTPUT_LIMIT
#define OUTPUT_LIMIT         (4 * 1024 * 1024)
#endif

/* 이 시간 동안 활동(accept, recv 1바이트 이상, send 1바이트 이상)이
 * 없으면 끊는다. (11/14 결정) 확인용: -DIDLE_TIMEOUT_MS=3000 */
#ifndef IDLE_TIMEOUT_MS
#define IDLE_TIMEOUT_MS      120000
#endif
#define IDLE_CHECK_MS        1000

/* 4. 규칙 에러 문구 표에 추가할 문구. server.c에만 있다. (11/14 결정) */
#define ERROR_OUTPUT_OVERFLOW "응답이 너무 많이 쌓였습니다"

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

    /* 출력이 4MB를 넘었다. send를 한 번 시도한 뒤 바로 닫는다. */
    int is_overflowed;

    /* 지금 epoll에 걸어 둔 조건. 같으면 epoll_ctl을 부르지 않는다. */
    uint32_t watched_events;

    /* 대기 목록: 100개를 채우고도 완성된 명령이 남은 연결.
     * 닫을 때 중간에서 바로 빼려고 앞뒤를 다 들고 있다. */
    int is_pending;
    uint64_t pending_round;   /* 대기 목록에 들어간 바퀴 */
    struct connection * pending_prev;
    struct connection * pending_next;

    /* 유휴 목록: 마지막 활동 순서. 맨 앞이 가장 오래 조용한 연결이다.
     * recv·send가 1바이트 이상이면 has_activity를 켜고, serve_connection이
     * 끝날 때 맨 뒤로 옮긴다. */
    int has_activity;
    int64_t last_active_ms;
    struct connection * idle_prev;
    struct connection * idle_next;
} connection_t;

typedef struct {
    int listen_fd;
    int epoll_fd;
    store_t * store;

    /* 들어간 순서대로 꺼낸다 (먼저 기다린 연결부터) */
    connection_t * pending_head;
    connection_t * pending_tail;

    /* epoll_wait 한 번 = 한 바퀴. 이번 바퀴에 들어간 연결은 다음 바퀴에
     * 처리해서, 한 연결이 한 바퀴에 100개를 넘게 처리하지 않게 한다. */
    uint64_t round;

    connection_t * idle_head;
    connection_t * idle_tail;
    int64_t last_idle_check_ms;

    /* fd가 꽉 차서 받는 소켓을 감시 목록에서 뺐다. 연결이 닫히면 다시 넣는다. */
    int is_accept_paused;
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
static int64_t read_now_ms (void);
static void push_idle (server_t * server, connection_t * connection,
                       int64_t now_ms);
static int is_transient_accept_error (int error);
static int is_out_of_resources (int error);
static void pause_accepting (server_t * server);
static int is_would_block (int error);
static void handle_connection_event (server_t * server,
                                     connection_t * connection,
                                     uint32_t events);
static void close_connection (server_t * server, connection_t * connection);
static void remove_pending (server_t * server, connection_t * connection);
static void remove_idle (server_t * server, connection_t * connection);
static void resume_accepting (server_t * server);
static void serve_connection (server_t * server, connection_t * connection,
                              int should_run);
static connection_t * receive_and_run (store_t * store,
                                       connection_t * connection);
static int has_complete_command (const connection_t * connection);
static ssize_t receive_once (store_t * store, connection_t * connection);
static char * ensure_input_space (store_t * store, connection_t * connection,
                                  size_t size);
static char * grow_buffer_or_purge (store_t * store, int64_t now_ms,
                                    char ** buffer, size_t * capacity,
                                    size_t needed);
static char * grow_buffer (char ** buffer, size_t * capacity, size_t needed);
static int is_connection_lost (ssize_t received);
static connection_t * run_commands (store_t * store,
                                    connection_t * connection);
static connection_t * append_reply (store_t * store, int64_t now_ms,
                                    connection_t * connection,
                                    const reply_t * reply);
static char * ensure_output_space (store_t * store, int64_t now_ms,
                                   connection_t * connection, size_t size);
static void discard_sent_output (connection_t * connection);
static connection_t * flush_if_over_limit (connection_t * connection);
static int is_output_over_limit (const connection_t * connection);
static connection_t * flush_output (connection_t * connection);
static int has_unsent_output (const connection_t * connection);
static void mark_overflowed (store_t * store, int64_t now_ms,
                             connection_t * connection);
static void compact_input (connection_t * connection, size_t offset);
static int is_done_closing (const connection_t * connection);
static void touch_idle (server_t * server, connection_t * connection);
static void push_pending (server_t * server, connection_t * connection);
static int update_watch (int epoll_fd, connection_t * connection);
static uint32_t decide_watch_events (const connection_t * connection);
static void run_pending_connections (server_t * server);
static void close_idle_connections (server_t * server);

/* ── main ──────────────────────────────────────────────────── */

int main (void) {
    struct sockaddr_in address;
    if (read_listen_address(&address) == NULL) {
        return EXIT_FAILURE;
    }
    ignore_sigpipe();

    server_t server;
    if (start_server(&server, &address) == NULL) {
        return EXIT_FAILURE;
    }
    print_listening(&address);

    /* epoll_wait가 복구할 수 없게 실패할 때만 돌아온다. */
    run_event_loop(&server);
    return EXIT_FAILURE;
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

    memset(server, 0, sizeof(*server));
    server->listen_fd = listen_fd;
    server->epoll_fd = epoll_fd;
    server->store = store;
    return server;
}

/* 반환: 받는 소켓 fd. 실패하면 이유를 출력하고 -1 (fd라 NULL을 쓸 수 없음) */
static int create_listen_socket (const struct sockaddr_in * address) {
    /* 스레드가 하나라 accept에서 멈추면 모든 클라이언트가 같이 멈춘다. */
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    // send가 없으면 바로 돌아와야하기 때문에 NONBLOCK으로 설정
    if (listen_fd == -1) {
        perror("socket");
        return -1;
    }

    /* 재시작 직후 TIME_WAIT 때문에 bind가 실패하지 않게 한다. */
    int enabled = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, // time_wait에 걸렸을 때 새 서버를 열어서 bind가 되게 함
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

/* 대기 목록에 남은 연결이 있으면 할 일이 남은 것이라 epoll_wait에서
 * 자지 않는다 (대기 시간 0). */
static void run_event_loop (server_t * server) {
    struct epoll_event ready[READY_EVENT_CAPACITY];
    for (;;) {
        server->round++;
        int wait_ms = server->pending_head != NULL ? 0 : EVENT_WAIT_MS;
        int count = epoll_wait(server->epoll_fd, ready,
                               READY_EVENT_CAPACITY, wait_ms);
        if (count == -1 && errno != EINTR) {
            perror("epoll_wait");
            return;
        }
        for (int i = 0; i < count; i++) {
            handle_ready_event(server, &ready[i]);
        }
        run_pending_connections(server);
        close_idle_connections(server);
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

/* level-triggered라 줄 선 접속을 EAGAIN이 나올 때까지 다 받는다. */
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
        if (is_out_of_resources(errno)) {
            pause_accepting(server);
            return;
        }
        if (!is_would_block(errno)) {
            perror("accept4");
        }
        return;
    }
}

/* fd: 넘겨받는다. 실패하면 여기서 닫는다.
 * 받은 순간도 활동이라 유휴 목록 맨 뒤에 넣는다. */
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
    push_idle(server, connection, read_now_ms());
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

/* epoll 등록·목록은 건드리지 않는다. 등록된 연결은 close_connection으로. */
static void destroy_connection (connection_t * connection) {
    close(connection->fd);
    free(connection->input);
    free(connection->output);
    free(connection);
}

/* 만료는 상대 시각(지금 + 초 × 1000)이라, 시스템 시계를 바꿔도
 * 흔들리지 않는 MONOTONIC을 쓴다. 0 이상이다 (command.c 요구). */
static int64_t read_now_ms (void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* 맨 뒤 = 가장 최근에 활동한 연결 */
static void push_idle (server_t * server, connection_t * connection,
                       int64_t now_ms) {
    connection->last_active_ms = now_ms;
    connection->idle_prev = server->idle_tail;
    connection->idle_next = NULL;
    if (server->idle_tail == NULL) {
        server->idle_head = connection;
    } else {
        server->idle_tail->idle_next = connection;
    }
    server->idle_tail = connection;
}

/* 그 접속 하나만 실패한 경우. 줄에 남은 다음 접속은 계속 받는다. */
static int is_transient_accept_error (int error) {
    return error == ECONNABORTED || error == EINTR || error == EPROTO;
}

/* fd나 메모리가 바닥난 경우. 연결이 하나 닫혀야 다시 받을 수 있다. */
static int is_out_of_resources (int error) {
    return error == EMFILE || error == ENFILE
           || error == ENOBUFS || error == ENOMEM;
}

/* 줄 선 접속은 커널 backlog에서 기다리게 두고 받는 소켓을 감시 목록에서
 * 뺀다. 안 빼면 level-triggered라 줄이 빌 때까지 계속 깨어나서 CPU를
 * 다 쓴다. 연결이 하나 닫히면 resume_accepting이 다시 넣는다. 아무도
 * 안 닫으면 유휴 타임아웃(최대 약 2분)이 자리를 만든다. (11/14 결정) */
static void pause_accepting (server_t * server) {
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, server->listen_fd,
                  NULL) == -1) {
        perror("epoll_ctl(받는 소켓 빼기)");
        return;
    }
    server->is_accept_paused = 1;
    fprintf(stderr, "accept4: %s → 연결이 닫힐 때까지 새 접속을 "
            "받지 않습니다\n", strerror(errno));
}

static int is_would_block (int error) {
    return error == EAGAIN || error == EWOULDBLOCK;
}

/* 대기 목록에 있는 연결은 여기서 명령을 처리하지 않는다. 지난 바퀴에
 * 100개를 채운 연결이라 run_pending_connections가 이번 바퀴에 처리한다. */
static void handle_connection_event (server_t * server,
                                     connection_t * connection,
                                     uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) {
        close_connection(server, connection);
        return;
    }
    int should_run = (events & EPOLLIN) && !connection->is_pending;
    serve_connection(server, connection, should_run);
}

/* 닫는 길을 한 곳에 모아 둔다. 목록에서 빼지 않으면 해제한 연결을
 * 다음 바퀴에 다시 쓰게 된다. fd가 하나 비었으니 멈춘 accept도 다시 연다. */
static void close_connection (server_t * server, connection_t * connection) {
    if (connection->is_pending) {
        remove_pending(server, connection);
    }
    remove_idle(server, connection);
    epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, connection->fd, NULL);
    destroy_connection(connection);
    if (server->is_accept_paused) {
        resume_accepting(server);
    }
}

static void remove_pending (server_t * server, connection_t * connection) {
    if (connection->pending_prev == NULL) {
        server->pending_head = connection->pending_next;
    } else {
        connection->pending_prev->pending_next = connection->pending_next;
    }
    if (connection->pending_next == NULL) {
        server->pending_tail = connection->pending_prev;
    } else {
        connection->pending_next->pending_prev = connection->pending_prev;
    }
    connection->pending_prev = NULL;
    connection->pending_next = NULL;
    connection->is_pending = 0;
}

static void remove_idle (server_t * server, connection_t * connection) {
    if (connection->idle_prev == NULL) {
        server->idle_head = connection->idle_next;
    } else {
        connection->idle_prev->idle_next = connection->idle_next;
    }
    if (connection->idle_next == NULL) {
        server->idle_tail = connection->idle_prev;
    } else {
        connection->idle_next->idle_prev = connection->idle_prev;
    }
    connection->idle_prev = NULL;
    connection->idle_next = NULL;
}

/* 실패하면 멈춘 채로 두고 다음 연결이 닫힐 때 다시 시도한다. */
static void resume_accepting (server_t * server) {
    if (watch_readable(server->epoll_fd, server->listen_fd, NULL) == -1) {
        perror("epoll_ctl(받는 소켓 다시 넣기)");
        return;
    }
    server->is_accept_paused = 0;
}

/* 명령 처리 → 보내기 → 닫을지 판단 → 유휴·대기 목록 → 감시 조건 순서.
 * should_run: 읽기·명령 처리를 할지. 거짓이면 보내기부터 한다
 * (EPOLLOUT만 왔거나, 대기 목록에 있는 연결).
 * 응답은 EPOLLOUT을 기다리지 않고 바로 보내 본다. 못 다 보낸 것만
 * EPOLLOUT으로 이어 보낸다. */
static void serve_connection (server_t * server, connection_t * connection,
                              int should_run) {
    if (should_run && !connection->is_closing
        && receive_and_run(server->store, connection) == NULL) {
        close_connection(server, connection);
        return;
    }
    /* 응답을 안 읽어서 쌓인 것이라 기다려도 안 닿는다. 문구는 원인 기록용 */
    if (connection->is_overflowed) {
        flush_output(connection);
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
    if (connection->has_activity) {
        touch_idle(server, connection);
    }
    /* update_watch가 대기 여부를 보고 EPOLLIN을 끄므로 그보다 먼저 */
    if (!connection->is_closing && has_complete_command(connection)) {
        push_pending(server, connection);
    }
    if (update_watch(server->epoll_fd, connection) == -1) {
        perror("epoll_ctl(감시 조건 변경)");
        close_connection(server, connection);
        return;
    }
}

/* 완성된 명령이 남아 있으면 recv하지 않고 남은 것부터 처리한다.
 * 이벤트 하나에 recv도 한 번 (11/14 결정 다).
 * 반환: 닫아야 하면 NULL (상대가 끊음, recv 에러, 버퍼 메모리 부족) */
static connection_t * receive_and_run (store_t * store,
                                       connection_t * connection) {
    if (!has_complete_command(connection)) {
        ssize_t received = receive_once(store, connection);
        if (is_connection_lost(received)) {
            return NULL;
        }
        if (received < 0) {
            return connection;
        }
    }
    return run_commands(store, connection);
}

/* 형식 에러·너무 큼도 참이다. 에러를 답해야 하는 "처리할 것"이라서. */
static int has_complete_command (const connection_t * connection) {
    if (connection->input_length == 0) {
        return 0;
    }
    int argc = 0;
    command_arg_t args[RESP_MAX_ARGS];
    size_t consumed = 0;
    return resp_parse(connection->input, connection->input_length,
                      &argc, args, &consumed) != PARSE_INCOMPLETE;
}

/* 반환: recv 그대로. 입력 버퍼를 못 늘리면 errno = ENOMEM으로 -1 */
static ssize_t receive_once (store_t * store, connection_t * connection) {
    char * space = ensure_input_space(store, connection, RECV_CHUNK);
    if (space == NULL) {
        errno = ENOMEM;
        return -1;
    }
    ssize_t received = recv(connection->fd, space, RECV_CHUNK, 0);
    if (received > 0) {
        connection->input_length += (size_t) received;
        connection->has_activity = 1;
    }
    return received;
}

/* 아직 해석한 명령도 응답도 없어서 purge가 지워도 다칠 포인터가 없다.
 * 그래서 지금 시각을 새로 읽어 쓴다. (11/14 결정 a)
 * 반환: 입력 끝(쓸 자리). 못 늘리면 NULL (입력 버퍼는 그대로) */
static char * ensure_input_space (store_t * store, connection_t * connection,
                                  size_t size) {
    size_t needed = connection->input_length + size;
    if (needed > connection->input_capacity
        && grow_buffer_or_purge(store, read_now_ms(), &connection->input,
                                &connection->input_capacity,
                                needed) == NULL) {
        return NULL;
    }
    return connection->input + connection->input_length;
}

/* 늘리다 실패하면 만료 키를 한꺼번에 지우고 한 번만 다시 시도한다.
 * 지운 게 없으면 다시 시도해도 같으니 바로 실패한다. (11/14 결정)
 * 반환: 새 버퍼. 실패하면 NULL (*buffer·*capacity는 그대로) */
static char * grow_buffer_or_purge (store_t * store, int64_t now_ms,
                                    char ** buffer, size_t * capacity,
                                    size_t needed) {
    if (grow_buffer(buffer, capacity, needed) != NULL) {
        return *buffer;
    }
    if (!store_purge_expired(store, now_ms)) {
        return NULL;
    }
    return grow_buffer(buffer, capacity, needed);
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

/* 입력 버퍼의 완성된 명령을 앞에서부터 최대 COMMANDS_PER_ROUND개 처리한다.
 * 남은 명령은 대기 목록으로 다음 바퀴에 처리한다 (serve_connection).
 * 형식 에러·너무 큼이면 에러를 답하고 닫는 중으로 바꾼다. 그 뒤의 입력은
 * 읽지 않고 버린다. (E24, E31)
 * 출력이 4MB를 넘으면 그 자리에서 멈추고 끊을 준비를 한다.
 * 반환: 닫아야 하면 NULL (출력 버퍼 메모리 부족, send 에러) */
static connection_t * run_commands (store_t * store,
                                    connection_t * connection) {
    size_t offset = 0;
    for (int executed = 0; executed < COMMANDS_PER_ROUND
                           && !connection->is_closing; executed++) {
        int argc = 0;
        command_arg_t args[RESP_MAX_ARGS];
        size_t consumed = 0;
        parse_result_t result = resp_parse(connection->input + offset,
                                           connection->input_length - offset,
                                           &argc, args, &consumed);
        if (result == PARSE_INCOMPLETE) {
            break;
        }

        /* 🔒 조건 1: 출력 버퍼 purge에 이 값을 그대로 넘긴다 */
        int64_t now_ms = read_now_ms();
        reply_t reply;
        if (result == PARSE_DONE) {
            command_execute(store, argc, args, now_ms, &reply);
            offset += consumed;
        } else {
            resp_fill_error_reply(result, &reply);
            connection->is_closing = 1;
            offset = connection->input_length;
        }
        if (append_reply(store, now_ms, connection, &reply) == NULL) {
            return NULL;
        }
        if (flush_if_over_limit(connection) == NULL) {
            return NULL;
        }
        if (is_output_over_limit(connection)) {
            mark_overflowed(store, now_ms, connection);
            return connection;
        }
    }
    compact_input(connection, offset);
    return connection;
}

/* reply가 BULK면 store 안을 가리키므로 command_execute 바로 다음에,
 * 다른 store_* 호출 없이 부른다. 반환: 출력 버퍼를 못 늘리면 NULL */
static connection_t * append_reply (store_t * store, int64_t now_ms,
                                    connection_t * connection,
                                    const reply_t * reply) {
    size_t size = resp_reply_size(reply);
    char * space = ensure_output_space(store, now_ms, connection, size);
    if (space == NULL) {
        return NULL;
    }
    connection->output_length += resp_write_reply(reply, space);
    return connection;
}

/* 자리가 모자라면 이미 보낸 앞부분부터 비우고, 그래도 모자라면 늘린다.
 * now_ms: command_execute에 넘긴 값 그대로. 방금 GET한 값은 이 시각에
 * 만료가 아니라 purge가 지우지 않고(🔒 조건 1), purge는 남는 값의 주소를
 * 바꾸지 않는다(🔒 조건 2, store.c). 그래서 BULK가 계속 유효하다.
 * 반환: 출력 끝(쓸 자리). 못 늘리면 NULL (안 보낸 응답은 그대로) */
static char * ensure_output_space (store_t * store, int64_t now_ms,
                                   connection_t * connection, size_t size) {
    if (connection->output_length + size > connection->output_capacity) {
        discard_sent_output(connection);
    }
    size_t needed = connection->output_length + size;
    if (needed > connection->output_capacity
        && grow_buffer_or_purge(store, now_ms, &connection->output,
                                &connection->output_capacity,
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

/* 명령 100개를 다 처리한 뒤에야 send하므로, 잘 읽는 클라이언트도
 * 1MB GET 몇 개를 붙여 보내면 잠깐 4MB를 넘는다. 끊기 전에 한 번
 * 보내 보고, 그래도 넘을 때만 "안 읽어서 쌓인 것"으로 본다.
 * 반환: send 에러면 NULL */
static connection_t * flush_if_over_limit (connection_t * connection) {
    if (!is_output_over_limit(connection)) {
        return connection;
    }
    return flush_output(connection);
}

static int is_output_over_limit (const connection_t * connection) {
    return connection->output_length - connection->output_sent
           > OUTPUT_LIMIT;
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
            connection->has_activity = 1;
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

/* 에러 문구를 못 붙여도(메모리 부족) 끊는 건 그대로 한다. */
static void mark_overflowed (store_t * store, int64_t now_ms,
                             connection_t * connection) {
    reply_t reply;
    reply.kind = REPLY_ERROR;
    reply.data = ERROR_OUTPUT_OVERFLOW;
    reply.length = strlen(ERROR_OUTPUT_OVERFLOW);
    append_reply(store, now_ms, connection, &reply);
    connection->is_overflowed = 1;
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

/* 에러 응답까지 다 보냈으면 참 ("에러를 답한 뒤 끊는다", resp.h) */
static int is_done_closing (const connection_t * connection) {
    return connection->is_closing && !has_unsent_output(connection);
}

/* 활동이 있었던 연결을 유휴 목록 맨 뒤로 옮긴다. EAGAIN만 난 send는
 * 활동이 아니다 (응답을 안 읽는 연결도 결국 타임아웃으로 정리되게). */
static void touch_idle (server_t * server, connection_t * connection) {
    remove_idle(server, connection);
    push_idle(server, connection, read_now_ms());
    connection->has_activity = 0;
}

/* 맨 뒤에 넣는다. 이번 바퀴 번호를 적어 두어 이번 바퀴에는 다시
 * 처리되지 않게 한다 (run_pending_connections). */
static void push_pending (server_t * server, connection_t * connection) {
    if (connection->is_pending) {
        return;
    }
    connection->pending_round = server->round;
    connection->pending_prev = server->pending_tail;
    connection->pending_next = NULL;
    if (server->pending_tail == NULL) {
        server->pending_head = connection;
    } else {
        server->pending_tail->pending_next = connection;
    }
    server->pending_tail = connection;
    connection->is_pending = 1;
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

/* 닫는 중·대기 중엔 EPOLLIN을 뺀다. 어차피 recv하지 않는데, 켜 두면
 * 커널에 남은 입력 때문에 level-triggered 알림이 계속 온다.
 * 0(아무것도 안 봄)이어도 EPOLLERR·EPOLLHUP은 커널이 알려준다. */
static uint32_t decide_watch_events (const connection_t * connection) {
    if (connection->is_closing) {
        return EPOLLOUT;
    }
    uint32_t events = connection->is_pending ? 0 : EPOLLIN;
    if (has_unsent_output(connection)) {
        events |= EPOLLOUT;
    }
    return events;
}

/* 지난 바퀴까지 들어온 연결만 처리한다. 이번 바퀴에 들어간 연결(방금
 * 100개를 처리한 연결)은 pending_round가 이번 바퀴라서 다음 바퀴로 넘어간다.
 * 들어간 순서대로라 그런 연결이 나오면 뒤는 전부 이번 바퀴 것이다. */
static void run_pending_connections (server_t * server) {
    while (server->pending_head != NULL
           && server->pending_head->pending_round < server->round) {
        connection_t * connection = server->pending_head;
        remove_pending(server, connection);
        serve_connection(server, connection, 1);
    }
}

/* 유휴 목록은 마지막 활동 순서라, 맨 앞부터 보다가 안 오래된 연결이
 * 나오면 뒤는 전부 더 최근 것이다. 그래서 전체를 훑지 않는다.
 * 대기 목록 때문에 epoll_wait가 0ms로 돌 때도 1초에 한 번만 본다. */
static void close_idle_connections (server_t * server) {
    int64_t now_ms = read_now_ms();
    if (now_ms - server->last_idle_check_ms < IDLE_CHECK_MS) {
        return;
    }
    server->last_idle_check_ms = now_ms;
    while (server->idle_head != NULL
           && now_ms - server->idle_head->last_active_ms >= IDLE_TIMEOUT_MS) {
        close_connection(server, server->idle_head);
    }
}
