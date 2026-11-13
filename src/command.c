/*
 * command.c — 단어 목록을 실행해 응답 종류를 채운다 (노션 4번 검사 3~7)
 *
 * 판단은 여기서, 바이트로 바꾸는 일은 resp.c가 한다.
 * 그래서 이 파일에는 "+OK", "\r\n" 같은 RESP 글자가 나오지 않는다.
 *
 *   command_execute ─┬─ find_command: 이름 → 명령 표 한 줄        (검사 3)
 *                    ├─ has_*_shape: 인자 개수·EX 자리             (검사 3)
 *                    ├─ is_empty_key: 빈 키                       (검사 4)
 *                    └─ command->execute
 *                        ├─ execute_get:    store_get → BULK / NIL
 *                        ├─ execute_set:    기간 있나            (검사 5)
 *                        │                  → parse_expire_at    (검사 6)
 *                        │                  → save_with_purge → OK
 *                        ├─ execute_del:    store_del → OK / 없는 키 (검사 7)
 *                        └─ execute_expire: parse_expire_at      (검사 6)
 *                                           → store_expire → OK / 없는 키
 *                                                                (검사 7)
 *
 * store는 모든 검사가 끝난 뒤 각 execute_*의 마지막에만 부른다.
 * 그래서 에러가 나면 원래 값·기간이 그대로 남는다. (E6, E7, E13, E32)
 */
#include "command.h"

#include <ctype.h>
#include <string.h>

/* ── 자료 구조 ─────────────────────────────────────────────── */

/*
 * 명령 표 한 줄. 표로 부르려고 모양을 맞춘 함수라서,
 * 명령에 따라 argc·args·now_ms 중 안 쓰는 인자가 있다.
 */
typedef struct {
    const char * name;      /* 대문자로 적는다 (대소문자 무시 비교 기준) */
    int (* has_shape) (int argc, const command_arg_t * args);
    void (* execute) (store_t * store, int argc, const command_arg_t * args,
                      int64_t now_ms, reply_t * reply);
} command_t;

/* 에러 문구는 노션 4번 그대로. 테스트가 글자까지 비교한다. */
#define ERROR_UNKNOWN_COMMAND "모르는 명령입니다"
#define ERROR_EMPTY_KEY       "빈 키입니다"
#define ERROR_NEED_EXPIRE     "보관 기간이 필요합니다"
#define ERROR_BAD_VALUE       "잘못된 값입니다"
#define ERROR_NO_KEY          "없는 키입니다"
/* 노션에 없는 문구 (store_set의 -1). 팀 확인 필요 */
#define ERROR_OUT_OF_MEMORY   "메모리가 부족합니다"

#define MS_PER_SECOND INT64_C(1000)

/* ── 도우미 함수 원형 ──────────────────────────────────────── */

static const command_t * find_command (int argc, const command_arg_t * args);
static int is_same_word_ignoring_case (const command_arg_t * word,
                                       const char * name);
static void set_error_reply (reply_t * reply, const char * message);
static int has_key_only_shape (int argc, const command_arg_t * args);
static int has_set_shape (int argc, const command_arg_t * args);
static int has_expire_shape (int argc, const command_arg_t * args);
static int is_empty_key (const command_arg_t * key);
static void execute_get (store_t * store, int argc,
                         const command_arg_t * args, int64_t now_ms,
                         reply_t * reply);
static void set_bulk_reply (reply_t * reply, const char * val, size_t vlen);
static void set_nil_reply (reply_t * reply);
static void execute_set (store_t * store, int argc,
                         const command_arg_t * args, int64_t now_ms,
                         reply_t * reply);
static int64_t * parse_expire_at (const command_arg_t * text, int64_t now_ms,
                                  int64_t * expire_at_ms);
static int is_no_expire_text (const command_arg_t * text);
static int64_t * parse_positive_seconds (const command_arg_t * text,
                                         int64_t * seconds);
static int save_with_purge (store_t * store, const command_arg_t * key,
                            const command_arg_t * value,
                            int64_t expire_at_ms, int64_t now_ms);
static void set_ok_reply (reply_t * reply);
static void execute_del (store_t * store, int argc,
                         const command_arg_t * args, int64_t now_ms,
                         reply_t * reply);
static void execute_expire (store_t * store, int argc,
                            const command_arg_t * args, int64_t now_ms,
                            reply_t * reply);

/* ── 명령 표 ───────────────────────────────────────────────── */

static const command_t COMMANDS[] = {
    { "GET",    has_key_only_shape, execute_get    },
    { "SET",    has_set_shape,      execute_set    },
    { "DEL",    has_key_only_shape, execute_del    },
    { "EXPIRE", has_expire_shape,   execute_expire },
};

#define COMMAND_COUNT (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

/* ── 공개 함수 ─────────────────────────────────────────────── */

/* 여러 에러에 걸리면 노션 4번 검사 순서대로 처음 것 하나만 답한다. (E30) */
void command_execute (store_t * s, int argc, const command_arg_t * args,
                      int64_t now_ms, reply_t * reply) {
    const command_t * command = find_command(argc, args);
    if (command == NULL) {
        set_error_reply(reply, ERROR_UNKNOWN_COMMAND);
        return;
    }
    if (!command->has_shape(argc, args)) {
        set_error_reply(reply, ERROR_UNKNOWN_COMMAND);
        return;
    }
    if (is_empty_key(&args[1])) {
        set_error_reply(reply, ERROR_EMPTY_KEY);
        return;
    }
    command->execute(s, argc, args, now_ms, reply);
}

/* ── 도우미 함수 (처음 불리는 순서) ────────────────────────── */

/* 단어 0개(*0)는 resp.c가 형식 에러로 막는다. 여기 검사는 안전장치. */
static const command_t * find_command (int argc, const command_arg_t * args) {
    if (argc < 1) {
        return NULL;
    }
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (is_same_word_ignoring_case(&args[0], COMMANDS[i].name)) {
            return &COMMANDS[i];
        }
    }
    return NULL;
}

/*
 * 명령 이름·EX는 대소문자를 안 가린다. (E18)
 * word는 '\0'으로 끝나지 않을 수 있어서 strcmp 대신 길이를 먼저 맞춘다.
 */
static int is_same_word_ignoring_case (const command_arg_t * word,
                                       const char * name) {
    size_t length = strlen(name);
    if (word->length != length) {
        return 0;
    }
    for (size_t i = 0; i < length; i++) {
        if (toupper((unsigned char)word->data[i]) != name[i]) {
            return 0;
        }
    }
    return 1;
}

/* message: 고정 문구만 받는다. reply가 가리키기만 해서 수명이 길어야 한다. */
static void set_error_reply (reply_t * reply, const char * message) {
    reply->kind = REPLY_ERROR;
    reply->data = message;
    reply->length = strlen(message);
}

/* GET k, DEL k → 통과. GET, GET k v → 모르는 명령 (E21) */
static int has_key_only_shape (int argc, const command_arg_t * args) {
    (void)args;
    return argc == 2;
}

/*
 * SET k v              → 통과. 검사 5에서 "보관 기간이 필요합니다" (E6)
 * SET k v EX           → 통과. 검사 5에서 "보관 기간이 필요합니다" (E29)
 * SET k v EX n         → 통과
 * SET k v FOO 10       → 모르는 명령 (E29)
 * SET k                → 모르는 명령 (E21)
 * SET k v EX 10 EX 20  → 여기 안 옴. 단어 6개 이상은 resp.c가 형식 에러 (E29)
 */
static int has_set_shape (int argc, const command_arg_t * args) {
    if (argc == 3) {
        return 1;
    }
    if (argc == 4 || argc == 5) {
        return is_same_word_ignoring_case(&args[3], "EX");
    }
    return 0;
}

/* EXPIRE k n → 통과. EXPIRE k, EXPIRE k n m → 모르는 명령 (E21) */
static int has_expire_shape (int argc, const command_arg_t * args) {
    (void)args;
    return argc == 3;
}

static int is_empty_key (const command_arg_t * key) {
    return key->length == 0;
}

/* 없는 키와 만료된 키는 똑같이 NIL. 앱은 원본에서 가져와 다시 SET (E2, E8) */
static void execute_get (store_t * store, int argc,
                         const command_arg_t * args, int64_t now_ms,
                         reply_t * reply) {
    (void)argc;
    const char * val;
    size_t vlen;
    if (!store_get(store, args[1].data, args[1].length, now_ms, &val, &vlen)) {
        set_nil_reply(reply);
        return;
    }
    set_bulk_reply(reply, val, vlen);
}

/* val: store 안을 가리킨다. 다음 store_* 호출 전에 resp.c가 바이트로 써야 함 */
static void set_bulk_reply (reply_t * reply, const char * val, size_t vlen) {
    reply->kind = REPLY_BULK;
    reply->data = val;
    reply->length = vlen;
}

static void set_nil_reply (reply_t * reply) {
    reply->kind = REPLY_NIL;
    reply->data = NULL;
    reply->length = 0;
}

/* argc 3·4는 모양 검사를 통과한 "기간 없는 SET"이다. (검사 5, E6, E29) */
static void execute_set (store_t * store, int argc,
                         const command_arg_t * args, int64_t now_ms,
                         reply_t * reply) {
    if (argc != 5) {
        set_error_reply(reply, ERROR_NEED_EXPIRE);
        return;
    }
    int64_t expire_at_ms;
    if (parse_expire_at(&args[4], now_ms, &expire_at_ms) == NULL) {
        set_error_reply(reply, ERROR_BAD_VALUE);
        return;
    }
    if (save_with_purge(store, &args[1], &args[2], expire_at_ms,
                        now_ms) != 0) {
        set_error_reply(reply, ERROR_OUT_OF_MEMORY);
        return;
    }
    set_ok_reply(reply);
}

/*
 * 기간 글자 → 만료 시각. 성공하면 expire_at_ms, 실패하면 NULL. (검사 6)
 *   "-1"   → STORE_NO_EXPIRE (영구)
 *   "10"   → now_ms + 10 × 1000
 *   지금 + 기간 × 1000이 int64를 넘으면 실패. 따로 상한은 없다. (노션 4번)
 * 넘침은 계산 "전에" 본다. 넘친 뒤 확인하는 건 C에서 정의되지 않은 동작.
 * now_ms는 0 이상이어야 한다 (server.c가 시계에서 읽은 값).
 */
static int64_t * parse_expire_at (const command_arg_t * text, int64_t now_ms,
                                  int64_t * expire_at_ms) {
    if (is_no_expire_text(text)) {
        *expire_at_ms = STORE_NO_EXPIRE;
        return expire_at_ms;
    }

    int64_t seconds;
    if (parse_positive_seconds(text, &seconds) == NULL) {
        return NULL;
    }
    /* 곱하면 넘치나 */
    if (seconds > INT64_MAX / MS_PER_SECOND) {
        return NULL;
    }
    int64_t duration_ms = seconds * MS_PER_SECOND;
    /* 더하면 넘치나 */
    if (now_ms > INT64_MAX - duration_ms) {
        return NULL;
    }

    *expire_at_ms = now_ms + duration_ms;
    return expire_at_ms;
}

static int is_no_expire_text (const command_arg_t * text) {
    return text->length == 2 && memcmp(text->data, "-1", 2) == 0;
}

/*
 * 받는 것: 0으로 시작하지 않는 양의 정수. 글자 그대로만 본다.
 *   "10"                         → 10
 *   "0", "01", "-01", "-2"       → NULL (E7, E13)
 *   "+1", " 1", "1abc", "1.5", "" → NULL
 * strtoll은 앞 공백과 '+'를 받아 주고 "-01"을 -1로 읽어서 쓰지 않는다.
 * 첫 글자가 1~9가 아니면 0, 음수, 앞의 0, 부호, 공백, 빈 값이 한 번에 걸린다.
 * 20자리 같은 수는 int64에도 안 들어가서 글자마다 넘침을 확인한다.
 */
static int64_t * parse_positive_seconds (const command_arg_t * text,
                                         int64_t * seconds) {
    if (text->length == 0 || text->data[0] < '1' || text->data[0] > '9') {
        return NULL;
    }
    int64_t total = 0;
    for (size_t i = 0; i < text->length; i++) {
        if (text->data[i] < '0' || text->data[i] > '9') {
            return NULL;
        }
        int digit = text->data[i] - '0';
        if (total > (INT64_MAX - digit) / 10) {
            return NULL;
        }
        total = total * 10 + digit;
    }
    *seconds = total;
    return seconds;
}

/*
 * 게으른 만료라 아무도 안 건드린 만료 키가 메모리를 잡고 있을 수 있다.
 * 메모리가 부족하면 만료 키를 모두 치우고 한 번만 다시 시도한다.
 * 치운 게 없으면 다시 해도 같으므로 바로 실패.
 * 연결은 끊지 않는다. 메모리는 store 전체가 쓰는 거라 끊어도 안 생긴다.
 * store_set은 실패해도 원래 값을 그대로 두므로 다시 불러도 안전하다.
 * 반환: store_set과 같음 (0 성공, -1 메모리 부족)
 */
static int save_with_purge (store_t * store, const command_arg_t * key,
                            const command_arg_t * value,
                            int64_t expire_at_ms, int64_t now_ms) {
    if (store_set(store, key->data, key->length, value->data, value->length,
                  expire_at_ms) == 0) {
        return 0;
    }
    if (!store_purge_expired(store, now_ms)) {
        return -1;
    }
    return store_set(store, key->data, key->length, value->data,
                     value->length, expire_at_ms);
}

static void set_ok_reply (reply_t * reply) {
    reply->kind = REPLY_OK;
    reply->data = NULL;
    reply->length = 0;
}

/* 없는 키·만료된 키는 에러다 (실제 Redis의 0과 다름, 노션 결정). (E4, E5) */
static void execute_del (store_t * store, int argc,
                         const command_arg_t * args, int64_t now_ms,
                         reply_t * reply) {
    (void)argc;
    if (!store_del(store, args[1].data, args[1].length, now_ms)) {
        set_error_reply(reply, ERROR_NO_KEY);
        return;
    }
    set_ok_reply(reply);
}

/*
 * 기간 검사(6)가 키 검사(7)보다 먼저다.
 *   있는 키에 EXPIRE k 0 → 잘못된 값 (E13)
 *   없는 키에 EXPIRE k 0 → 잘못된 값 (없는 키 아님)
 * 만료된 키를 되살리거나 새로 만들지 않는다. (E11)
 */
static void execute_expire (store_t * store, int argc,
                            const command_arg_t * args, int64_t now_ms,
                            reply_t * reply) {
    (void)argc;
    int64_t expire_at_ms;
    if (parse_expire_at(&args[2], now_ms, &expire_at_ms) == NULL) {
        set_error_reply(reply, ERROR_BAD_VALUE);
        return;
    }
    if (!store_expire(store, args[1].data, args[1].length,
                      expire_at_ms, now_ms)) {
        set_error_reply(reply, ERROR_NO_KEY);
        return;
    }
    set_ok_reply(reply);
}
