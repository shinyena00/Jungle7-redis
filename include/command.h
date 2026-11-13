/*
 * command.h — 조각 1 커맨드 파싱 인터페이스 (제안, 팀 확인 필요)
 *
 * RESP 파싱(①)이 잘라 준 단어 목록을 받아, 노션 4번 "에러 검사 순서" 3~7번대로
 * 검사하고 store_*를 부른 뒤, 보낼 응답을 돌려준다.
 * 1·2번(형식 깨짐, 크기 초과)은 ①에서 이미 걸러졌다고 가정한다.
 *
 * 소켓을 모르는 함수라서 서버 없이도 따로 확인할 수 있다.
 */
#ifndef COMMAND_H
#define COMMAND_H

#include <stddef.h>
#include <stdint.h>

#include "store.h"

/* 보낼 응답 하나.
 * text가 NULL이 아니면 그 바이트를 그대로 보낸다 ("+OK\r\n", "-ERR ...\r\n", "$-1\r\n").
 * text가 NULL이면 value를 벌크 문자열($길이\r\n값\r\n)로 만들어 보낸다 (GET 성공).
 * value는 store 안을 가리키므로 다음 store_* 호출 전에 복사해야 한다. */
typedef struct command_reply {
    const char * text;
    size_t text_length;
    const char * value;
    size_t value_length;
} command_reply_t;

/* arguments[0]은 명령 이름, count는 1 이상.
 * 각 단어는 (arguments[i], lengths[i]) 쌍이다. '\0'으로 끝난다고 가정하지 않는다. */
command_reply_t execute_command (store_t * store, int count,
                                 const char * const * arguments,
                                 const size_t * lengths, int64_t now_ms);

#endif
