#ifndef COMMAND_H
#define COMMAND_H

#include <stddef.h>
#include <stdint.h>

#include "store.h"

typedef enum {
    COMMAND_UNKNOWN,
    COMMAND_SET,
    COMMAND_GET,
    COMMAND_DEL,
    COMMAND_EXPIRE
} command_kind_t;

typedef struct {
    const char * data;
    size_t length;
} command_argument_t;

typedef enum {
    COMMAND_REPLY_OK,
    COMMAND_REPLY_NIL,
    COMMAND_REPLY_BULK,
    COMMAND_REPLY_ERROR,
    COMMAND_REPLY_OUT_OF_MEMORY
} command_reply_kind_t;

typedef struct {
    command_reply_kind_t kind;
    const char * data;
    size_t length;
} command_reply_t;

command_kind_t find_command_kind(const command_argument_t * argument);

/* now_ms는 0 이상. BULK의 data는 다음 store_* 호출 전까지 빌려 쓴다. */
/* 최대 앞 5개 인자만 참조한다. argument_count는 실제 인자 수다. */
/* ERROR의 data는 RESP 앞뒤 구분자를 뺀 오류 본문이다. */
command_reply_t execute_command(store_t * store,
                                const command_argument_t * arguments,
                                size_t argument_count, int64_t now_ms);

#endif
