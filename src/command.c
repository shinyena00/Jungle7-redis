/*
 * command.c — 단어 목록을 실행해 응답 종류를 채운다 (검사 3~7)
 *
 * 공개 함수 모양은 include/command.h 그대로 둔다.
 *
 *   command_execute ──┬─ find_command      모르는 명령        (검사 3)
 *                     ├─ has_*_shape       인자 개수·옵션     (검사 3)
 *                     ├─ is_empty_key      빈 키              (검사 4)
 *                     └─ execute_*         기간 없음 → 잘못된 값 → 없는 키
 *                                                             (검사 5~7)
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

/* 에러 문구는 노션 4번 규칙 그대로. 테스트가 글자까지 비교한다. */
#define ERROR_UNKNOWN_COMMAND "모르는 명령입니다"
#define ERROR_EMPTY_KEY       "빈 키입니다"
#define ERROR_NEED_EXPIRE     "보관 기간이 필요합니다"
#define ERROR_BAD_VALUE       "잘못된 값입니다"
#define ERROR_NO_KEY          "없는 키입니다"
#define ERROR_OUT_OF_MEMORY   "메모리가 부족합니다"

#define MS_PER_SECOND 1000

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
static int is_valid_seconds (const command_arg_t * seconds, int64_t now_ms);
static int is_seconds_text (const command_arg_t * seconds);
static int is_no_expire_text (const command_arg_t * seconds);
static int is_within_expire_limit (const command_arg_t * seconds,
                                   int64_t now_ms);
static int64_t convert_to_expire_at (const command_arg_t * seconds,
                                     int64_t now_ms);
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

/* 단어가 하나도 없으면 이름이 없으므로 모르는 명령(NULL)으로 본다. */
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
 * name은 대문자로 적힌 글자여야 한다. 명령 이름·EX는 대소문자를 안 가린다. (E18)
 * word는 '\0'으로 끝나지 않으므로 strcmp 대신 길이로 비교한다.
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

/* message: 고정 문구만 받는다. reply가 가리키기만 하므로 수명이 길어야 한다. */
static void set_error_reply (reply_t * reply, const char * message) {
    reply->kind = REPLY_ERROR;
    reply->data = message;
    reply->length = strlen(message);
}

static int has_key_only_shape (int argc, const command_arg_t * args) {
    (void)args;
    return argc == 2;
}

/*
 * SET k v, SET k v EX도 통과시킨다. 이 둘은 "모르는 명령"이 아니라
 * 검사 5의 "보관 기간이 필요합니다"로 답해야 하기 때문. (E6, E29)
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

static int has_expire_shape (int argc, const command_arg_t * args) {
    (void)args;
    return argc == 3;
}

static int is_empty_key (const command_arg_t * key) {
    return key->length == 0;
}

/* 없는 키와 만료된 키는 똑같이 NIL로 답한다. (E2, E8) */
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

/* val: store 안을 가리킨다. 다음 store_* 호출 전에 직렬화해야 한다. */
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
    if (!is_valid_seconds(&args[4], now_ms)) {
        set_error_reply(reply, ERROR_BAD_VALUE);
        return;
    }
    int64_t expire_at_ms = convert_to_expire_at(&args[4], now_ms);
    if (store_set(store, args[1].data, args[1].length,
                  args[2].data, args[2].length, expire_at_ms) != 0) {
        set_error_reply(reply, ERROR_OUT_OF_MEMORY);
        return;
    }
    set_ok_reply(reply);
}

/* 검사 6. 글자 모양을 먼저 보고, 그다음 만료 시각이 int64에 들어가는지 본다. */
static int is_valid_seconds (const command_arg_t * seconds, int64_t now_ms) {
    return is_seconds_text(seconds) && is_within_expire_limit(seconds, now_ms);
}

/*
 * 정확히 -1(영구)이거나 0으로 시작하지 않는 숫자만 통과.
 * 0, -2, -01, 01, +1, " 1", 1abc, 1.5, 빈 값은 거절한다. (E7, E13)
 * strtoll은 앞 공백과 '+'를 받아 주므로 쓰지 않는다.
 */
static int is_seconds_text (const command_arg_t * seconds) {
    if (is_no_expire_text(seconds)) {
        return 1;
    }
    if (seconds->length == 0 || seconds->data[0] == '0') {
        return 0;
    }
    for (size_t i = 0; i < seconds->length; i++) {
        if (seconds->data[i] < '0' || seconds->data[i] > '9') {
            return 0;
        }
    }
    return 1;
}

static int is_no_expire_text (const command_arg_t * seconds) {
    return seconds->length == 2 && memcmp(seconds->data, "-1", 2) == 0;
}

/*
 * now_ms + 초 × 1000이 int64를 넘지 않는가. 실제로 곱하고 더하면 그 계산이
 * 먼저 넘치므로, 상한을 (INT64_MAX - now_ms) / 1000으로 옮겨서 비교한다.
 * 20자리 같은 수는 int64에도 안 들어가서 글자를 읽을 때마다 확인한다.
 * is_seconds_text를 통과한 글자, 0 이상인 now_ms만 받는다.
 */
static int is_within_expire_limit (const command_arg_t * seconds,
                                   int64_t now_ms) {
    if (is_no_expire_text(seconds)) {
        return 1;
    }
    int64_t limit = (INT64_MAX - now_ms) / MS_PER_SECOND;
    int64_t value = 0;
    for (size_t i = 0; i < seconds->length; i++) {
        int digit = seconds->data[i] - '0';
        if (value > (limit - digit) / 10) {
            return 0;
        }
        value = value * 10 + digit;
    }
    return 1;
}

/* is_valid_seconds를 통과한 글자만 받는다. 그래서 여기서는 넘치지 않는다. */
static int64_t convert_to_expire_at (const command_arg_t * seconds,
                                     int64_t now_ms) {
    if (is_no_expire_text(seconds)) {
        return STORE_NO_EXPIRE;
    }
    int64_t value = 0;
    for (size_t i = 0; i < seconds->length; i++) {
        value = value * 10 + (seconds->data[i] - '0');
    }
    return now_ms + value * MS_PER_SECOND;
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

/* 기간 검사(6)가 키 검사(7)보다 먼저다. 있는 키에 EXPIRE k 0 → 잘못된 값. (E13) */
static void execute_expire (store_t * store, int argc,
                            const command_arg_t * args, int64_t now_ms,
                            reply_t * reply) {
    (void)argc;
    if (!is_valid_seconds(&args[2], now_ms)) {
        set_error_reply(reply, ERROR_BAD_VALUE);
        return;
    }
    int64_t expire_at_ms = convert_to_expire_at(&args[2], now_ms);
    if (!store_expire(store, args[1].data, args[1].length,
                      expire_at_ms, now_ms)) {
        set_error_reply(reply, ERROR_NO_KEY);
        return;
    }
    set_ok_reply(reply);
}
