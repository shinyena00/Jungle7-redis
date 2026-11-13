/*
 * resp.c — RESP 번역기 (검사 1·2)
 *
 *   읽기: 입력 버퍼 → 단어 목록(command_arg_t) 또는 아직 / 형식 에러 / 너무 큼
 *   쓰기: 응답 종류(reply_t) → RESP 바이트
 *
 * 단어 목록은 복사하지 않고 입력 버퍼 안을 가리킨다. (command.h)
 * 명령 이름·키가 있는지는 모른다. 그건 command.c 몫이다.
 *
 *   resp_parse ──┬─ read_array_header   "*<개수>\r\n"
 *                └─ read_bulk           "$<길이>\r\n<본문>\r\n" × 개수
 *                    ├─ is_too_big       본문을 기다리기 전에 크기 판정
 *                    └─ read_bulk_body
 */
#include "resp.h"

#include <ctype.h>
#include <string.h>

/* ── 자료 구조 ─────────────────────────────────────────────── */

#define MAX_KEY_LENGTH    256
#define MAX_VALUE_LENGTH  1048576

/* 한도를 넘는 길이는 모두 이 값으로 본다. 큰 숫자를 끝까지 곱하면 넘치기 때문. */
#define SATURATED_LENGTH  (MAX_VALUE_LENGTH + 1)

/* 길이 숫자의 자릿수 상한. '\r'이 안 오는 줄을 끝없이 기다리지 않으려고 둔다. */
#define MAX_NUMBER_DIGITS 20

/* 네 명령(GET·SET·DEL·EXPIRE) 모두 두 번째 단어가 키다. (E15) */
#define KEY_INDEX 1

/* 에러 문구는 노션 4번 규칙 그대로. 테스트가 글자까지 비교한다. */
#define ERROR_TOO_BIG    "너무 큽니다"
#define ERROR_BAD_FORMAT "잘못된 요청 형식입니다"

#define TEXT_OK      "+OK\r\n"
#define TEXT_NIL     "$-1\r\n"
#define ERROR_PREFIX "-ERR "
#define BULK_PREFIX  "$"
#define CRLF         "\r\n"

/* 문자열 상수의 길이 ('\0' 제외) */
#define LITERAL_LENGTH(text) (sizeof(text) - 1)

/* ── 도우미 함수 원형 ──────────────────────────────────────── */

static parse_result_t read_array_header (const char * buffer, size_t length,
                                         size_t * position, size_t * count);
static parse_result_t read_number_line (const char * buffer, size_t length,
                                        size_t * position, char prefix,
                                        size_t * number);
static parse_result_t find_line_end (const char * buffer, size_t length,
                                     size_t start, size_t * end);
static parse_result_t parse_length (const char * digits, size_t length,
                                    size_t * number);
static size_t append_digit (size_t value, char digit);
static parse_result_t read_crlf (const char * buffer, size_t length,
                                 size_t * position);
static parse_result_t read_bulk (const char * buffer, size_t length,
                                 size_t * position, size_t index,
                                 command_arg_t * arg);
static int is_too_big (size_t index, size_t bulk_length);
static parse_result_t read_bulk_body (const char * buffer, size_t length,
                                      size_t * position, size_t body_length,
                                      command_arg_t * arg);
static void set_error_reply (reply_t * reply, const char * message);
static size_t count_digits (size_t number);
static size_t write_text (char * out, const char * text, size_t length);
static size_t write_bulk (char * out, const char * data, size_t length);
static size_t write_number (char * out, size_t number);
static size_t write_error (char * out, const char * message, size_t length);

/* ── 공개 함수 ─────────────────────────────────────────────── */

/* 앞에서부터 읽다가 처음 걸린 결과를 돌려준다. 그래서 1MB 넘는 길이는
 * 본문이 오기 전에 바로 TOO_BIG이 된다. (E31) */
parse_result_t resp_parse (const char * buffer, size_t length, int * argc,
                           command_arg_t * args, size_t * consumed) {
    size_t position = 0;
    size_t count = 0;
    parse_result_t result = read_array_header(buffer, length, &position,
                                              &count);
    if (result != PARSE_DONE) {
        return result;
    }
    for (size_t i = 0; i < count; i++) {
        result = read_bulk(buffer, length, &position, i, &args[i]);
        if (result != PARSE_DONE) {
            return result;
        }
    }
    *argc = (int)count;
    *consumed = position;
    return PARSE_DONE;
}

void resp_fill_error_reply (parse_result_t result, reply_t * reply) {
    if (result == PARSE_TOO_BIG) {
        set_error_reply(reply, ERROR_TOO_BIG);
        return;
    }
    set_error_reply(reply, ERROR_BAD_FORMAT);
}

size_t resp_reply_size (const reply_t * reply) {
    switch (reply->kind) {
    case REPLY_OK:
        return LITERAL_LENGTH(TEXT_OK);
    case REPLY_NIL:
        return LITERAL_LENGTH(TEXT_NIL);
    case REPLY_BULK:
        return LITERAL_LENGTH(BULK_PREFIX) + count_digits(reply->length)
               + LITERAL_LENGTH(CRLF) + reply->length + LITERAL_LENGTH(CRLF);
    case REPLY_ERROR:
        return LITERAL_LENGTH(ERROR_PREFIX) + reply->length
               + LITERAL_LENGTH(CRLF);
    }
    return 0;
}

size_t resp_write_reply (const reply_t * reply, char * out) {
    switch (reply->kind) {
    case REPLY_OK:
        return write_text(out, TEXT_OK, LITERAL_LENGTH(TEXT_OK));
    case REPLY_NIL:
        return write_text(out, TEXT_NIL, LITERAL_LENGTH(TEXT_NIL));
    case REPLY_BULK:
        return write_bulk(out, reply->data, reply->length);
    case REPLY_ERROR:
        return write_error(out, reply->data, reply->length);
    }
    return 0;
}

/* ── 도우미 함수 (처음 불리는 순서) ────────────────────────── */

/*
 * *0은 조각 1에서 정하지 않아 형식 에러로 둔다. (tests/README.md)
 * *6 이상도 형식 에러. 단어를 다 받기 전에 *N만 보고 거른다. (E29, E36)
 */
static parse_result_t read_array_header (const char * buffer, size_t length,
                                         size_t * position, size_t * count) {
    parse_result_t result = read_number_line(buffer, length, position, '*',
                                             count);
    if (result != PARSE_DONE) {
        return result;
    }
    if (*count == 0) {
        return PARSE_BAD_FORMAT;
    }
    if (*count > RESP_MAX_ARGS) {
        return PARSE_BAD_FORMAT;
    }
    return PARSE_DONE;
}

/* "<prefix><숫자>\r\n" 한 줄. 성공하면 position을 줄 다음으로 옮긴다.
 * 첫 글자가 '*'가 아닌 요청(인라인 명령)도 여기서 형식 에러가 된다. */
static parse_result_t read_number_line (const char * buffer, size_t length,
                                        size_t * position, char prefix,
                                        size_t * number) {
    if (*position >= length) {
        return PARSE_INCOMPLETE;
    }
    if (buffer[*position] != prefix) {
        return PARSE_BAD_FORMAT;
    }
    size_t start = *position + 1;
    size_t end = 0;
    parse_result_t result = find_line_end(buffer, length, start, &end);
    if (result != PARSE_DONE) {
        return result;
    }
    result = parse_length(buffer + start, end - start, number);
    if (result != PARSE_DONE) {
        return result;
    }
    *position = end;
    return read_crlf(buffer, length, position);
}

/* end: 줄을 끝내는 '\r'의 위치 */
static parse_result_t find_line_end (const char * buffer, size_t length,
                                     size_t start, size_t * end) {
    for (size_t i = start; i < length; i++) {
        if (buffer[i] == '\r') {
            *end = i;
            return PARSE_DONE;
        }
        if (i - start >= MAX_NUMBER_DIGITS) {
            return PARSE_BAD_FORMAT;
        }
    }
    return PARSE_INCOMPLETE;
}

/* 숫자만 받는다. 빈 칸, 부호(+1, -1), 공백, 글자는 형식 에러. (E24) */
static parse_result_t parse_length (const char * digits, size_t length,
                                    size_t * number) {
    if (length == 0) {
        return PARSE_BAD_FORMAT;
    }
    size_t value = 0;
    for (size_t i = 0; i < length; i++) {
        if (!isdigit((unsigned char)digits[i])) {
            return PARSE_BAD_FORMAT;
        }
        value = append_digit(value, digits[i]);
    }
    *number = value;
    return PARSE_DONE;
}

/* 한도를 넘으면 SATURATED_LENGTH에 멈춘다. 곱하기 전 값이 한도 이하라서
 * 32비트 size_t에서도 넘치지 않는다. */
static size_t append_digit (size_t value, char digit) {
    if (value > MAX_VALUE_LENGTH) {
        return SATURATED_LENGTH;
    }
    value = value * 10 + (size_t)(digit - '0');
    if (value > MAX_VALUE_LENGTH) {
        return SATURATED_LENGTH;
    }
    return value;
}

/* 받은 바이트까지만 보고 틀렸으면 바로 형식 에러, 덜 왔으면 아직. (E24) */
static parse_result_t read_crlf (const char * buffer, size_t length,
                                 size_t * position) {
    for (size_t i = 0; i < LITERAL_LENGTH(CRLF); i++) {
        if (*position + i >= length) {
            return PARSE_INCOMPLETE;
        }
        if (buffer[*position + i] != CRLF[i]) {
            return PARSE_BAD_FORMAT;
        }
    }
    *position += LITERAL_LENGTH(CRLF);
    return PARSE_DONE;
}

/* index: 몇 번째 단어인지. 키 자리면 한도가 다르다. */
static parse_result_t read_bulk (const char * buffer, size_t length,
                                 size_t * position, size_t index,
                                 command_arg_t * arg) {
    size_t bulk_length = 0;
    parse_result_t result = read_number_line(buffer, length, position, '$',
                                             &bulk_length);
    if (result != PARSE_DONE) {
        return result;
    }
    if (is_too_big(index, bulk_length)) {
        return PARSE_TOO_BIG;
    }
    return read_bulk_body(buffer, length, position, bulk_length, arg);
}

/* 키 256B, 그 밖의 모든 단어(명령 이름·EX·기간 포함) 1MB까지 허용 (경계 포함).
 * 명령을 보기 전에 판정하므로 모르는 명령도 키 자리가 257B면 너무 큼.
 * (E15, E16, E33~E35) */
static int is_too_big (size_t index, size_t bulk_length) {
    if (index == KEY_INDEX) {
        return bulk_length > MAX_KEY_LENGTH;
    }
    return bulk_length > MAX_VALUE_LENGTH;
}

/* 본문 안의 \r\n·\0은 그냥 바이트로 넘긴다. 길이로만 끊는다. (E17) */
static parse_result_t read_bulk_body (const char * buffer, size_t length,
                                      size_t * position, size_t body_length,
                                      command_arg_t * arg) {
    if (length - *position < body_length) {
        return PARSE_INCOMPLETE;
    }
    arg->data = buffer + *position;
    arg->length = body_length;
    *position += body_length;
    return read_crlf(buffer, length, position);
}

/* message: 고정 문구만 받는다. reply가 가리키기만 하므로 수명이 길어야 한다. */
static void set_error_reply (reply_t * reply, const char * message) {
    reply->kind = REPLY_ERROR;
    reply->data = message;
    reply->length = strlen(message);
}

/* 0도 한 자리("$0")로 센다. */
static size_t count_digits (size_t number) {
    size_t digits = 1;
    while (number >= 10) {
        number /= 10;
        digits++;
    }
    return digits;
}

static size_t write_text (char * out, const char * text, size_t length) {
    memcpy(out, text, length);
    return length;
}

/* 값에 \r\n·\0이 있어도 길이만큼 그대로 복사한다. (E17) */
static size_t write_bulk (char * out, const char * data, size_t length) {
    size_t written = write_text(out, BULK_PREFIX, LITERAL_LENGTH(BULK_PREFIX));
    written += write_number(out + written, length);
    written += write_text(out + written, CRLF, LITERAL_LENGTH(CRLF));
    written += write_text(out + written, data, length);
    written += write_text(out + written, CRLF, LITERAL_LENGTH(CRLF));
    return written;
}

/* sprintf를 안 쓰는 이유: 끝에 '\0'을 붙여 out을 1바이트 넘어 쓰기 때문. */
static size_t write_number (char * out, size_t number) {
    size_t digits = count_digits(number);
    for (size_t i = digits; i > 0; i--) {
        out[i - 1] = (char)('0' + number % 10);
        number /= 10;
    }
    return digits;
}

static size_t write_error (char * out, const char * message, size_t length) {
    size_t written = write_text(out, ERROR_PREFIX,
                                LITERAL_LENGTH(ERROR_PREFIX));
    written += write_text(out + written, message, length);
    written += write_text(out + written, CRLF, LITERAL_LENGTH(CRLF));
    return written;
}
