/**
 * @file    test_uds.c
 * @brief   UDS 应用层单元测试：会话权限、各服务正/负响应、NRC 覆盖、S3 超时。
 */
#include "minunit.h"
#include "uds_service.h"
#include "security.h"
#include <string.h>

static uds_ctx_t g_ctx;
static uint8_t  g_resp[512];
static uint16_t g_rlen;

/* 便捷：初始化并设置主密钥（与主密钥一致才能算出正确 key） */
static void setup(void) {
    UDS_Init(&g_ctx);
    const uint8_t master[16] = {0xDE,0xAD,0xBE,0xEF,0x01,0x23,0x45,0x67,
                                0x89,0xAB,0xCD,0xEF,0xFE,0xDC,0xBA,0x98};
    SEC_Init(master);
    SEC_RngSeed(0xCAFEBABEu);
}

/* 运行一条请求，返回 UDS_ProcessRequest 的返回值(0=有响应,1=抑制) */
static uint8_t run(const uint8_t *req, uint16_t rlen) {
    return UDS_ProcessRequest(&g_ctx, req, rlen, g_resp, &g_rlen);
}

/* ============ 会话 ============ */
static void test_default_session(void) {
    setup();
    TEST_ASSERT_EQUAL(0x01, UDS_FsmSession(&g_ctx.fsm));
}

static void test_session_switch_extended(void) {
    setup();
    uint8_t req[] = {0x10, 0x03};
    TEST_ASSERT_EQUAL(0, run(req, 2));
    TEST_ASSERT_EQUAL(0x50, g_resp[0]);
    TEST_ASSERT_EQUAL(0x03, g_resp[1]);
    TEST_ASSERT_EQUAL(0x03, UDS_FsmSession(&g_ctx.fsm));
    /* 切换会话后安全等级应复位 */
    TEST_ASSERT_EQUAL(0, UDS_FsmSecurity(&g_ctx.fsm));
}

/* ============ $22 读 DID ============ */
static void test_read_vin(void) {
    setup();
    uint8_t req[] = {0x22, 0xF1, 0x90};
    TEST_ASSERT_EQUAL(0, run(req, 3));
    TEST_ASSERT_EQUAL(0x62, g_resp[0]);
    TEST_ASSERT_EQUAL(0xF1, g_resp[1]);
    TEST_ASSERT_EQUAL(0x90, g_resp[2]);
    TEST_ASSERT_EQUAL('L', g_resp[3]);
    TEST_ASSERT_EQUAL(16 + 3, g_rlen);
}

static void test_read_unknown_did(void) {
    setup();
    uint8_t req[] = {0x22, 0x99, 0x99};
    TEST_ASSERT_EQUAL(0, run(req, 3));
    TEST_ASSERT_EQUAL(0x7F, g_resp[0]);
    TEST_ASSERT_EQUAL(0x22, g_resp[1]);
    TEST_ASSERT_EQUAL(0x31, g_resp[2]);   /* requestOutOfRange */
}

/* ============ $27 安全访问 ============ */
static void test_security_in_default_denied(void) {
    setup();
    uint8_t req[] = {0x27, 0x01};
    TEST_ASSERT_EQUAL(0, run(req, 2));
    TEST_ASSERT_EQUAL(0x7F, g_resp[0]);
    TEST_ASSERT_EQUAL(0x27, g_resp[1]);
    TEST_ASSERT_EQUAL(0x7F, g_resp[2]);   /* serviceNotSupportedInSession */
}

static void test_security_full_flow(void) {
    setup();
    uint8_t req1[] = {0x10, 0x03};
    run(req1, 2);                          /* 进入扩展会话 */

    uint8_t req2[] = {0x27, 0x01};
    TEST_ASSERT_EQUAL(0, run(req2, 2));    /* 请求 seed */
    TEST_ASSERT_EQUAL(0x67, g_resp[0]);
    TEST_ASSERT_EQUAL(0x01, g_resp[1]);
    TEST_ASSERT_EQUAL(18, g_rlen);         /* 0x67 0x01 + 16 字节 seed */

    uint8_t seed[16];
    memcpy(seed, g_resp + 2, 16);

    uint8_t key[16];
    SEC_ComputeKey(seed, key);             /* 诊断仪侧派生 key */

    uint8_t req3[18];
    req3[0] = 0x27; req3[1] = 0x02;
    memcpy(req3 + 2, key, 16);
    TEST_ASSERT_EQUAL(0, run(req3, 18));   /* 提交 key */
    TEST_ASSERT_EQUAL(0x67, g_resp[0]);
    TEST_ASSERT_EQUAL(0x02, g_resp[1]);
    TEST_ASSERT_EQUAL(1, UDS_FsmSecurity(&g_ctx.fsm));  /* 已解锁 */
}

static void test_security_invalid_key(void) {
    setup();
    uint8_t req1[] = {0x10, 0x03}; run(req1, 2);
    uint8_t req2[] = {0x27, 0x01}; run(req2, 2);
    uint8_t bad[16];
    memset(bad, 0x00, 16);
    uint8_t req3[18];
    req3[0]=0x27; req3[1]=0x02; memcpy(req3+2, bad, 16);
    TEST_ASSERT_EQUAL(0, run(req3, 18));
    TEST_ASSERT_EQUAL(0x7F, g_resp[0]);
    TEST_ASSERT_EQUAL(0x35, g_resp[2]);   /* invalidKey */
}

static void test_security_key_without_seed(void) {
    setup();
    uint8_t req1[] = {0x10, 0x03}; run(req1, 2);
    uint8_t req[] = {0x27, 0x02, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    TEST_ASSERT_EQUAL(0, run(req, 18));
    TEST_ASSERT_EQUAL(0x24, g_resp[2]);   /* requestSequenceError */
}

/* ============ $2E 写 DID ============ */
static void test_write_did(void) {
    setup();
    uint8_t req1[] = {0x10, 0x03}; run(req1, 2);
    uint8_t req[] = {0x2E, 0x02, 0x01, 0x0A, 0x0B, 0x0C, 0x0D};
    TEST_ASSERT_EQUAL(0, run(req, 7));
    TEST_ASSERT_EQUAL(0x6E, g_resp[0]);
    TEST_ASSERT_EQUAL(0x02, g_resp[1]);
    TEST_ASSERT_EQUAL(0x01, g_resp[2]);
    /* 回读验证 */
    uint8_t rd[] = {0x22, 0x02, 0x01};
    run(rd, 3);
    TEST_ASSERT_EQUAL(0x0A, g_resp[3]);
    TEST_ASSERT_EQUAL(0x0D, g_resp[6]);
}

/* ============ $31 例程控制 ============ */
static void test_routine_crc(void) {
    setup();
    uint8_t req1[] = {0x10, 0x03}; run(req1, 2);
    uint8_t req[] = {0x31, 0x01, 0xFF, 0x00};  /* 计算 Flash CRC */
    TEST_ASSERT_EQUAL(0, run(req, 4));
    TEST_ASSERT_EQUAL(0x71, g_resp[0]);
    TEST_ASSERT_EQUAL(9, g_rlen);
}

static void test_routine_erase(void) {
    setup();
    uint8_t req1[] = {0x10, 0x03}; run(req1, 2);
    uint8_t req[] = {0x31, 0x01, 0x02, 0x01};  /* 擦除 APP 区 */
    TEST_ASSERT_EQUAL(0, run(req, 4));
    TEST_ASSERT_EQUAL(0x71, g_resp[0]);
    TEST_ASSERT_EQUAL(5, g_rlen);
}

/* ============ $34/$36/$37 刷写三件套 ============ */
static void test_programming_flow(void) {
    setup();
    uint8_t req_s[] = {0x10, 0x02}; run(req_s, 2);  /* 编程会话 */
    /* 解锁安全 */
    uint8_t req_seed[] = {0x27, 0x01}; run(req_seed, 2);
    uint8_t seed[16]; memcpy(seed, g_resp+2, 16);
    uint8_t key[16]; SEC_ComputeKey(seed, key);
    uint8_t req_key[18]; req_key[0]=0x27; req_key[1]=0x02; memcpy(req_key+2,key,16);
    run(req_key, 18);

    /* $34 请求下载：addr=flash.base, size=0x20(32) */
    uint32_t addr = g_ctx.flash.base;
    uint8_t req34[11] = {0x34, 0x00, 0x44,
        (uint8_t)(addr>>24),(uint8_t)(addr>>16),(uint8_t)(addr>>8),(uint8_t)addr,
        0x00,0x00,0x00,0x20};
    TEST_ASSERT_EQUAL(0, run(req34, 11));
    TEST_ASSERT_EQUAL(0x74, g_resp[0]);
    TEST_ASSERT_EQUAL(0x20, g_resp[1]);

    /* $36 块1 (10 字节) */
    uint8_t block1[12];
    block1[0]=0x36; block1[1]=0x01;
    for (int i=0;i<10;i++) block1[2+i]=(uint8_t)(i+0x10);
    TEST_ASSERT_EQUAL(0, run(block1, 12));
    TEST_ASSERT_EQUAL(0x76, g_resp[0]);
    TEST_ASSERT_EQUAL(0x01, g_resp[1]);

    /* $36 块2 (剩余 22 字节) */
    uint8_t block2[24];
    block2[0]=0x36; block2[1]=0x02;
    for (int i=0;i<22;i++) block2[2+i]=(uint8_t)(0x20+i);
    TEST_ASSERT_EQUAL(0, run(block2, 24));
    TEST_ASSERT_EQUAL(0x76, g_resp[0]);

    /* $37 退出 */
    uint8_t req37[] = {0x37};
    TEST_ASSERT_EQUAL(0, run(req37, 1));
    TEST_ASSERT_EQUAL(0x77, g_resp[0]);

    /* 校验 Flash 写入内容 */
    uint8_t rb[32];
    UDS_FlashRead(&g_ctx.flash, addr, rb, 32);
    for (int i=0;i<10;i++) TEST_ASSERT_EQUAL((uint8_t)(i+0x10), rb[i]);
    for (int i=0;i<22;i++) TEST_ASSERT_EQUAL((uint8_t)(0x20+i), rb[10+i]);
}

static void test_download_security_denied(void) {
    setup();
    uint8_t req_s[] = {0x10, 0x02}; run(req_s, 2);  /* 编程会话但安全锁定 */
    uint8_t req34[11] = {0x34, 0x00, 0x44,
        0x08,0x01,0x00,0x00, 0x00,0x00,0x00,0x20};
    TEST_ASSERT_EQUAL(0, run(req34, 11));
    TEST_ASSERT_EQUAL(0x33, g_resp[2]);   /* securityAccessDenied */
}

static void test_download_wrong_session(void) {
    setup();
    uint8_t req[] = {0x22, 0xF1, 0x90}; run(req, 3); /* 默认会话 */
    uint8_t req34[11] = {0x34, 0x00, 0x44,
        0x08,0x01,0x00,0x00, 0x00,0x00,0x00,0x20};
    /* 默认会话下 $34 不被允许 */
    TEST_ASSERT_EQUAL(0, run(req34, 11));
    TEST_ASSERT_EQUAL(0x7F, g_resp[2]);   /* serviceNotSupportedInSession */
}

/* ============ S3 超时 ============ */
static void test_s3_timeout(void) {
    setup();
    uint8_t req[] = {0x10, 0x03}; run(req, 2);
    TEST_ASSERT_EQUAL(0x03, UDS_FsmSession(&g_ctx.fsm));
    uint8_t fell = UDS_FsmTick(&g_ctx.fsm, 6000); /* 超过 S3(5000ms) */
    TEST_ASSERT_EQUAL(1, fell);
    TEST_ASSERT_EQUAL(0x01, UDS_FsmSession(&g_ctx.fsm));
}

/* ============ $3E TesterPresent ============ */
static void test_tester_present_normal(void) {
    setup();
    uint8_t req[] = {0x3E, 0x00};
    TEST_ASSERT_EQUAL(0, run(req, 2));
    TEST_ASSERT_EQUAL(0x7E, g_resp[0]);
}
static void test_tester_present_suppress(void) {
    setup();
    uint8_t req[] = {0x3E, 0x80};   /* 抑制位 */
    TEST_ASSERT_EQUAL(1, run(req, 2)); /* 无响应 */
}

/* ============ $11 复位 ============ */
static void test_ecu_reset(void) {
    setup();
    uint8_t req_s[] = {0x10, 0x03}; run(req_s, 2);
    uint8_t req[] = {0x11, 0x01};
    TEST_ASSERT_EQUAL(0, run(req, 2));
    TEST_ASSERT_EQUAL(0x51, g_resp[0]);
    TEST_ASSERT_EQUAL(0x01, UDS_FsmSession(&g_ctx.fsm)); /* 复位回落默认 */
}

/* =========== $85 控制 DTC 设置（刷写前关/后开） =========== */
static void test_control_dtc(void) {
    setup();
    uint8_t req_s[] = {0x10, 0x03}; run(req_s, 2);
    uint8_t off[] = {0x85, 0x02};
    TEST_ASSERT_EQUAL(0, run(off, 2));
    TEST_ASSERT_EQUAL(0xC5, g_resp[0]);
    TEST_ASSERT_EQUAL(0x02, g_resp[1]);
    TEST_ASSERT_EQUAL(0, g_ctx.dtc_enabled);
    uint8_t on[] = {0x85, 0x01};
    TEST_ASSERT_EQUAL(0, run(on, 2));
    TEST_ASSERT_EQUAL(0x01, g_resp[1]);
    TEST_ASSERT_EQUAL(1, g_ctx.dtc_enabled);
}

/* =========== $28 通信控制（刷写前关非诊断通信） =========== */
static void test_comm_control(void) {
    setup();
    uint8_t req_s[] = {0x10, 0x03}; run(req_s, 2);
    uint8_t dis[] = {0x28, 0x03};
    TEST_ASSERT_EQUAL(0, run(dis, 2));
    TEST_ASSERT_EQUAL(0x68, g_resp[0]);
    TEST_ASSERT_EQUAL(0x03, g_resp[1]);
    TEST_ASSERT_EQUAL(0x03, g_ctx.comm_disabled);
    uint8_t en[] = {0x28, 0x00};
    TEST_ASSERT_EQUAL(0, run(en, 2));
    TEST_ASSERT_EQUAL(0x00, g_ctx.comm_disabled);
}

/* =========== 工业级预编程条件检查 $31 FF01 =========== */
static void test_routine_precond(void) {
    setup();
    uint8_t req_s[] = {0x10, 0x03}; run(req_s, 2);
    uint8_t req[] = {0x31, 0x01, 0xFF, 0x01};
    TEST_ASSERT_EQUAL(0, run(req, 4));
    TEST_ASSERT_EQUAL(0x71, g_resp[0]);
    TEST_ASSERT_EQUAL(5, g_rlen);
    TEST_ASSERT_EQUAL(0x00, g_resp[4]);  /* routineStatus: 条件满足 */
}

void run_uds_tests(void)
{
    printf("\n[UDS Application]\n");
    MU_RUN_TEST(test_default_session);
    MU_RUN_TEST(test_session_switch_extended);
    MU_RUN_TEST(test_read_vin);
    MU_RUN_TEST(test_read_unknown_did);
    MU_RUN_TEST(test_security_in_default_denied);
    MU_RUN_TEST(test_security_full_flow);
    MU_RUN_TEST(test_security_invalid_key);
    MU_RUN_TEST(test_security_key_without_seed);
    MU_RUN_TEST(test_write_did);
    MU_RUN_TEST(test_routine_crc);
    MU_RUN_TEST(test_routine_erase);
    MU_RUN_TEST(test_programming_flow);
    MU_RUN_TEST(test_download_security_denied);
    MU_RUN_TEST(test_download_wrong_session);
    MU_RUN_TEST(test_s3_timeout);
    MU_RUN_TEST(test_tester_present_normal);
    MU_RUN_TEST(test_tester_present_suppress);
    MU_RUN_TEST(test_ecu_reset);
    MU_RUN_TEST(test_control_dtc);
    MU_RUN_TEST(test_comm_control);
    MU_RUN_TEST(test_routine_precond);
}
