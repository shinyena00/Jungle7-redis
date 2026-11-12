/*
 * test_store.c — 조각 1 단위 테스트 (U1~U18, 노션 5-1)
 *
 * 해시 테이블(store.h)을 네트워크 없이 직접 부른다.
 * 시각은 테스트가 정해서 넘긴다 → sleep 없이 만료 경계를 확인.
 *
 * ⚠️ AI 초안. 사람이 한 줄씩 검토하고, 기대값이 노션 4번 규칙과 맞는지 확인할 것.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../check.h"
#include "store.h"

#define NBUCKETS 8 /* 조각 1: 칸 8개 고정 */

/* 문자열 키·값 줄여 쓰기 (strlen으로 길이) */
#define S(str) (str), strlen(str)

/* 조회해서 기대값과 같은지. 길이까지 비교 */
static int get_is(store_t *s, const char *key, size_t klen, int64_t now,
                  const char *want, size_t wlen) {
    const char *v = NULL;
    size_t vlen = 0;
    if (store_get(s, key, klen, now, &v, &vlen) != 1) return 0;
    return vlen == wlen && memcmp(v, want, wlen) == 0;
}

static int get_none(store_t *s, const char *key, size_t klen, int64_t now) {
    const char *v = NULL;
    size_t vlen = 0;
    return store_get(s, key, klen, now, &v, &vlen) == 0;
}

/* ---------- R1. 저장·조회 ---------- */

/* U1: 저장 후 조회 → 값 (R1) */
static void u01_set_then_get(void) {
    store_t *s = store_new(NBUCKETS);
    CHECK(s != NULL);
    CHECK(store_set(s, S("k"), S("v"), STORE_NO_EXPIRE) == 0);
    CHECK(get_is(s, S("k"), 0, S("v")));
    store_free(s);
}

/* U2: 같은 키 재저장 후 조회 → 새 값 (R1) */
static void u02_overwrite(void) {
    store_t *s = store_new(NBUCKETS);
    CHECK(store_set(s, S("k"), S("old"), STORE_NO_EXPIRE) == 0);
    CHECK(store_set(s, S("k"), S("new-longer"), STORE_NO_EXPIRE) == 0);
    CHECK(get_is(s, S("k"), 0, S("new-longer")));
    store_free(s);
}

/* U3: 없는 키 조회 → 없음 (R1) */
static void u03_missing(void) {
    store_t *s = store_new(NBUCKETS);
    CHECK(get_none(s, S("nope"), 0));
    store_free(s);
}

/* U4: 칸 8개에 키 9개 → 9개 모두 조회 (비둘기집: 충돌이 반드시 생김) (R1) */
static void u04_collision_9_keys(void) {
    store_t *s = store_new(NBUCKETS);
    char k[16], v[16];
    for (int i = 0; i < 9; i++) {
        snprintf(k, sizeof k, "key%d", i);
        snprintf(v, sizeof v, "val%d", i);
        CHECK(store_set(s, S(k), S(v), STORE_NO_EXPIRE) == 0);
    }
    for (int i = 0; i < 9; i++) {
        snprintf(k, sizeof k, "key%d", i);
        snprintf(v, sizeof v, "val%d", i);
        CHECK(get_is(s, S(k), 0, S(v)));
    }
    store_free(s);
}

/*
 * U5~U7: 체인 맨 앞 / 가운데 / 맨 끝 삭제 (R1, R3)
 *
 * 테스트는 해시 함수 결과(어느 키가 어느 칸에 가는지)를 모른다고 가정한다.
 * 대신 키 24개를 칸 8개에 넣으면 어떤 칸에는 키가 3개 이상 있다(비둘기집).
 * 키를 하나씩 지워보는 걸 24번 반복하면, 그 칸의 맨 앞·가운데·맨 끝이 각각 한 번은 지워진다.
 * 매번 "지운 키만 없고 나머지 23개는 전부 있다"를 확인한다.
 */
#define NDEL 24
static void u05_07_delete_each_chain_position(void) {
    char k[16], v[16];
    for (int del = 0; del < NDEL; del++) {
        store_t *s = store_new(NBUCKETS);
        for (int i = 0; i < NDEL; i++) {
            snprintf(k, sizeof k, "key%d", i);
            snprintf(v, sizeof v, "val%d", i);
            store_set(s, S(k), S(v), STORE_NO_EXPIRE);
        }
        snprintf(k, sizeof k, "key%d", del);
        CHECK(store_del(s, S(k), 0) == 1);
        for (int i = 0; i < NDEL; i++) {
            snprintf(k, sizeof k, "key%d", i);
            snprintf(v, sizeof v, "val%d", i);
            if (i == del) CHECK(get_none(s, S(k), 0));
            else CHECK(get_is(s, S(k), 0, S(v)));
        }
        store_free(s);
    }
}

/* U8: '\0'이 든 키는 길이까지 비교 → 서로 다른 키 (R1) */
static void u08_nul_in_key(void) {
    store_t *s = store_new(NBUCKETS);
    const char k1[] = {'a', '\0', 'b'};
    const char k2[] = {'a', '\0', 'c'};
    const char k3[] = {'a'};
    CHECK(store_set(s, k1, 3, S("B"), STORE_NO_EXPIRE) == 0);
    CHECK(store_set(s, k2, 3, S("C"), STORE_NO_EXPIRE) == 0);
    CHECK(store_set(s, k3, 1, S("A"), STORE_NO_EXPIRE) == 0);
    CHECK(get_is(s, k1, 3, 0, S("B")));
    CHECK(get_is(s, k2, 3, 0, S("C")));
    CHECK(get_is(s, k3, 1, 0, S("A")));
    store_free(s);
}

/* ---------- R3. 삭제 ---------- */

/* U9: 삭제 후 조회 → 없음 (R3) */
static void u09_del_then_get(void) {
    store_t *s = store_new(NBUCKETS);
    store_set(s, S("k"), S("v"), STORE_NO_EXPIRE);
    CHECK(store_del(s, S("k"), 0) == 1);
    CHECK(get_none(s, S("k"), 0));
    CHECK(store_del(s, S("k"), 0) == 0); /* 두 번째 삭제는 "없음" */
    store_free(s);
}

/* U10: 삭제 후 같은 키 재저장 → 새 값 (R3) */
static void u10_del_then_set(void) {
    store_t *s = store_new(NBUCKETS);
    store_set(s, S("k"), S("v1"), STORE_NO_EXPIRE);
    store_del(s, S("k"), 0);
    CHECK(store_set(s, S("k"), S("v2"), STORE_NO_EXPIRE) == 0);
    CHECK(get_is(s, S("k"), 0, S("v2")));
    store_free(s);
}

/* ---------- R2. 만료 (시각은 ms) ---------- */

/* 저장 시각 1000ms에 "10초" 보관 → 만료 시각 11000 */
#define T0 INT64_C(1000)
#define EXP (T0 + INT64_C(10000))

/* U11: 만료 1ms 전 → 값 (R2) */
static void u11_before_expire(void) {
    store_t *s = store_new(NBUCKETS);
    store_set(s, S("k"), S("v"), EXP);
    CHECK(get_is(s, S("k"), EXP - 1, S("v")));
    store_free(s);
}

/* U12: 만료 시각 정각 → 아직 값 (경계 규칙: now > expire_at 이어야 만료) (R2) */
static void u12_at_expire_still_visible(void) {
    store_t *s = store_new(NBUCKETS);
    store_set(s, S("k"), S("v"), EXP);
    CHECK(get_is(s, S("k"), EXP, S("v")));
    store_free(s);
}

/* U13: 만료 시각 + 1ms → 없음 (R2) */
static void u13_after_expire_gone(void) {
    store_t *s = store_new(NBUCKETS);
    store_set(s, S("k"), S("v"), EXP);
    CHECK(get_none(s, S("k"), EXP + 1));
    /* 한 번 만료로 없다고 나왔으면, 시계를 되돌려도(테스트 전용 상황) 다시 보일 필요는 없다.
       그래서 여기서는 되돌린 시각으로 다시 묻지 않는다. */
    store_free(s);
}

/* U14: 만료됐지만 아직 안 지워진 키에 재저장 → 바로 조회하면 새 값 (R2) */
static void u14_set_over_expired_node(void) {
    store_t *s = store_new(NBUCKETS);
    store_set(s, S("k"), S("old"), EXP);
    /* EXP+5000 시점: 아무도 조회 안 해서 노드는 남아 있을 수 있음 */
    int64_t now = EXP + 5000;
    CHECK(store_set(s, S("k"), S("new"), now + 10000) == 0);
    CHECK(get_is(s, S("k"), now, S("new")));
    store_free(s);
}

/* U15: 영구(-1) 저장 → 아주 먼 시각에도 값 (R2) */
static void u15_no_expire(void) {
    store_t *s = store_new(NBUCKETS);
    store_set(s, S("k"), S("v"), STORE_NO_EXPIRE);
    CHECK(get_is(s, S("k"), INT64_MAX / 2, S("v")));
    store_free(s);
}

/* U16: 기간만 새로 걸기 → 값은 그대로, 새 만료 시각 적용 (R2) */
static void u16_expire_changes_only_deadline(void) {
    store_t *s = store_new(NBUCKETS);
    store_set(s, S("k"), S("v"), EXP);
    int64_t new_exp = EXP + 20000;
    CHECK(store_expire(s, S("k"), new_exp, T0 + 1) == 1);
    CHECK(get_is(s, S("k"), EXP + 1, S("v")));  /* 옛 만료 시각이 지나도 값 */
    CHECK(get_is(s, S("k"), new_exp, S("v")));  /* 새 만료 시각 정각에도 값 */
    CHECK(get_none(s, S("k"), new_exp + 1));    /* 새 만료 시각 넘으면 없음 */
    store_free(s);
}

/* U17: 재저장하면 기간이 새로 시작 (옛 만료 시각이 지나도 값) (R2) */
static void u17_reset_restarts_deadline(void) {
    store_t *s = store_new(NBUCKETS);
    store_set(s, S("k"), S("v1"), EXP);
    int64_t new_exp = EXP + 20000;
    store_set(s, S("k"), S("v2"), new_exp);
    CHECK(get_is(s, S("k"), EXP + 1, S("v2")));
    CHECK(get_none(s, S("k"), new_exp + 1));
    store_free(s);
}

/* U18: 만료된 키 삭제·기간 걸기 → "없는 키" (R2, R3) */
static void u18_expired_key_del_and_expire(void) {
    store_t *s = store_new(NBUCKETS);
    store_set(s, S("a"), S("v"), EXP);
    store_set(s, S("b"), S("v"), EXP);
    CHECK(store_del(s, S("a"), EXP + 1) == 0);
    CHECK(store_expire(s, S("b"), EXP + 99999, EXP + 1) == 0);
    /* 없는 키였으니 기간 걸기가 키를 되살리면 안 됨 */
    CHECK(get_none(s, S("b"), EXP + 2));
    store_free(s);
}

int main(void) {
    puts("== 조각 1 단위 테스트 (store) ==");
    RUN(u01_set_then_get);
    RUN(u02_overwrite);
    RUN(u03_missing);
    RUN(u04_collision_9_keys);
    RUN(u05_07_delete_each_chain_position);
    RUN(u08_nul_in_key);
    RUN(u09_del_then_get);
    RUN(u10_del_then_set);
    RUN(u11_before_expire);
    RUN(u12_at_expire_still_visible);
    RUN(u13_after_expire_gone);
    RUN(u14_set_over_expired_node);
    RUN(u15_no_expire);
    RUN(u16_expire_changes_only_deadline);
    RUN(u17_reset_restarts_deadline);
    RUN(u18_expired_key_del_and_expire);
    return FINISH();
}
