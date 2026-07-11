/**
 * @file    test_security.c
 * @brief   AES-128-ECB 已知答案测试(KAT) + $27 Seed&Key 派生闭环验证。
 */
#include "minunit.h"
#include "security.h"
#include <string.h>

static void test_aes_kat(void)
{
    /* FIPS-197 官方示例向量 */
    const uint8_t key[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                             0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    const uint8_t in[16]  = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                             0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    const uint8_t exp[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                             0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    uint8_t out[16];
    AES128_ECB_Encrypt(key, in, out);
    TEST_ASSERT_EQUAL_MEMORY(exp, out, 16);
}

static void test_seed_key_roundtrip(void)
{
    const uint8_t master[16] = {0xDE,0xAD,0xBE,0xEF,0x01,0x23,0x45,0x67,
                                0x89,0xAB,0xCD,0xEF,0xFE,0xDC,0xBA,0x98};
    SEC_Init(master);
    SEC_RngSeed(0xCAFEBABEu);

    uint8_t seed[SEC_SEED_LEN];
    SEC_GenSeed(seed);

    uint8_t key[SEC_KEY_LEN];
    SEC_ComputeKey(seed, key);

    /* 诊断仪侧用同样算法验证：应判定通过 */
    TEST_ASSERT_EQUAL(1, SEC_CheckKey(seed, key));

    /* 篡改 1 字节：应判定失败 */
    uint8_t bad[SEC_KEY_LEN];
    memcpy(bad, key, SEC_KEY_LEN);
    bad[0] ^= 0xFF;
    TEST_ASSERT_EQUAL(0, SEC_CheckKey(seed, bad));
}

static void test_deterministic_seed(void)
{
    const uint8_t master[16] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
                                0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x00};
    SEC_Init(master);
    SEC_RngSeed(0x1234u);
    uint8_t s1[SEC_SEED_LEN], s2[SEC_SEED_LEN];
    SEC_GenSeed(s1);
    SEC_RngSeed(0x1234u);   /* 复位 PRNG */
    SEC_GenSeed(s2);
    TEST_ASSERT_EQUAL_MEMORY(s1, s2, SEC_SEED_LEN);  /* 同种子 -> 同序列（可复现） */
}

void run_security_tests(void)
{
    printf("\n[Security / $27]\n");
    MU_RUN_TEST(test_aes_kat);
    MU_RUN_TEST(test_seed_key_roundtrip);
    MU_RUN_TEST(test_deterministic_seed);
}
