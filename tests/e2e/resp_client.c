/* resp_client.c — 외부 테스트용 RESP 클라이언트 구현 (테스트 도우미) */
#include "resp_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int port_from_env(void) {
    const char *p = getenv("MINI_REDIS_PORT");
    return p ? atoi(p) : 6380;
}

int conn_open(conn_t *c) {
    memset(c, 0, sizeof *c);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port_from_env());
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(c->fd);
        c->fd = -1;
        return -1;
    }
    int one = 1; /* 작은 조각을 모으지 말고 바로 보내기 (1바이트씩 보내기 테스트용) */
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return 0;
}

void conn_close(conn_t *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    free(c->buf);
    c->buf = NULL;
    c->len = c->cap = 0;
}

int send_raw(conn_t *c, const void *data, size_t len) {
    const char *p = data;
    while (len > 0) {
        ssize_t n = send(c->fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

size_t build_cmd(char *out, size_t cap, int argc, const char *const *argv,
                 const size_t *lens) {
    size_t pos = (size_t)snprintf(out, cap, "*%d\r\n", argc);
    for (int i = 0; i < argc; i++) {
        pos += (size_t)snprintf(out + pos, cap - pos, "$%zu\r\n", lens[i]);
        memcpy(out + pos, argv[i], lens[i]);
        pos += lens[i];
        memcpy(out + pos, "\r\n", 2);
        pos += 2;
    }
    return pos;
}

int send_cmd(conn_t *c, int argc, const char *const *argv, const size_t *lens) {
    size_t cap = 64;
    for (int i = 0; i < argc; i++) cap += lens[i] + 32;
    char *buf = malloc(cap);
    if (!buf) return -1;
    size_t n = build_cmd(buf, cap, argc, argv, lens);
    int r = send_raw(c, buf, n);
    free(buf);
    return r;
}

int send_cmdv(conn_t *c, ...) {
    const char *argv[16];
    size_t lens[16];
    int argc = 0;
    va_list ap;
    va_start(ap, c);
    const char *s;
    while ((s = va_arg(ap, const char *)) != NULL && argc < 16) {
        argv[argc] = s;
        lens[argc] = strlen(s);
        argc++;
    }
    va_end(ap);
    return send_cmd(c, argc, argv, lens);
}

/* buf 앞에 응답 하나가 다 왔으면 그 길이, 덜 왔으면 0, 모양이 이상하면 -1 */
static long full_reply_len(const char *buf, size_t len) {
    if (len < 3) return 0;
    const char *crlf = NULL;
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            crlf = buf + i;
            break;
        }
    }
    if (!crlf) return 0;
    long header = (long)(crlf - buf) + 2;
    switch (buf[0]) {
    case '+': case '-': case ':':
        return header;
    case '$': {
        long n = strtol(buf + 1, NULL, 10);
        if (n < 0) return header;              /* $-1\r\n */
        long total = header + n + 2;
        return (long)len >= total ? total : 0;
    }
    default:
        return -1;                             /* 조각 1 서버가 보낼 일 없는 모양 */
    }
}

static int fill(conn_t *c, int timeout_ms) {
    struct pollfd pfd = {c->fd, POLLIN, 0};
    int r = poll(&pfd, 1, timeout_ms);
    if (r <= 0) return -1;                     /* 시간 초과 */
    if (c->cap - c->len < 65536) {
        size_t ncap = c->cap ? c->cap * 2 : 131072;
        while (ncap - c->len < 65536) ncap *= 2;
        char *nb = realloc(c->buf, ncap);
        if (!nb) return -1;
        c->buf = nb;
        c->cap = ncap;
    }
    ssize_t n = recv(c->fd, c->buf + c->len, c->cap - c->len, 0);
    if (n == 0) return 0;                      /* 닫힘 */
    if (n < 0) return (errno == ECONNRESET) ? 0 : -1;
    c->len += (size_t)n;
    return 1;
}

int read_reply(conn_t *c, char **out, size_t *outlen, int timeout_ms) {
    for (;;) {
        long n = full_reply_len(c->buf ? c->buf : "", c->len);
        if (n < 0) return -1;
        if (n > 0) {
            *out = malloc((size_t)n + 1);
            memcpy(*out, c->buf, (size_t)n);
            (*out)[n] = '\0';
            *outlen = (size_t)n;
            memmove(c->buf, c->buf + n, c->len - (size_t)n);
            c->len -= (size_t)n;
            return 1;
        }
        int r = fill(c, timeout_ms);
        if (r == 0) return c->len == 0 ? 0 : -1;
        if (r < 0) return -1;
    }
}

static void show(const char *label, const char *s, size_t n) {
    fprintf(stderr, "      %s: \"", label);
    for (size_t i = 0; i < n && i < 120; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\r') fputs("\\r", stderr);
        else if (ch == '\n') fputs("\\n", stderr);
        else if (ch == 0) fputs("\\0", stderr);
        else fputc(ch, stderr);
    }
    fprintf(stderr, "%s\"\n", n > 120 ? "..." : "");
}

int expect_reply(conn_t *c, const char *want, size_t wantlen) {
    char *got = NULL;
    size_t glen = 0;
    int r = read_reply(c, &got, &glen, 2000);
    if (r != 1) {
        show("want", want, wantlen);
        fprintf(stderr, "      got : %s\n", r == 0 ? "(연결 닫힘)" : "(시간 초과/오류)");
        return 0;
    }
    int ok = glen == wantlen && memcmp(got, want, wantlen) == 0;
    if (!ok) {
        show("want", want, wantlen);
        show("got ", got, glen);
    }
    free(got);
    return ok;
}

int expect_bulk(conn_t *c, const char *val, size_t vlen) {
    char head[32];
    int h = snprintf(head, sizeof head, "$%zu\r\n", vlen);
    size_t total = (size_t)h + vlen + 2;
    char *want = malloc(total);
    memcpy(want, head, (size_t)h);
    memcpy(want + h, val, vlen);
    memcpy(want + h + vlen, "\r\n", 2);
    int ok = expect_reply(c, want, total);
    free(want);
    return ok;
}

int wait_closed(conn_t *c, int timeout_ms) {
    if (c->len > 0) return 0;                  /* 아직 안 읽은 응답이 있음 */
    int r = fill(c, timeout_ms);
    return r == 0;
}

void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}
