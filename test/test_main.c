/**
 * @file    test_main.c
 * @brief   单元测试主入口：定义 minunit 全局计数器并提供 main()。
 */
#include "minunit.h"
#include <stdio.h>

int mu_tests_run   = 0;
int mu_tests_failed = 0;
int mu_assertions  = 0;

/* 各测试套件入口（定义于对应 .c） */
void run_can_tp_tests(void);
void run_security_tests(void);
void run_uds_tests(void);
void run_stack_tests(void);
void run_ota_tests(void);

int main(void) {
    printf("========================================\n");
    printf(" UDS Bootloader Protocol Stack - Tests\n");
    printf("========================================");
    run_can_tp_tests();
    run_security_tests();
    run_uds_tests();
    run_stack_tests();
    run_ota_tests();

    printf("\n----------------------------------------\n");
    if (mu_tests_failed == 0) {
        printf(" RESULT: ALL PASS  (%d tests, %d assertions)\n",
               mu_tests_run, mu_assertions);
    } else {
        printf(" RESULT: %d FAILED / %d tests, %d assertions\n",
               mu_tests_failed, mu_tests_run, mu_assertions);
    }
    printf("----------------------------------------\n");
    printf("\n按回车键退出...");
    getchar();
    return mu_tests_failed ? 1 : 0;
}
