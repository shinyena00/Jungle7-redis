/*
 * resp.h — RESP 번역 인터페이스 (server.c ↔ resp.c 약속)
 *
 * server.c가 소켓에서 모은 바이트를 resp_parse에 넘기면 단어 목록이 나온다.
 * 그 목록을 command_execute에 넘기고, 받은 reply_t를 resp_write_reply로
 * 바이트로 바꿔 보낸다.
 *
 * 설계 원칙
 *  - resp.c는 소켓·store를 모르고 malloc도 하지 않는다.
 *  - 상태를 저장하지 않는다. 덜 온 요청은 바이트가 더 모인 뒤
 *    버퍼 처음부터 다시 부르면 된다. (E22)
 *  - 검사 1(형식)·2(크기)만 한다. 명령 이름·인자 개수는 command.c 몫이다.
 */
#ifndef RESP_H
#define RESP_H

#include <stddef.h>

#include "command.h"

/* 한 명령의 최대 단어 수. 가장 긴 명령이 SET k v EX n(5개)이라 5.
 * 넘으면 *N만 보고 바로 형식 에러 + 끊기. (11/13 결정, E29, E36) */
#define RESP_MAX_ARGS 5

/* BAD_FORMAT·TOO_BIG이면 에러를 답한 뒤 연결을 끊는다. (E15, E16, E24, E31) */
typedef enum {
    PARSE_INCOMPLETE,   /* 바이트가 더 와야 한다 */
    PARSE_DONE,         /* 명령 하나 완성 */
    PARSE_BAD_FORMAT,   /* -ERR 잘못된 요청 형식입니다 */
    PARSE_TOO_BIG,      /* -ERR 너무 큽니다 */
} parse_result_t;

/*
 * buffer 맨 앞의 명령 하나를 읽는다.
 * args: RESP_MAX_ARGS칸 이상. 복사본이 아니라 buffer 안을 가리키므로
 *       응답을 다 쓸 때까지 buffer를 옮기거나 지우면 안 된다.
 * DONE일 때만 argc·args·consumed가 의미 있다. consumed 뒤에는
 * 다음 명령이 이어 붙어 있을 수 있다. (E23)
 */
parse_result_t resp_parse (const char * buffer, size_t length, int * argc,
                           command_arg_t * args, size_t * consumed);

/* result: PARSE_BAD_FORMAT 또는 PARSE_TOO_BIG만 넘긴다. */
void resp_fill_error_reply (parse_result_t result, reply_t * reply);

/* resp_write_reply가 쓸 바이트 수. server.c가 out 버퍼 크기를 정할 때 쓴다. */
size_t resp_reply_size (const reply_t * reply);

/*
 * out: resp_reply_size(reply)바이트 이상. 끝에 '\0'을 붙이지 않는다.
 * BULK는 store 안을 가리키므로 다음 store_* 호출 전에 불러야 한다.
 * 반환: 쓴 바이트 수 (resp_reply_size와 같다)
 */
size_t resp_write_reply (const reply_t * reply, char * out);

#endif
