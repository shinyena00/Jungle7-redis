#include "command.h"

#include <stdbool.h>
#include <string.h>

#define MILLISECONDS_PER_SECOND INT64_C(1000)

static bool has_keyword(const command_argument_t * argument,
                        const char * keyword);
static command_reply_t create_error_reply(const char * message);
static bool has_valid_command_shape(command_kind_t kind,
                                    const command_argument_t * arguments,
                                    size_t argument_count);
static command_reply_t execute_set(store_t * store,
                                   const command_argument_t * arguments,
                                   size_t argument_count, int64_t now_ms);
static int64_t * parse_expire_at(const command_argument_t * argument,
                                 int64_t now_ms, int64_t * expire_at_ms);
static int64_t * parse_positive_seconds(const command_argument_t * argument,
                                        int64_t * seconds);
static command_reply_t execute_get(store_t * store,
                                   const command_argument_t * key,
                                   int64_t now_ms);
static command_reply_t execute_del(store_t * store,
                                   const command_argument_t * key,
                                   int64_t now_ms);
static command_reply_t execute_expire(store_t * store,
                                      const command_argument_t * arguments,
                                      int64_t now_ms);

/* ── 공개 함수 ── */

command_kind_t find_command_kind (const command_argument_t * argument) {
    if (has_keyword(argument, "SET")) {
        return COMMAND_SET;
    }
    if (has_keyword(argument, "GET")) {
        return COMMAND_GET;
    }
    if (has_keyword(argument, "DEL")) {
        return COMMAND_DEL;
    }
    if (has_keyword(argument, "EXPIRE")) {
        return COMMAND_EXPIRE;
    }
    return COMMAND_UNKNOWN;
}

command_reply_t execute_command (store_t * store,
                                 const command_argument_t * arguments,
                                 size_t argument_count, int64_t now_ms) {
    if (argument_count == 0) {
        return create_error_reply("ERR 모르는 명령입니다");
    }

    command_kind_t kind = find_command_kind(&arguments[0]);
    if (!has_valid_command_shape(kind, arguments, argument_count)) {
        return create_error_reply("ERR 모르는 명령입니다");
    }

    const command_argument_t * key = &arguments[1];
    if (key->length == 0) {
        return create_error_reply("ERR 빈 키입니다");
    }

    switch (kind) {
    case COMMAND_SET:
        return execute_set(store, arguments, argument_count, now_ms);
    case COMMAND_GET:
        return execute_get(store, key, now_ms);
    case COMMAND_DEL:
        return execute_del(store, key, now_ms);
    case COMMAND_EXPIRE:
        return execute_expire(store, arguments, now_ms);
    default:
        return create_error_reply("ERR 모르는 명령입니다");
    }
}

static bool has_keyword (const command_argument_t * argument,
                         const char * keyword) {
    if (argument->length != strlen(keyword)) {
        return false;
    }

    for (size_t i = 0; i < argument->length; i++) {
        unsigned char byte = (unsigned char)argument->data[i];
        if (byte >= 'a' && byte <= 'z') {
            byte -= 'a' - 'A';
        }
        if (byte != (unsigned char)keyword[i]) {
            return false;
        }
    }
    return true;
}

static command_reply_t create_error_reply (const char * message) {
    return (command_reply_t){
        .kind = COMMAND_REPLY_ERROR,
        .data = message,
        .length = strlen(message)
    };
}

static bool has_valid_command_shape (command_kind_t kind,
                                     const command_argument_t * arguments,
                                     size_t argument_count) {
    switch (kind) {
    case COMMAND_SET:
        if (argument_count < 3 || argument_count > 5) {
            return false;
        }
        return argument_count == 3 || has_keyword(&arguments[3], "EX");
    case COMMAND_GET:
    case COMMAND_DEL:
        return argument_count == 2;
    case COMMAND_EXPIRE:
        return argument_count == 3;
    case COMMAND_UNKNOWN:
        return false;
    }
    return false;
}

static command_reply_t execute_set (store_t * store,
                                    const command_argument_t * arguments,
                                    size_t argument_count, int64_t now_ms) {
    if (argument_count < 5) {
        return create_error_reply("ERR 보관 기간이 필요합니다");
    }

    int64_t expire_at_ms;
    if (parse_expire_at(&arguments[4], now_ms, &expire_at_ms) == NULL) {
        return create_error_reply("ERR 잘못된 값입니다");
    }

    const command_argument_t * key = &arguments[1];
    const command_argument_t * value = &arguments[2];
    if (store_set(store, key->data, key->length, value->data, value->length,
                  expire_at_ms) == -1) {
        return (command_reply_t){.kind = COMMAND_REPLY_OUT_OF_MEMORY};
    }
    return (command_reply_t){.kind = COMMAND_REPLY_OK};
}

static int64_t * parse_expire_at (const command_argument_t * argument,
                                  int64_t now_ms, int64_t * expire_at_ms) {
    if (argument->length == 2 && memcmp(argument->data, "-1", 2) == 0) {
        *expire_at_ms = STORE_NO_EXPIRE;
        return expire_at_ms;
    }

    int64_t seconds;
    if (parse_positive_seconds(argument, &seconds) == NULL) {
        return NULL;
    }
    if (seconds > INT64_MAX / MILLISECONDS_PER_SECOND) {
        return NULL;
    }

    int64_t duration_ms = seconds * MILLISECONDS_PER_SECOND;
    if (now_ms > INT64_MAX - duration_ms) {
        return NULL;
    }

    *expire_at_ms = now_ms + duration_ms;
    return expire_at_ms;
}

static int64_t * parse_positive_seconds (const command_argument_t * argument,
                                         int64_t * seconds) {
    int64_t value = 0;
    for (size_t i = 0; i < argument->length; i++) {
        unsigned char byte = (unsigned char)argument->data[i];
        if (byte < '0' || byte > '9') {
            return NULL;
        }

        int64_t digit = byte - '0';
        if (value > (INT64_MAX - digit) / 10) {
            return NULL;
        }
        value = value * 10 + digit;
    }

    if (value == 0) {
        return NULL;
    }
    *seconds = value;
    return seconds;
}

static command_reply_t execute_get (store_t * store,
                                    const command_argument_t * key,
                                    int64_t now_ms) {
    const char * value;
    size_t value_length;
    if (store_get(store, key->data, key->length, now_ms, &value,
                  &value_length) == 0) {
        return (command_reply_t){.kind = COMMAND_REPLY_NIL};
    }
    return (command_reply_t){
        .kind = COMMAND_REPLY_BULK, .data = value, .length = value_length
    };
}

static command_reply_t execute_del (store_t * store,
                                    const command_argument_t * key,
                                    int64_t now_ms) {
    if (store_del(store, key->data, key->length, now_ms) == 0) {
        return create_error_reply("ERR 없는 키입니다");
    }
    return (command_reply_t){.kind = COMMAND_REPLY_OK};
}

static command_reply_t execute_expire (store_t * store,
                                       const command_argument_t * arguments,
                                       int64_t now_ms) {
    int64_t expire_at_ms;
    if (parse_expire_at(&arguments[2], now_ms, &expire_at_ms) == NULL) {
        return create_error_reply("ERR 잘못된 값입니다");
    }

    const command_argument_t * key = &arguments[1];
    if (store_expire(store, key->data, key->length, expire_at_ms,
                     now_ms) == 0) {
        return create_error_reply("ERR 없는 키입니다");
    }
    return (command_reply_t){.kind = COMMAND_REPLY_OK};
}
