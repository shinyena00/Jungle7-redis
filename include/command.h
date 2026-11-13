/*
 * command.h — 명령 실행 인터페이스 (resp.c ↔ command.c 약속)
 *
 * resp.c가 RESP를 풀어 단어 목록을 만들고, command.c가 그 목록을 실행해
 * 응답 "종류"를 돌려준다. 바이트로 바꾸는 일은 다시 resp.c가 한다.
 *
 * 설계 원칙
 *  - command.c는 소켓·시계·RESP를 모른다. 지금 시각(now_ms)은 호출자가 넘긴다.
 *  - 검사 1·2(형식, 크기)는 resp.c가 끝낸 뒤 부른다. 여기서는 검사 3~7만 한다.
 *  - 에러가 나면 store를 부르지 않는다. 원래 값·기간이 그대로 남는다. (R1, R2)
 */
#ifndef COMMAND_H
#define COMMAND_H

#include <stddef.h>
#include <stdint.h>

#include "store.h"

/* 단어 하나. 복사본이 아니라 resp.c 입력 버퍼 안을 가리킨다. */
typedef struct {
    const char * data;
    size_t length;
} command_arg_t;

typedef enum {
    REPLY_OK,       /* +OK */
    REPLY_NIL,      /* $-1 */
    REPLY_BULK,     /* $<length>\r\n<data> */
    REPLY_ERROR,    /* -ERR <data> */
} reply_kind_t;

/*
 * BULK: data는 store 안의 값. 다음 store_* 호출 전까지만 유효하다.
 * ERROR: data는 "-ERR " 뒤에 붙일 고정 문구 (UTF-8, '\0'으로 끝남).
 * OK·NIL: data는 NULL, length는 0.
 */
typedef struct {
    reply_kind_t kind;
    const char * data;
    size_t length;
} reply_t;

/* 검사 3~7을 순서대로 하고, 통과하면 store를 불러 reply를 채운다.
 * reply는 어떤 경우에도 하나로 채워진다. */
void command_execute (store_t * s, int argc, const command_arg_t * args,
                      int64_t now_ms, reply_t * reply);

#endif
