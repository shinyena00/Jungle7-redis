/*
 * check.h — 아주 작은 테스트 도우미 (의존성 없음)
 *
 *   CHECK(조건)   조건이 거짓이면 파일:줄과 조건을 찍고 실패로 센다. 테스트는 계속 진행.
 *   RUN(함수)     테스트 함수 하나를 실행하고 ok / FAIL을 찍는다.
 *   FINISH()      전체 결과를 찍고 종료 코드를 돌려준다 (실패 있으면 1).
 */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

static int g_checks_failed = 0;
static int g_tests_run = 0;
static int g_tests_failed = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                    #cond);                                                \
            g_checks_failed++;                                             \
        }                                                                  \
    } while (0)

#define RUN(fn)                                                            \
    do {                                                                   \
        int before_ = g_checks_failed;                                     \
        printf("%-48s", #fn);                                              \
        fflush(stdout);                                                    \
        fn();                                                              \
        g_tests_run++;                                                     \
        if (g_checks_failed == before_) {                                  \
            puts("ok");                                                    \
        } else {                                                           \
            puts("FAIL");                                                  \
            g_tests_failed++;                                              \
        }                                                                  \
        fflush(stdout);                                                    \
    } while (0)

#define FINISH()                                                           \
    (printf("\n%d tests, %d failed\n", g_tests_run, g_tests_failed),       \
     g_tests_failed ? 1 : 0)

#endif
