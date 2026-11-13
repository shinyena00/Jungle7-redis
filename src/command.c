/*
 * command.c — 조각 1 커맨드 파싱 (노션 4번 "에러 검사 순서" 3~7번)
 *
 *   execute_command ─┬─ find_command: 명령 이름 + 단어 개수 → 어떤 명령인지 (3번)
 *                    ├─ 빈 키인가 (4번)
 *                    ├─ execute_get: store_get → 값 / nil
 *                    ├─ execute_del: store_del → OK / 없는 키 (7번)
 *                    ├─ SET에 기간이 없나 (5번)
 *                    ├─ parse_seconds → compute_expire_at: 기간 숫자 (6번)
 *                    ├─ execute_set: store_set → OK
 *                    └─ execute_expire: store_expire → OK / 없는 키 (7번)
 */
#include "command.h"

#include <string.h>
#include <strings.h>

/* ── 자료 구조 ─────────────────────────────────────────────── */

typedef enum command_kind {
    COMMAND_UNKNOWN,
    COMMAND_GET,
    COMMAND_SET,
    COMMAND_DEL,
    COMMAND_EXPIRE,
} command_kind_t;

/* 응답 문구는 노션 4번 그대로. 테스트가 바이트 단위로 비교한다. */
#define REPLY_OK        "+OK\r\n"
#define REPLY_NIL       "$-1\r\n"
#define REPLY_UNKNOWN   "-ERR 모르는 명령입니다\r\n"
#define REPLY_EMPTY_KEY "-ERR 빈 키입니다\r\n"
#define REPLY_NEED_TTL  "-ERR 보관 기간이 필요합니다\r\n"
#define REPLY_BAD_VALUE "-ERR 잘못된 값입니다\r\n"
#define REPLY_NO_KEY    "-ERR 없는 키입니다\r\n"
/* 노션에 없는 문구 (store_set의 -1). 팀 확인 필요 */
#define REPLY_NO_MEMORY "-ERR 메모리가 부족합니다\r\n"

/* 문자열 상수의 길이를 컴파일할 때 셈 (끝의 '\0' 한 바이트 뺌) */
#define TEXT_REPLY(literal) create_text_reply((literal), sizeof(literal) - 1)

/* ── 도우미 함수 원형 ──────────────────────────────────────── */

static command_kind_t find_command (int count,
                                    const char * const * arguments,
                                    const size_t * lengths);
static int is_word (const char * argument, size_t length, const char * word);
static int is_set_shape (int count, const char * const * arguments,
                         const size_t * lengths);
static command_reply_t create_text_reply (const char * text, size_t length);
static command_reply_t execute_get (store_t * store, const char * key,
                                    size_t key_length, int64_t now_ms);
static command_reply_t create_value_reply (const char * value,
                                           size_t value_length);
static command_reply_t execute_del (store_t * store, const char * key,
                                    size_t key_length, int64_t now_ms);
static int parse_seconds (const char * text, size_t length,
                          int64_t * seconds);
static int compute_expire_at (int64_t seconds, int64_t now_ms,
                              int64_t * expire_at_ms);
static command_reply_t execute_set (store_t * store, const char * key,
                                    size_t key_length, const char * value,
                                    size_t value_length,
                                    int64_t expire_at_ms);
static command_reply_t execute_expire (store_t * store, const char * key,
                                       size_t key_length,
                                       int64_t expire_at_ms, int64_t now_ms);

/* ── 공개 함수 ─────────────────────────────────────────────── */

/* 검사 순서(노션 4번)가 위에서 아래로 그대로 읽히게 둔다.
 * 모든 검사를 통과한 뒤에만 store를 건드려서, 에러면 상태가 그대로다. */
command_reply_t execute_command (store_t * store, int count,
                                 const char * const * arguments,
                                 const size_t * lengths, int64_t now_ms) {
    command_kind_t kind = find_command(count, arguments, lengths);
    if (kind == COMMAND_UNKNOWN) {
        return TEXT_REPLY(REPLY_UNKNOWN);
    }

    /* 3번을 통과한 명령은 모두 arguments[1]이 키다 */
    const char * key = arguments[1];
    size_t key_length = lengths[1];
    if (key_length == 0) {
        return TEXT_REPLY(REPLY_EMPTY_KEY);
    }

    if (kind == COMMAND_GET) {
        return execute_get(store, key, key_length, now_ms);
    }
    if (kind == COMMAND_DEL) {
        return execute_del(store, key, key_length, now_ms);
    }

    /* 3번에서 SET k v, SET k v EX를 통과시켰으니 여기서 거른다 */
    if (kind == COMMAND_SET && count < 5) {
        return TEXT_REPLY(REPLY_NEED_TTL);
    }

    /* 기간 숫자 자리: SET k v EX n은 4번, EXPIRE k n은 2번 */
    int position = (kind == COMMAND_SET) ? 4 : 2;
    int64_t seconds;
    int64_t expire_at_ms;
    if (!parse_seconds(arguments[position], lengths[position], &seconds)
        || !compute_expire_at(seconds, now_ms, &expire_at_ms)) {
        return TEXT_REPLY(REPLY_BAD_VALUE);
    }

    if (kind == COMMAND_SET) {
        return execute_set(store, key, key_length, arguments[2], lengths[2],
                           expire_at_ms);
    }
    return execute_expire(store, key, key_length, expire_at_ms, now_ms);
}

/* ── 도우미 함수 (처음 불리는 순서) ────────────────────────── */

/* 이름과 단어 개수가 둘 다 맞아야 그 명령이다. 개수가 틀리면 "모르는 명령" (E21) */
static command_kind_t find_command (int count,
                                    const char * const * arguments,
                                    const size_t * lengths) {
    const char * name = arguments[0];
    size_t name_length = lengths[0];

    if (is_word(name, name_length, "GET") && count == 2) {
        return COMMAND_GET;
    }
    if (is_word(name, name_length, "DEL") && count == 2) {
        return COMMAND_DEL;
    }
    if (is_word(name, name_length, "EXPIRE") && count == 3) {
        return COMMAND_EXPIRE;
    }
    if (is_word(name, name_length, "SET")
        && is_set_shape(count, arguments, lengths)) {
        return COMMAND_SET;
    }
    return COMMAND_UNKNOWN;
}

/* 명령 이름과 EX는 대소문자를 구분하지 않는다 (E18).
 * argument는 '\0'으로 안 끝날 수 있어 길이를 먼저 맞춘 뒤 비교한다. */
static int is_word (const char * argument, size_t length, const char * word) {
    size_t word_length = strlen(word);
    return length == word_length
        && strncasecmp(argument, word, word_length) == 0;
}

/*
 * SET k v           → 통과. 5번에서 "보관 기간이 필요합니다" (E6)
 * SET k v EX        → 통과. 5번에서 "보관 기간이 필요합니다" (E29)
 * SET k v EX n      → 통과
 * SET k v FOO 10, SET k v EX 10 EX 20, SET k → 모르는 명령 (E21, E29)
 */
static int is_set_shape (int count, const char * const * arguments,
                         const size_t * lengths) {
    if (count == 3) {
        return 1;
    }
    if (count != 4 && count != 5) {
        return 0;
    }
    return is_word(arguments[3], lengths[3], "EX");
}

static command_reply_t create_text_reply (const char * text, size_t length) {
    command_reply_t reply = {
        .text = text,
        .text_length = length,
        .value = NULL,
        .value_length = 0,
    };
    return reply;
}

/* 만료된 키도 없는 키와 똑같이 nil (노션 4번) */
static command_reply_t execute_get (store_t * store, const char * key,
                                    size_t key_length, int64_t now_ms) {
    const char * value;
    size_t value_length;
    if (store_get(store, key, key_length, now_ms, &value, &value_length)) {
        return create_value_reply(value, value_length);
    }
    return TEXT_REPLY(REPLY_NIL);
}

/* value: store 안을 가리킨다. 보내는 쪽이 다음 store_* 호출 전에 복사 */
static command_reply_t create_value_reply (const char * value,
                                           size_t value_length) {
    command_reply_t reply = {
        .text = NULL,
        .text_length = 0,
        .value = value,
        .value_length = value_length,
    };
    return reply;
}

/* 만료된 키도 "없는 키" (E4, u18) */
static command_reply_t execute_del (store_t * store, const char * key,
                                    size_t key_length, int64_t now_ms) {
    if (store_del(store, key, key_length, now_ms)) {
        return TEXT_REPLY(REPLY_OK);
    }
    return TEXT_REPLY(REPLY_NO_KEY);
}

/*
 * 받는 것: "-1" 또는 0으로 시작하지 않는 양의 정수. 글자 그대로만 본다.
 * strtol·atoi는 "-01"을 -1로, "1abc"를 1로, " 1"·"+1"을 1로 읽어서 쓰지 않는다.
 * 넘침은 곱하기 "전에" 검사한다. 넘친 뒤 확인하는 건 C에서 정의되지 않은 동작. (E7)
 */
static int parse_seconds (const char * text, size_t length,
                          int64_t * seconds) {
    if (length == 2 && text[0] == '-' && text[1] == '1') {
        *seconds = -1;
        return 1;
    }
    /* 첫 글자가 1~9가 아니면 0, 음수, 앞의 0, 부호, 공백, 빈 문자열이다 */
    if (length == 0 || text[0] < '1' || text[0] > '9') {
        return 0;
    }
    int64_t total = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        int digit = text[i] - '0';
        if (total > (INT64_MAX - digit) / 10) {
            return 0;
        }
        total = total * 10 + digit;
    }
    *seconds = total;
    return 1;
}

/* 지금(ms) + 기간×1000이 int64를 넘으면 실패 (노션 4번). 따로 상한은 없다 */
static int compute_expire_at (int64_t seconds, int64_t now_ms,
                              int64_t * expire_at_ms) {
    if (seconds == -1) {
        *expire_at_ms = STORE_NO_EXPIRE;
        return 1;
    }
    if (seconds > (INT64_MAX - now_ms) / 1000) {
        return 0;
    }
    *expire_at_ms = now_ms + seconds * 1000;
    return 1;
}

/* 이미 있는 키(만료됐어도)면 값·기간을 통째로 바꾼다: 기간 새로 시작 (u17) */
static command_reply_t execute_set (store_t * store, const char * key,
                                    size_t key_length, const char * value,
                                    size_t value_length,
                                    int64_t expire_at_ms) {
    if (store_set(store, key, key_length, value, value_length,
                  expire_at_ms) != 0) {
        return TEXT_REPLY(REPLY_NO_MEMORY);
    }
    return TEXT_REPLY(REPLY_OK);
}

/* 만료된 키도 "없는 키". 키를 새로 만들지 않는다 (E11, u18) */
static command_reply_t execute_expire (store_t * store, const char * key,
                                       size_t key_length,
                                       int64_t expire_at_ms, int64_t now_ms) {
    if (store_expire(store, key, key_length, expire_at_ms, now_ms)) {
        return TEXT_REPLY(REPLY_OK);
    }
    return TEXT_REPLY(REPLY_NO_KEY);
}
