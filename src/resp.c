#include "resp.h"

#include <stdbool.h>
#include <stdint.h>

#define MAX_KEY_LENGTH 256
#define MAX_VALUE_LENGTH 1048576

typedef struct {
    const char * data;
    size_t remaining;
} resp_input_t;

static resp_status_t parse_length(resp_input_t * input, char prefix,
                                  size_t * length);
static resp_status_t consume_crlf(resp_input_t * input);
static size_t find_bulk_limit(command_kind_t kind, size_t index);
static resp_status_t parse_bulk_argument(resp_input_t * input, size_t limit,
                                         command_argument_t * argument,
                                         bool * oversized);

/* ── 공개 함수 ── */

resp_status_t parse_resp_request (const char * buffer, size_t length,
                                  resp_request_t * request) {
    resp_input_t input = {buffer, length};
    size_t argument_count;
    resp_status_t status = parse_length(&input, '*', &argument_count);
    if (status == RESP_TOO_LARGE) {
        return RESP_INVALID_FORMAT;
    }
    if (status != RESP_COMPLETE) {
        return status;
    }

    command_kind_t kind = COMMAND_UNKNOWN;
    bool oversized = false;
    for (size_t i = 0; i < argument_count; i++) {
        command_argument_t argument;
        size_t limit = find_bulk_limit(kind, i);
        status = parse_bulk_argument(&input, limit, &argument, &oversized);
        if (status == RESP_INCOMPLETE && oversized) {
            return RESP_TOO_LARGE;
        }
        if (status != RESP_COMPLETE) {
            return status;
        }
        if (i < RESP_STORED_ARGUMENT_COUNT) {
            request->arguments[i] = argument;
        }
        if (i == 0) {
            kind = find_command_kind(&argument);
        }
    }

    if (oversized) {
        return RESP_TOO_LARGE;
    }
    request->argument_count = argument_count;
    request->consumed_length = length - input.remaining;
    return RESP_COMPLETE;
}

const char * find_resp_error_reply (resp_status_t status) {
    switch (status) {
    case RESP_INVALID_FORMAT:
        return "-ERR 잘못된 요청 형식입니다\r\n";
    case RESP_TOO_LARGE:
        return "-ERR 너무 큽니다\r\n";
    default:
        return NULL;
    }
}

static resp_status_t parse_length (resp_input_t * input, char prefix,
                                   size_t * length) {
    if (input->remaining == 0) {
        return RESP_INCOMPLETE;
    }
    if (*input->data != prefix) {
        return RESP_INVALID_FORMAT;
    }
    input->data++;
    input->remaining--;

    size_t value = 0;
    bool has_digit = false;
    bool exceeds_limit = false;
    while (input->remaining > 0 && *input->data >= '0' &&
           *input->data <= '9') {
        size_t digit = (size_t)(*input->data - '0');
        has_digit = true;
        if (value > (SIZE_MAX - digit) / 10) {
            exceeds_limit = true;
        }
        if (!exceeds_limit) {
            value = value * 10 + digit;
        }
        input->data++;
        input->remaining--;
    }

    if (input->remaining > 0 && !has_digit) {
        return RESP_INVALID_FORMAT;
    }
    resp_status_t status = consume_crlf(input);
    if (status != RESP_COMPLETE) {
        return status;
    }
    if (exceeds_limit) {
        return RESP_TOO_LARGE;
    }

    *length = value;
    return RESP_COMPLETE;
}

static resp_status_t consume_crlf (resp_input_t * input) {
    if (input->remaining == 0) {
        return RESP_INCOMPLETE;
    }
    if (input->data[0] != '\r') {
        return RESP_INVALID_FORMAT;
    }
    if (input->remaining == 1) {
        return RESP_INCOMPLETE;
    }
    if (input->data[1] != '\n') {
        return RESP_INVALID_FORMAT;
    }
    input->data += 2;
    input->remaining -= 2;
    return RESP_COMPLETE;
}

static size_t find_bulk_limit (command_kind_t kind, size_t index) {
    if (kind != COMMAND_UNKNOWN && index == 1) {
        return MAX_KEY_LENGTH;
    }
    if (kind == COMMAND_SET && index == 2) {
        return MAX_VALUE_LENGTH;
    }
    return SIZE_MAX;
}

static resp_status_t parse_bulk_argument (resp_input_t * input, size_t limit,
                                          command_argument_t * argument,
                                          bool * oversized) {
    size_t length;
    resp_status_t status = parse_length(input, '$', &length);
    if (status == RESP_TOO_LARGE && limit == SIZE_MAX) {
        return RESP_INVALID_FORMAT;
    }
    if (status != RESP_COMPLETE) {
        return status;
    }

    /* 이미 받은 바이트에서는 형식 오류를 먼저 확인한다. */
    if (length > limit) {
        *oversized = true;
    }
    if (length > input->remaining) {
        return RESP_INCOMPLETE;
    }

    argument->data = input->data;
    argument->length = length;
    input->data += length;
    input->remaining -= length;
    return consume_crlf(input);
}
