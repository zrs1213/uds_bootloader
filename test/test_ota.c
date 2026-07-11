/**
 * @file    test_ota.c
 * @brief   OTA 车端刷写代理单元测试。
 *
 * 覆盖：CBC-MAC 确定性 / 打包+验签 / 篡改拒签 / magic 错 / 非法块大小 /
 *        完整刷写闭环 / 验签失败不落盘 / 断点续传（故障注入后重传成功）。
 */
#include "minunit.h"
#include "ota_agent.h"
#include "security.h"
#include "uds_service.h"
#include "uds_io.h"
#include <string.h>

#define FW_LEN 70
static const uint8_t g_master[16] = {
    0xDE,0xAD,0xBE,0xEF,0x01,0x23,0x45,0x67,
    0x89,0xAB,0xCD,0xEF,0xFE,0xDC,0xBA,0x98
};
static uint8_t g_fw[FW_LEN];

static void fill_fw(void) {
    for (int i = 0; i < FW_LEN; i++) g_fw[i] = (uint8_t)(i * 7 + 3);
}

static void test_ota_cbc_mac_deterministic(void) {
    uint8_t k[16]; for (int i = 0; i < 16; i++) k[i] = (uint8_t)(i + 0x10);
    uint8_t d[40];   for (int i = 0; i < 40; i++) d[i] = (uint8_t)(i + 1);
    uint8_t m1[16], m2[16], m3[16];
    OTA_CbcMac(k, d, 40, m1);
    OTA_CbcMac(k, d, 40, m2);
    TEST_ASSERT_EQUAL_MEMORY(m1, m2, 16);   /* 同输入同签名 */
    d[5] ^= 0xFF;
    OTA_CbcMac(k, d, 40, m3);
    TEST_ASSERT(memcmp(m1, m3, 16) != 0);  /* 篡改后签名变 */
}

static void test_ota_build_verify_ok(void) {
    fill_fw();
    ota_agent_t a; OTA_Init(&a, g_master);
    uint8_t pkg[256];
    uint32_t len = OTA_BuildPackage(&a, UDS_FLASH_BASE, 32, g_fw, FW_LEN,
                                    pkg, (uint32_t)sizeof(pkg));
    TEST_ASSERT_EQUAL(118u, len);                     /* 48 + 70 */
    TEST_ASSERT_EQUAL(OTA_OK, OTA_Verify(&a, pkg, len));
}

static void test_ota_verify_tamper(void) {
    fill_fw();
    ota_agent_t a; OTA_Init(&a, g_master);
    uint8_t pkg[256];
    uint32_t len = OTA_BuildPackage(&a, UDS_FLASH_BASE, 32, g_fw, FW_LEN,
                                    pkg, (uint32_t)sizeof(pkg));
    uint8_t bad[256]; memcpy(bad, pkg, len);
    bad[OTA_HDR_SIZE + 5] ^= 0xFF;                  /* 篡改载荷，不重算签名 */
    TEST_ASSERT_EQUAL(OTA_ERR_SIGN, OTA_Verify(&a, bad, len));
}

static void test_ota_verify_bad_magic(void) {
    fill_fw();
    ota_agent_t a; OTA_Init(&a, g_master);
    uint8_t pkg[256];
    uint32_t len = OTA_BuildPackage(&a, UDS_FLASH_BASE, 32, g_fw, FW_LEN,
                                    pkg, (uint32_t)sizeof(pkg));
    pkg[0] = 'X';
    TEST_ASSERT_EQUAL(OTA_ERR_MAGIC, OTA_Verify(&a, pkg, len));
}

static void test_ota_build_bad_blocksz(void) {
    fill_fw();
    ota_agent_t a; OTA_Init(&a, g_master);
    uint8_t pkg[256];
    TEST_ASSERT_EQUAL(0u, OTA_BuildPackage(&a, UDS_FLASH_BASE, 0,  g_fw, FW_LEN,
                                            pkg, (uint32_t)sizeof(pkg)));
    TEST_ASSERT_EQUAL(0u, OTA_BuildPackage(&a, UDS_FLASH_BASE, 33, g_fw, FW_LEN,
                                            pkg, (uint32_t)sizeof(pkg)));
}

static void test_ota_flash_success(void) {
    fill_fw();
    SEC_Init(g_master);
    ota_agent_t a; OTA_Init(&a, g_master);
    uds_ctx_t ctx; UDS_Init(&ctx);
    uint8_t pkg[256];
    uint32_t len = OTA_BuildPackage(&a, UDS_FLASH_BASE, 32, g_fw, FW_LEN,
                                    pkg, (uint32_t)sizeof(pkg));
    ota_status_t st = OTA_Flash(&a, &ctx, pkg, len);
    TEST_ASSERT_EQUAL(OTA_OK, st);
    TEST_ASSERT(a.state == 4u);            /* done */
    TEST_ASSERT(a.need_rollback == 0u);
    TEST_ASSERT(a.total_blocks == 3u);    /* 70/32 -> 3 块 */
    TEST_ASSERT(a.written_blocks == 3u);
    TEST_ASSERT_EQUAL_MEMORY(ctx.flash.buf, g_fw, FW_LEN);  /* Flash == 固件 */
    TEST_ASSERT(a.fw_crc != 0u);
}

static void test_ota_flash_sign_fail_no_write(void) {
    fill_fw();
    SEC_Init(g_master);
    ota_agent_t a; OTA_Init(&a, g_master);
    uds_ctx_t ctx; UDS_Init(&ctx);   /* Flash 初始全 0xFF */
    uint8_t pkg[256];
    uint32_t len = OTA_BuildPackage(&a, UDS_FLASH_BASE, 32, g_fw, FW_LEN,
                                    pkg, (uint32_t)sizeof(pkg));
    uint8_t bad[256]; memcpy(bad, pkg, len);
    bad[OTA_HDR_SIZE + 5] ^= 0xFF;
    ota_status_t st = OTA_Flash(&a, &ctx, bad, len);
    TEST_ASSERT_EQUAL(OTA_ERR_SIGN, st);
    TEST_ASSERT(a.state == 5u);
    TEST_ASSERT(a.need_rollback == 1u);
    TEST_ASSERT(ctx.flash.buf[0] == 0xFFu);  /* 验签失败不应触发任何擦写 */
}

static void test_ota_resume_after_interrupt(void) {
    fill_fw();
    SEC_Init(g_master);
    ota_agent_t a; OTA_Init(&a, g_master);
    uds_ctx_t ctx; UDS_Init(&ctx);
    uint8_t pkg[256];
    uint32_t len = OTA_BuildPackage(&a, UDS_FLASH_BASE, 32, g_fw, FW_LEN,
                                    pkg, (uint32_t)sizeof(pkg));
    /* 第一次：刷 2 块后注入中断（模拟断电/掉线） */
    a.fail_after_block = 2;
    ota_status_t st1 = OTA_Flash(&a, &ctx, pkg, len);
    TEST_ASSERT_EQUAL(OTA_ERR_XFER, st1);
    TEST_ASSERT(a.written_blocks == 2u);
    TEST_ASSERT(a.state == 5u);
    /* 第二次：重新上电，完整重传（调度器决策），agent 进度保持 */
    a.fail_after_block = 0;
    ota_status_t st2 = OTA_Flash(&a, &ctx, pkg, len);
    TEST_ASSERT_EQUAL(OTA_OK, st2);
    TEST_ASSERT(a.total_blocks == 3u);
    TEST_ASSERT(a.written_blocks == 3u);
    TEST_ASSERT(a.state == 4u);
    TEST_ASSERT(a.need_rollback == 0u);
    TEST_ASSERT_EQUAL_MEMORY(ctx.flash.buf, g_fw, FW_LEN);
}

void run_ota_tests(void) {
    printf("\n[OTA Agent]\n");
    MU_RUN_TEST(test_ota_cbc_mac_deterministic);
    MU_RUN_TEST(test_ota_build_verify_ok);
    MU_RUN_TEST(test_ota_verify_tamper);
    MU_RUN_TEST(test_ota_verify_bad_magic);
    MU_RUN_TEST(test_ota_build_bad_blocksz);
    MU_RUN_TEST(test_ota_flash_success);
    MU_RUN_TEST(test_ota_flash_sign_fail_no_write);
    MU_RUN_TEST(test_ota_resume_after_interrupt);
}
