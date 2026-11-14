#ifndef RESP_H
#define RESP_H

#include <stddef.h>

#include "command.h"

#define RESP_STORED_ARGUMENT_COUNT 5

typedef enum {
    RESP_COMPLETE,
    RESP_INCOMPLETE,
    RESP_INVALID_FORMAT,
    RESP_TOO_LARGE
} resp_status_t;

typedef struct {
    command_argument_t arguments[RESP_STORED_ARGUMENT_COUNT];
    size_t argument_count;
    size_t consumed_length;
} resp_request_t;

/* 완료 시에만 결과가 유효하다. 인자 데이터는 입력 버퍼를 빌려 쓴다. */
/* 인자 수는 실제 개수이며, 배열에는 앞의 최대 5개만 담긴다. */
resp_status_t parse_resp_request(const char * buffer, size_t length,
                                 resp_request_t * request);

/* 오류에 해당하면 전송할 RESP 문자열, 그 밖에는 NULL을 반환한다. */
const char * find_resp_error_reply(resp_status_t status);

#endif
