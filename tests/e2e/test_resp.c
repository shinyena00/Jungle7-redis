/*
 * test_resp.c — 조각 1 외부 테스트 (E1~E32, 노션 5-1)
 *
 * 띄워 둔 미니 Redis 서버에 소켓으로 RESP를 보내고, 응답을 바이트 단위로 비교한다.
 * 키는 테스트마다 "t<pid>:<케이스>:" 접두어를 붙여서 테스트끼리 서로 안 섞이게 한다.
 *
 * ⚠️ AI 초안. 사람이 한 줄씩 검토하고, 기대값이 노션 4번 규칙과 맞는지 확인할 것.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../check.h"
#include "resp_client.h"

/* ---------- 기대 응답 (노션 4번 규칙 그대로) ---------- */
#define RE_OK      "+OK\r\n"
#define RE_NIL     "$-1\r\n"
#define E_NOKEY   "-ERR 없는 키입니다\r\n"
#define E_BADVAL  "-ERR 잘못된 값입니다\r\n"
#define E_NEEDEX  "-ERR 보관 기간이 필요합니다\r\n"
#define E_UNKNOWN "-ERR 모르는 명령입니다\r\n"
#define E_EMPTY   "-ERR 빈 키입니다\r\n"
#define E_TOOBIG  "-ERR 너무 큽니다\r\n"
#define E_FORMAT  "-ERR 잘못된 요청 형식입니다\r\n"

#define MAX_KEY 256
#define MAX_VAL 1048576

#define EXPECT(c, lit) CHECK(expect_reply((c), (lit), sizeof(lit) - 1))
#define EXPECT_BULK(c, s) CHECK(expect_bulk((c), (s), strlen(s)))
#define SEND(c, ...) CHECK(send_cmdv((c), __VA_ARGS__, NULL) == 0)

/* 만료 테스트: EX 1 → 1초 뒤 + 여유 */
#define AFTER_1S 1100

/* 케이스별 고유 키 */
static void key(char *out, const char *tcase, const char *name) {
    snprintf(out, 128, "t%d:%s:%s", (int)getpid(), tcase, name);
}

static void open_or_fail(conn_t *c) {
    if (conn_open(c) != 0) {
        fprintf(stderr, "    서버에 연결할 수 없음\n");
        CHECK(0);
    }
}

/* ---------- 기본 명령 ---------- */

/* E1: SET → +OK, GET → 값 (R1, R4) */
static void e01_set_get(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e01", "k");
    SEND(&c, "SET", k, "v", "EX", "10");
    EXPECT(&c, RE_OK);
    SEND(&c, "GET", k);
    EXPECT_BULK(&c, "v");
    conn_close(&c);
}

/* E2: 없는 키 GET → nil (R1) */
static void e02_get_missing(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e02", "nope");
    SEND(&c, "GET", k);
    EXPECT(&c, RE_NIL);
    conn_close(&c);
}

/* E3: 있는 키 DEL → +OK, 이어서 GET → nil (R3) */
static void e03_del_existing(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e03", "k");
    SEND(&c, "SET", k, "v", "EX", "10");
    EXPECT(&c, RE_OK);
    SEND(&c, "DEL", k);
    EXPECT(&c, RE_OK);
    SEND(&c, "GET", k);
    EXPECT(&c, RE_NIL);
    conn_close(&c);
}

/* E4: 없는 키 DEL → 없는 키입니다 (R3) */
static void e04_del_missing(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e04", "nope");
    SEND(&c, "DEL", k);
    EXPECT(&c, E_NOKEY);
    conn_close(&c);
}

/* E5: 같은 DEL 두 번 → +OK, 그다음 없는 키입니다 (R3, R5) */
static void e05_del_twice(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e05", "k");
    SEND(&c, "SET", k, "v", "EX", "10");
    EXPECT(&c, RE_OK);
    SEND(&c, "DEL", k);
    EXPECT(&c, RE_OK);
    SEND(&c, "DEL", k);
    EXPECT(&c, E_NOKEY);
    conn_close(&c);
}

/* ---------- 보관 기간 ---------- */

/* E6: EX 없는 SET → 보관 기간이 필요합니다, 원래 값 그대로 (R2) */
static void e06_set_without_ex(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e06", "k");
    SEND(&c, "SET", k, "orig", "EX", "100");
    EXPECT(&c, RE_OK);
    SEND(&c, "SET", k, "new");
    EXPECT(&c, E_NEEDEX);
    SEND(&c, "GET", k);
    EXPECT_BULK(&c, "orig");
    conn_close(&c);
}

/* E7: 잘못된 기간 → 잘못된 값입니다, 원래 값 그대로 (R2)
 *   -1이 아닌 0·음수, 정수가 아닌 것, 넘치는 큰 수 */
static void e07_bad_ex_values(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e07", "k");
    SEND(&c, "SET", k, "orig", "EX", "100");
    EXPECT(&c, RE_OK);
    const char *bad[] = {
        "0", "-2", "-01", "1abc", "1.5",
        "99999999999999999999",      /* 넘치는 큰 수 */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        SEND(&c, "SET", k, "new", "EX", bad[i]);
        if (!expect_reply(&c, E_BADVAL, sizeof(E_BADVAL) - 1)) {
            fprintf(stderr, "      (EX %s)\n", bad[i]);
            CHECK(0);
        }
    }
    SEND(&c, "GET", k);
    EXPECT_BULK(&c, "orig");
    conn_close(&c);
}

/* E8: EX 1 → 1초 뒤 GET → nil (실제 시간) (R2) */
static void e08_expires_after_1s(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e08", "k");
    SEND(&c, "SET", k, "v", "EX", "1");
    EXPECT(&c, RE_OK);
    SEND(&c, "GET", k);
    EXPECT_BULK(&c, "v");
    sleep_ms(AFTER_1S);
    SEND(&c, "GET", k);
    EXPECT(&c, RE_NIL);
    conn_close(&c);
}

/* E9: EX -1 → 같은 시간이 지나도 값 (R2) */
static void e09_ex_minus1_forever(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e09", "k");
    SEND(&c, "SET", k, "v", "EX", "-1");
    EXPECT(&c, RE_OK);
    sleep_ms(AFTER_1S);
    SEND(&c, "GET", k);
    EXPECT_BULK(&c, "v");
    conn_close(&c);
}

/* E10: EXPIRE → +OK, 값은 그대로, 새 기간 적용 (R2) */
static void e10_expire_existing(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e10", "k");
    SEND(&c, "SET", k, "v", "EX", "100");
    EXPECT(&c, RE_OK);
    SEND(&c, "EXPIRE", k, "1");
    EXPECT(&c, RE_OK);
    SEND(&c, "GET", k);
    EXPECT_BULK(&c, "v");
    sleep_ms(AFTER_1S);
    SEND(&c, "GET", k);
    EXPECT(&c, RE_NIL);
    conn_close(&c);
}

/* E11: 없는 키 EXPIRE → 없는 키입니다 (R2) */
static void e11_expire_missing(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e11", "nope");
    SEND(&c, "EXPIRE", k, "10");
    EXPECT(&c, E_NOKEY);
    SEND(&c, "GET", k);            /* 키가 생기면 안 됨 */
    EXPECT(&c, RE_NIL);
    conn_close(&c);
}

/* E12: EX 1로 저장 후 EXPIRE -1 → 1초 뒤에도 값 (R2) */
static void e12_expire_to_forever(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e12", "k");
    SEND(&c, "SET", k, "v", "EX", "1");
    EXPECT(&c, RE_OK);
    SEND(&c, "EXPIRE", k, "-1");
    EXPECT(&c, RE_OK);
    sleep_ms(AFTER_1S);
    SEND(&c, "GET", k);
    EXPECT_BULK(&c, "v");
    conn_close(&c);
}

/* E13: EXPIRE k 0 → 잘못된 값입니다, 값·기간 그대로 (원래 기간 1초가 지나면 사라짐) (R2) */
static void e13_expire_zero(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e13", "k");
    SEND(&c, "SET", k, "v", "EX", "1");
    EXPECT(&c, RE_OK);
    SEND(&c, "EXPIRE", k, "0");
    EXPECT(&c, E_BADVAL);
    SEND(&c, "GET", k);
    EXPECT_BULK(&c, "v");
    sleep_ms(AFTER_1S);
    SEND(&c, "GET", k);
    EXPECT(&c, RE_NIL);
    conn_close(&c);
}

/* ---------- 키·값 제한 ---------- */

/* E14: 빈 키 → 빈 키입니다 (SET / GET / DEL / EXPIRE), 연결 유지 (R1) */
static void e14_empty_key(void) {
    conn_t c; open_or_fail(&c);
    SEND(&c, "SET", "", "v", "EX", "10");
    EXPECT(&c, E_EMPTY);
    SEND(&c, "GET", "");
    EXPECT(&c, E_EMPTY);
    SEND(&c, "DEL", "");
    EXPECT(&c, E_EMPTY);
    SEND(&c, "EXPIRE", "", "10");
    EXPECT(&c, E_EMPTY);
    char k[128]; key(k, "e14", "after");
    SEND(&c, "GET", k);            /* 연결이 살아 있음 */
    EXPECT(&c, RE_NIL);
    conn_close(&c);
}

/* 접두어로 시작해서 len바이트를 채운 키 */
static char *long_key(const char *tcase, size_t len) {
    char prefix[128];
    key(prefix, tcase, "");
    char *k = malloc(len + 1);
    memset(k, 'x', len);
    memcpy(k, prefix, strlen(prefix));
    k[len] = '\0';
    return k;
}

/* E15: 키 256B → +OK, 257B → 너무 큽니다 + 연결 끊김 (R1) */
static void e15_key_size_limit(void) {
    conn_t c; open_or_fail(&c);
    char *k256 = long_key("e15", MAX_KEY);
    SEND(&c, "SET", k256, "v", "EX", "10");
    EXPECT(&c, RE_OK);
    SEND(&c, "GET", k256);
    EXPECT_BULK(&c, "v");
    conn_close(&c);

    open_or_fail(&c);
    char *k257 = long_key("e15", MAX_KEY + 1);
    SEND(&c, "SET", k257, "v", "EX", "10");
    EXPECT(&c, E_TOOBIG);
    CHECK(wait_closed(&c, 2000));
    conn_close(&c);
    free(k256);
    free(k257);
}

/* E16: 값 1,048,576B → +OK (GET으로 전부 확인), 1,048,577B → 너무 큽니다 + 끊김 (R1)
 *
 * 초과 쪽은 길이 머리(`$1048577`)까지만 보낸다. 서버가 본문을 받기 전에 끊기로 했으니
 * (노션 결정), 본문까지 보내면 서버가 안 읽은 데이터를 버리며 연결이 RST로 끊겨
 * 에러 응답을 못 받을 수 있다. 그래서 테스트가 흔들리지 않게 머리까지만 보낸다. */
static void e16_value_size_limit(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e16", "k");
    char *v = malloc(MAX_VAL);
    for (size_t i = 0; i < MAX_VAL; i++) v[i] = (char)('a' + i % 26);
    const char *argv[] = {"SET", k, v, "EX", "10"};
    size_t lens[] = {3, strlen(k), MAX_VAL, 2, 2};
    CHECK(send_cmd(&c, 5, argv, lens) == 0);
    EXPECT(&c, RE_OK);
    SEND(&c, "GET", k);
    CHECK(expect_bulk(&c, v, MAX_VAL));
    conn_close(&c);
    free(v);

    open_or_fail(&c);
    char head[256];
    int n = snprintf(head, sizeof head,
                     "*5\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$%d\r\n",
                     strlen(k), k, MAX_VAL + 1);
    CHECK(send_raw(&c, head, (size_t)n) == 0);
    EXPECT(&c, E_TOOBIG);
    CHECK(wait_closed(&c, 2000));
    conn_close(&c);
}

/* E17: 값에 \r\n과 \0이 들어가도 그대로 저장·조회 (R1, R4) */
static void e17_binary_value(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e17", "k");
    const char v[] = {'a', '\r', '\n', 'b', '\0', 'c'};
    const char *argv[] = {"SET", k, v, "EX", "10"};
    size_t lens[] = {3, strlen(k), sizeof v, 2, 2};
    CHECK(send_cmd(&c, 5, argv, lens) == 0);
    EXPECT(&c, RE_OK);
    SEND(&c, "GET", k);
    CHECK(expect_bulk(&c, v, sizeof v));
    conn_close(&c);
}

/* ---------- 대소문자 ---------- */

/* E18: 명령·EX는 대소문자 구분 안 함 (R4) */
static void e18_command_case_insensitive(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e18", "k");
    SEND(&c, "set", k, "v1", "ex", "10");
    EXPECT(&c, RE_OK);
    SEND(&c, "Set", k, "v2", "Ex", "10");
    EXPECT(&c, RE_OK);
    SEND(&c, "sEt", k, "v3", "eX", "10");
    EXPECT(&c, RE_OK);
    SEND(&c, "get", k);
    EXPECT_BULK(&c, "v3");
    SEND(&c, "Expire", k, "10");
    EXPECT(&c, RE_OK);
    SEND(&c, "dEl", k);
    EXPECT(&c, RE_OK);
    conn_close(&c);
}

/* E19: 키는 대소문자 구분 (R1) */
static void e19_key_case_sensitive(void) {
    conn_t c; open_or_fail(&c);
    char kU[128], kl[128];
    key(kU, "e19", "Key");
    key(kl, "e19", "key");
    SEND(&c, "SET", kU, "v", "EX", "10");
    EXPECT(&c, RE_OK);
    SEND(&c, "GET", kl);
    EXPECT(&c, RE_NIL);
    conn_close(&c);
}

/* ---------- 모르는 명령 ---------- */

/* E20: 모르는 명령 → 모르는 명령입니다, 같은 연결로 계속 (R4) */
static void e20_unknown_command(void) {
    conn_t c; open_or_fail(&c);
    SEND(&c, "HELLO");
    EXPECT(&c, E_UNKNOWN);
    char k[128]; key(k, "e20", "k");
    SEND(&c, "GET", k);
    EXPECT(&c, RE_NIL);
    conn_close(&c);
}

/* E21: 인자 개수 틀림 → 모르는 명령입니다, 이어서 정상 (R4) */
static void e21_wrong_arg_count(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e21", "k");
    SEND(&c, "GET", k, "extra");
    EXPECT(&c, E_UNKNOWN);
    SEND(&c, "SET", k);            /* 값까지도 없음 → 3번 단계에서 걸림 */
    EXPECT(&c, E_UNKNOWN);
    SEND(&c, "DEL");
    EXPECT(&c, E_UNKNOWN);
    SEND(&c, "EXPIRE", k);
    EXPECT(&c, E_UNKNOWN);
    SEND(&c, "SET", k, "v", "EX", "10");
    EXPECT(&c, RE_OK);
    SEND(&c, "GET", k);
    EXPECT_BULK(&c, "v");
    conn_close(&c);
}

/* ---------- 요청 조각·붙임 ---------- */

/* E22: 명령 하나를 1바이트씩 → 정상 응답 하나 (R4) */
static void e22_one_byte_at_a_time(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e22", "k");
    const char *argv[] = {"SET", k, "v", "EX", "10"};
    size_t lens[] = {3, strlen(k), 1, 2, 2};
    char buf[512];
    size_t n = build_cmd(buf, sizeof buf, 5, argv, lens);
    for (size_t i = 0; i < n; i++) {
        CHECK(send_raw(&c, buf + i, 1) == 0);
        sleep_ms(2);
    }
    EXPECT(&c, RE_OK);
    SEND(&c, "GET", k);
    EXPECT_BULK(&c, "v");
    conn_close(&c);
}

/* E23: 명령 두 개를 한 번에 붙여 보냄 → 응답 두 개가 순서대로 (R4) */
static void e23_pipelined(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e23", "k");
    const char *a1[] = {"SET", k, "v", "EX", "10"};
    size_t l1[] = {3, strlen(k), 1, 2, 2};
    const char *a2[] = {"GET", k};
    size_t l2[] = {3, strlen(k)};
    char buf[1024];
    size_t n = build_cmd(buf, sizeof buf, 5, a1, l1);
    n += build_cmd(buf + n, sizeof buf - n, 2, a2, l2);
    CHECK(send_raw(&c, buf, n) == 0);
    EXPECT(&c, RE_OK);
    EXPECT_BULK(&c, "v");
    conn_close(&c);
}

/* E24: 깨진 RESP → 잘못된 요청 형식입니다 + 끊김, 새 연결은 정상 (R4)
 * (*로 시작하지 않는 요청은 조각 1에서 정하지 않아 테스트하지 않음) */
static void e24_malformed(void) {
    const char *cases[] = {
        /* $5라 했는데 뒤 5바이트 다음에 \r\n이 아님 */
        "*2\r\n$3\r\nGET\r\n$5\r\nabc\r\nXY",
        /* 길이 자리에 글자 */
        "*1\r\n$x\r\nGET\r\n",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        conn_t c; open_or_fail(&c);
        CHECK(send_raw(&c, cases[i], strlen(cases[i])) == 0);
        if (!expect_reply(&c, E_FORMAT, sizeof(E_FORMAT) - 1)) {
            fprintf(stderr, "      (case %zu)\n", i);
            CHECK(0);
        }
        CHECK(wait_closed(&c, 2000));
        conn_close(&c);
    }
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e24", "after");
    SEND(&c, "GET", k);
    EXPECT(&c, RE_NIL);
    conn_close(&c);
}

/* ---------- 동시성 ---------- */

#define NCLIENTS 20

/* E25: 여러 클라이언트가 동시에 서로 다른 키 SET → 전부 조회됨 (R5)
 * 모든 연결에 먼저 다 보내고 나서 응답을 읽어서, 서버 쪽에서 요청이 섞여 들어오게 한다. */
static void e25_concurrent_distinct_keys(void) {
    conn_t cs[NCLIENTS];
    char keys[NCLIENTS][128], vals[NCLIENTS][16];
    for (int i = 0; i < NCLIENTS; i++) {
        open_or_fail(&cs[i]);
        char name[16]; snprintf(name, sizeof name, "k%d", i);
        key(keys[i], "e25", name);
        snprintf(vals[i], sizeof vals[i], "v%d", i);
    }
    for (int i = 0; i < NCLIENTS; i++) SEND(&cs[i], "SET", keys[i], vals[i], "EX", "100");
    for (int i = 0; i < NCLIENTS; i++) EXPECT(&cs[i], RE_OK);
    for (int i = 0; i < NCLIENTS; i++) {
        SEND(&cs[0], "GET", keys[i]);
        EXPECT_BULK(&cs[0], vals[i]);
    }
    for (int i = 0; i < NCLIENTS; i++) conn_close(&cs[i]);
}

/* E26: 여러 클라이언트가 같은 키에 다른 값·기간으로 SET
 *      → 남은 값과 기간이 같은 요청에서 온 것 (값과 기간이 섞이지 않음) (R5)
 * 짝수 번은 "p<i>" + EX -1(영구), 홀수 번은 "t<i>" + EX 1.
 * 남은 값이 p로 시작하면 1초 뒤에도 남아야 하고, t로 시작하면 1초 뒤에 없어야 한다. */
static void e26_concurrent_same_key(void) {
    conn_t cs[NCLIENTS];
    char k[128]; key(k, "e26", "k");
    char vals[NCLIENTS][16];
    for (int i = 0; i < NCLIENTS; i++) {
        open_or_fail(&cs[i]);
        snprintf(vals[i], sizeof vals[i], "%c%d", i % 2 ? 't' : 'p', i);
    }
    for (int i = 0; i < NCLIENTS; i++)
        SEND(&cs[i], "SET", k, vals[i], "EX", i % 2 ? "1" : "-1");
    for (int i = 0; i < NCLIENTS; i++) EXPECT(&cs[i], RE_OK);

    SEND(&cs[0], "GET", k);
    char *got = NULL; size_t glen = 0;
    CHECK(read_reply(&cs[0], &got, &glen, 2000) == 1);
    /* got = "$<n>\r\n<값>\r\n" → 값의 첫 글자 */
    char kind = 0;
    if (got && got[0] == '$') {
        char *body = strstr(got, "\r\n");
        if (body) kind = body[2];
    }
    CHECK(kind == 'p' || kind == 't');
    sleep_ms(AFTER_1S);
    SEND(&cs[0], "GET", k);
    if (kind == 'p') CHECK(expect_reply(&cs[0], got, glen));   /* 영구 → 같은 값 */
    else EXPECT(&cs[0], RE_NIL);                                 /* 1초 → 사라짐 */
    free(got);
    for (int i = 0; i < NCLIENTS; i++) conn_close(&cs[i]);
}

/* E27: 한 클라이언트가 큰 값을 천천히 보내는 중에도 다른 클라이언트는 바로 응답받음 (R5) */
static void e27_slow_sender_does_not_block(void) {
    conn_t slow, fast;
    open_or_fail(&slow);
    open_or_fail(&fast);
    char k[128]; key(k, "e27", "big");
    size_t vlen = 512 * 1024;
    char *v = malloc(vlen);
    memset(v, 'z', vlen);
    const char *argv[] = {"SET", k, v, "EX", "10"};
    size_t lens[] = {3, strlen(k), vlen, 2, 2};
    char *buf = malloc(vlen + 256);
    size_t n = build_cmd(buf, vlen + 256, 5, argv, lens);

    CHECK(send_raw(&slow, buf, n / 2) == 0);      /* 절반만 보내고 멈춤 */
    char k2[128]; key(k2, "e27", "small");
    SEND(&fast, "SET", k2, "v", "EX", "10");
    char *got = NULL; size_t glen = 0;
    int r = read_reply(&fast, &got, &glen, 500);  /* 0.5초 안에 와야 함 */
    CHECK(r == 1 && glen == 5 && memcmp(got, RE_OK, 5) == 0);
    free(got);

    CHECK(send_raw(&slow, buf + n / 2, n - n / 2) == 0);  /* 나머지 */
    EXPECT(&slow, RE_OK);
    free(buf);
    free(v);
    conn_close(&slow);
    conn_close(&fast);
}

/* E28: 요청 중간에 클라이언트가 끊어도 서버는 안 죽고 다른 연결은 정상 (R4) */
static void e28_client_disconnects_midway(void) {
    conn_t a; open_or_fail(&a);
    const char *partial = "*5\r\n$3\r\nSET\r\n$1\r\nk";
    CHECK(send_raw(&a, partial, strlen(partial)) == 0);
    conn_close(&a);
    sleep_ms(50);
    conn_t b; open_or_fail(&b);
    char k[128]; key(k, "e28", "k");
    SEND(&b, "SET", k, "v", "EX", "10");
    EXPECT(&b, RE_OK);
    conn_close(&b);
}

/* ---------- SET 옵션 · 검사 순서 · 상태 불변 ---------- */

/* E29: SET 옵션 자리 → EX만 있으면 보관 기간 필요, 모르는 옵션·EX 두 번은 모르는 명령,
 *      원래 값 그대로 (R2, R4) */
static void e29_set_option_errors(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e29", "k");
    SEND(&c, "SET", k, "orig", "EX", "100");
    EXPECT(&c, RE_OK);
    SEND(&c, "SET", k, "new", "EX");
    EXPECT(&c, E_NEEDEX);
    SEND(&c, "SET", k, "new", "FOO", "10");
    EXPECT(&c, E_UNKNOWN);
    SEND(&c, "SET", k, "new", "EX", "10", "EX", "20");
    EXPECT(&c, E_UNKNOWN);
    SEND(&c, "GET", k);
    EXPECT_BULK(&c, "orig");
    conn_close(&c);
}

/* E30: 빈 키 + EX 0 → 빈 키입니다 (검사 순서: 빈 키가 기간보다 먼저) (R4) */
static void e30_error_order(void) {
    conn_t c; open_or_fail(&c);
    SEND(&c, "SET", "", "v", "EX", "0");
    EXPECT(&c, E_EMPTY);
    conn_close(&c);
}

/* E31: 1MB 넘는 길이를 선언만 하고 본문은 안 보냄 → 기다리지 않고 바로 너무 큽니다 + 끊김 (R1, R4) */
static void e31_huge_length_rejected_before_body(void) {
    conn_t c; open_or_fail(&c);
    const char *head = "*5\r\n$3\r\nSET\r\n$1\r\nk\r\n$2000000\r\n";
    CHECK(send_raw(&c, head, strlen(head)) == 0);
    EXPECT(&c, E_TOOBIG);
    CHECK(wait_closed(&c, 2000));
    conn_close(&c);
}

/* E32: 에러가 나면 원래 값 유지 (R1, R2) */
static void e32_error_keeps_state(void) {
    conn_t c; open_or_fail(&c);
    char k[128]; key(k, "e32", "k");
    SEND(&c, "SET", k, "orig", "EX", "100");
    EXPECT(&c, RE_OK);
    SEND(&c, "SET", k, "new", "EX", "0");
    EXPECT(&c, E_BADVAL);
    SEND(&c, "GET", k);
    EXPECT_BULK(&c, "orig");
    conn_close(&c);
}

int main(void) {
    conn_t probe;
    if (conn_open(&probe) != 0) {
        fprintf(stderr,
                "미니 Redis 서버에 연결할 수 없습니다 (127.0.0.1:%s).\n"
                "서버를 먼저 띄우거나 MINI_REDIS_PORT를 맞춰주세요.\n",
                getenv("MINI_REDIS_PORT") ? getenv("MINI_REDIS_PORT") : "6380");
        return 2;
    }
    conn_close(&probe);

    puts("== 조각 1 외부 테스트 (RESP) ==");
    RUN(e01_set_get);
    RUN(e02_get_missing);
    RUN(e03_del_existing);
    RUN(e04_del_missing);
    RUN(e05_del_twice);
    RUN(e06_set_without_ex);
    RUN(e07_bad_ex_values);
    RUN(e08_expires_after_1s);
    RUN(e09_ex_minus1_forever);
    RUN(e10_expire_existing);
    RUN(e11_expire_missing);
    RUN(e12_expire_to_forever);
    RUN(e13_expire_zero);
    RUN(e14_empty_key);
    RUN(e15_key_size_limit);
    RUN(e16_value_size_limit);
    RUN(e17_binary_value);
    RUN(e18_command_case_insensitive);
    RUN(e19_key_case_sensitive);
    RUN(e20_unknown_command);
    RUN(e21_wrong_arg_count);
    RUN(e22_one_byte_at_a_time);
    RUN(e23_pipelined);
    RUN(e24_malformed);
    RUN(e25_concurrent_distinct_keys);
    RUN(e26_concurrent_same_key);
    RUN(e27_slow_sender_does_not_block);
    RUN(e28_client_disconnects_midway);
    RUN(e29_set_option_errors);
    RUN(e30_error_order);
    RUN(e31_huge_length_rejected_before_body);
    RUN(e32_error_keeps_state);
    return FINISH();
}
