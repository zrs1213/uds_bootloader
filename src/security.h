/**
 * @file    security.h
 * @brief   UDS $27 SecurityAccess 的 Seed&Key 算法实现。
 *
 * 实现要点：
 *   - 采用 AES-128-ECB 作为"算法核心"（STM32F4 有硬件 CRYP 模块可 1:1 映射，
 *     本工程在 CANoe / Host 仿真环境下用纯 C 软件实现同一套接口，便于单元测试）。
 *   - 密钥派生：key = AES_ECB_Encrypt(MASTER_KEY, seed)
 *   - $27 01 返回 16 字节 seed；$27 02 收到 16 字节 key，校验是否与派生 key 一致。
 *
 * 注：seed 由可控 PRNG 产生，测试前用 SEC_RngSeed() 设定可复现序列，保证 KAT 可验。
 */
#ifndef UDS_SECURITY_H
#define UDS_SECURITY_H

#include <stdint.h>
#include <stddef.h>

#define SEC_SEED_LEN 16
#define SEC_KEY_LEN  16

/* AES-128-ECB 单块加密（in/out 均为 16 字节） */
void AES128_ECB_Encrypt(const uint8_t *key, const uint8_t *in, uint8_t *out);

/* 设定主密钥（算法秘密，烧录在 ECU 侧，诊断仪不可知） */
void SEC_Init(const uint8_t *master_key);

/* 设定 PRNG 种子，使 seed 序列可复现（测试用；真实环境用硬件 RNG 或 tick 熵） */
void SEC_RngSeed(uint32_t seed);

/* 生成一次 16 字节 seed（写入 seed 缓冲） */
void SEC_GenSeed(uint8_t *seed);

/* 由 seed 派生期望的 key：key = AES_ECB_Encrypt(MASTER_KEY, seed) */
void SEC_ComputeKey(const uint8_t *seed, uint8_t *key);

/* 校验诊断仪送来的 key 是否正确：1=通过，0=失败 */
uint8_t SEC_CheckKey(const uint8_t *seed, const uint8_t *key);

#endif /* UDS_SECURITY_H */
