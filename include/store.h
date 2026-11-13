/*
 * store.h — 조각 1 해시 테이블 인터페이스 (11/12 팀 확정)
 *
 * 단위 테스트(tests/unit/test_store.c)가 이 함수들을 직접 부른다.
 * 그래서 테스트를 먼저 쓰려면 이 모양이 먼저 정해져 있어야 한다.
 *
 * 설계 원칙
 *  - 시각은 호출하는 쪽이 넘긴다(now_ms). 테스트가 시계를 마음대로 정할 수 있어서
 *    만료 경계(정각 / 1ms 뒤)를 sleep 없이 정확히 확인할 수 있다. (R2)
 *  - 키·값은 (포인터, 길이)로 받는다. '\0'이 든 키를 허용하기로 했기 때문. (R1)
 *  - 만료 "시각"은 명령 해석 쪽에서 계산해 넘긴다 (지금 + 초 × 1000).
 *    int64 넘침 검사도 명령 해석 쪽 책임이다. store는 받은 시각만 비교한다.
 */
#ifndef STORE_H
#define STORE_H

#include <stddef.h>
#include <stdint.h>

/* 영구 보관 (EX -1, EXPIRE k -1) */
#define STORE_NO_EXPIRE INT64_C(-1)

typedef struct store store_t;

/* 칸 nbuckets개로 시작하는 빈 저장소. 조각 1은 8칸 고정, 늘리지 않음. (R1)
 * 반환: 실패하면 NULL */
store_t *store_new(size_t nbuckets);
void store_free(store_t *s);

/* 키·값을 복사해서 저장한다 (호출자가 버퍼를 바꿔도 저장된 값은 그대로). (R1, R2)
 * 이미 있는 키면(만료됐어도) 값과 만료 시각을 통째로 바꾼다 → 기간 새로 시작.
 * expire_at_ms: 현재 시각이 이 값을 "넘으면" 만료 (now > expire_at).
 *               STORE_NO_EXPIRE면 영구.
 * 반환: 0 성공, -1 메모리 부족 */
int store_set(store_t *s, const char *key, size_t klen,
              const char *val, size_t vlen, int64_t expire_at_ms);

/* 조회. (R1, R2)
 * 반환: 1 = 있음 (*val, *vlen에 저장된 값. 다음 store_* 호출 전까지만 유효)
 *       0 = 없음 또는 만료 (now_ms > expire_at). 만료된 노드는 이때 지워도 된다(게으른 만료). */
int store_get(store_t *s, const char *key, size_t klen, int64_t now_ms,
              const char **val, size_t *vlen);

/* 삭제. (R3)
 * 반환: 1 = 지움, 0 = 없음 또는 이미 만료 */
int store_del(store_t *s, const char *key, size_t klen, int64_t now_ms);

/* 값은 그대로 두고 만료 시각만 새로 건다. (R2)
 * expire_at_ms: STORE_NO_EXPIRE면 영구로 바꿈
 * 반환: 1 = 성공, 0 = 없음 또는 이미 만료 */
int store_expire(store_t *s, const char *key, size_t klen,
                 int64_t expire_at_ms, int64_t now_ms);

/* 만료된 키를 전부 지운다. 게으른 만료라 아무도 안 건드린 만료 키가
 * 메모리를 잡고 있을 수 있어서, store_set이 메모리 부족일 때 부른다.
 * 전체를 훑으므로 느리다. 평소에는 부르지 않는다. (11/13 추가, 팀 확인 필요)
 * 반환: 1 = 하나 이상 지움, 0 = 지운 것 없음 */
int store_purge_expired(store_t *s, int64_t now_ms);

#endif
