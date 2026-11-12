/*
 * resp_client.h — 외부 테스트용 작은 RESP 클라이언트 (테스트 도우미, 지도만 알면 되는 코드)
 *
 * 서버 주소: 127.0.0.1, 포트는 환경 변수 MINI_REDIS_PORT (없으면 6380)
 * 서버는 테스트 전에 따로 띄워 둔다.
 */
#ifndef RESP_CLIENT_H
#define RESP_CLIENT_H

#include <stddef.h>

typedef struct {
    int fd;
    char *buf;    /* 받았지만 아직 꺼내지 않은 바이트 */
    size_t len;
    size_t cap;
} conn_t;

/* 연결. 성공 0, 실패 -1 */
int conn_open(conn_t *c);
void conn_close(conn_t *c);

/* 바이트를 그대로 보냄 (깨진 요청, 조각 보내기용). 성공 0, 실패 -1 */
int send_raw(conn_t *c, const void *data, size_t len);

/* RESP 배열로 명령을 만들어 보냄. 인자마다 (포인터, 길이) */
int send_cmd(conn_t *c, int argc, const char *const *argv, const size_t *lens);

/* 문자열 인자 여러 개를 NULL로 끝내서 보냄: send_cmdv(c, "SET", k, "v", NULL) */
int send_cmdv(conn_t *c, ...);

/* RESP 명령 바이트를 버퍼에 만들어 줌 (1바이트씩 보내기 등). 반환: 길이 */
size_t build_cmd(char *out, size_t cap, int argc, const char *const *argv,
                 const size_t *lens);

/* 응답 하나를 통째로(원본 바이트) 읽음.
 * 반환: 1 = 응답 하나 받음 (*out은 malloc, 호출자가 free)
 *       0 = 서버가 연결을 닫음
 *      -1 = 시간 초과 또는 오류 */
int read_reply(conn_t *c, char **out, size_t *outlen, int timeout_ms);

/* 다음 응답이 want와 바이트 단위로 같은지. 다르면 받은 내용을 stderr에 찍음 */
int expect_reply(conn_t *c, const char *want, size_t wantlen);

/* 다음 응답이 벌크 문자열 val인지 ($길이\r\n값\r\n) */
int expect_bulk(conn_t *c, const char *val, size_t vlen);

/* timeout_ms 안에 서버가 연결을 닫는지 (더 받을 데이터 없이). 닫으면 1 */
int wait_closed(conn_t *c, int timeout_ms);

void sleep_ms(int ms);

#endif
