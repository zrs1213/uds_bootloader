/**
 * @file    minunit.h
 * @brief   极简单元测试框架（断言语义对齐 Unity），零外部依赖。
 *
 * 之所以自带一个最小框架而非引入完整 Unity，是为了让工程在任意纯 C99
 * 环境下 `make test` 一条命令即可跑通，不依赖网络下载第三方库。
 * 断言宏命名刻意与 Unity 保持一致（TEST_ASSERT_*），便于后续替换为
 * 官方 Unity 而无须改测试用例。
 */
#ifndef MINUNIT_H
#define MINUNIT_H

#include <stdio.h>
#include <string.h>

extern int mu_tests_run;
extern int mu_tests_failed;
extern int mu_assertions;

/* 测试函数签名 */
typedef void (*mu_test_func)(void);

/* 断言失败时打印位置并置失败标志 */
#define MU_FAIL(msg) \
    do { \
        mu_tests_failed++; \
        printf("    FAIL @ %s:%d : %s\n", __FILE__, __LINE__, (msg)); \
        return; \
    } while (0)

#define TEST_ASSERT(cond) \
    do { \
        mu_assertions++; \
        if (!(cond)) { MU_FAIL("assert (" #cond ")"); } \
    } while (0)

#define TEST_ASSERT_EQUAL(a, b) \
    do { \
        mu_assertions++; \
        if ((a) != (b)) { \
            mu_tests_failed++; \
            printf("    FAIL @ %s:%d : expected %d, got %d (" #a "==" #b ")\n", \
                   __FILE__, __LINE__, (int)(a), (int)(b)); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_HEX(a, b) \
    do { \
        mu_assertions++; \
        if ((a) != (b)) { \
            mu_tests_failed++; \
            printf("    FAIL @ %s:%d : expected 0x%X, got 0x%X\n", \
                   __FILE__, __LINE__, (unsigned)(a), (unsigned)(b)); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_MEMORY(a, b, len) \
    do { \
        mu_assertions++; \
        if (memcmp((a), (b), (len)) != 0) { MU_FAIL("memory mismatch (" #a " vs " #b ")"); } \
    } while (0)

#define TEST_ASSERT_NULL(p) \
    do { \
        mu_assertions++; \
        if ((p) != 0) { MU_FAIL("expected NULL (" #p ")"); } \
    } while (0)

/* 测试组注册宏：运行一个测试函数，捕获其是否失败 */
#define MU_RUN_TEST(func) \
    do { \
        int _snap = mu_tests_failed; \
        mu_tests_run++; \
        printf("  %-28s", #func); \
        fflush(stdout); \
        (func)(); \
        if (mu_tests_failed == _snap) { printf("PASS\n"); } \
        else { printf("<<FAILED>>\n"); } \
    } while (0)

#endif /* MINUNIT_H */
