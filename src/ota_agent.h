/**
 * @file    ota_agent.h
 * @brief   车端 OTA 刷写代理（轻量级"OTA 调度层"）。
 *
 * 角色定位：云端把升级包经 T-Box 下发到车端后，本模块充当"车端刷写客户端"，
 * 完成两件事：
 *   1) 包级鉴权 —— 对升级包做 CBC-MAC（AES-128, IV=0）验签，防止刷入被
 *      篡改/伪造的固件（比 $27 Seed&Key 的"链路鉴权"更进一步，是"包鉴权"）；
 *   2) 驱动刷写 —— 验签通过后，复用已验证的 UDS 应用层（$34/$36/$37/$31 FF00）
 *      把固件写进 Flash，并维护块级进度（断点续传/心跳上报）与失败回滚标记。
 *
 * 设计要点：
 *   - 不重新实现任何刷写逻辑，全部经 UDS_ProcessRequest() 复用现有协议栈；
 *   - 与平台无关：升级包由上层经"虚拟通道"投递（本模块只认字节数组），
 *     真实环境替换为 T-Box 的 4G/以太网收包即可；
 *   - CBC-MAC 直接复用 security.c 的 AES128_ECB_Encrypt 原语。
 *
 * 注意（边界，简历/面试务必如实）：本模块实现"车端验签 + 刷写引擎驱动"，
 * 云端编排、差分升级、A/B 双分区回滚属车企平台级工作，此处用 need_rollback
 * 标志位表达"回滚决策"的设计意图，不做整包平台。
 */
#ifndef OTA_AGENT_H
#define OTA_AGENT_H

#include <stdint.h>
#include "uds_service.h"   /* 引入 uds_ctx_t / DL_BLOCK_MAX / AES 等 */

/* 升级包头部固定 32 字节；末尾 16 字节 CBC-MAC 签名；开销合计 48 字节 */
#define OTA_HDR_SIZE       32
#define OTA_SIG_SIZE       16
#define OTA_PKG_OVERHEAD   (OTA_HDR_SIZE + OTA_SIG_SIZE)   /* = 48 */

typedef enum {
    OTA_OK = 0,
    OTA_ERR_PKG_SHORT,   /* 包长度不足 */
    OTA_ERR_MAGIC,       /* magic 不为 "OTAP" */
    OTA_ERR_BLOCKSZ,     /* block_size 非法（必须 1..DL_BLOCK_MAX） */
    OTA_ERR_ADDR,        /* 目标地址越出 Flash 区间 */
    OTA_ERR_SIGN,        /* CBC-MAC 验签失败（包被篡改/密钥不符） */
    OTA_ERR_SESSION,     /* 无法进入编程会话 */
    OTA_ERR_SECURITY,    /* $27 安全解锁失败 */
    OTA_ERR_DOWNLOAD,    /* $34 请求下载被拒 */
    OTA_ERR_XFER,        /* 某块 $36 传输被拒（或故障注入中断） */
    OTA_ERR_EXIT,        /* $37 退出传输被拒 */
    OTA_ERR_CRC,         /* $31 FF00 CRC 校验不匹配 */
} ota_status_t;

typedef struct {
    uint8_t  master[16];        /* 与车端 ECU 共享的主密钥（CBC-MAC 验签用） */
    uint32_t fw_addr;            /* 本次升级目标地址 */
    uint32_t fw_len;            /* 固件长度 */
    uint16_t block_size;        /* 每块净数据字节数（1..DL_BLOCK_MAX） */
    uint32_t total_blocks;      /* 固件分块总数 */
    uint16_t written_blocks;    /* 已成功写入块数（断点续传/进度上报） */
    uint8_t  state;            /* 0 idle 1 received 2 verified 3 flashing 4 done 5 failed */
    uint8_t  need_rollback;    /* 失败回滚标记（A/B 分区回退决策） */
    uint32_t fw_crc;           /* 刷写后 CRC32（由 $31 FF00 取回比对） */
    uint16_t fail_after_block;  /* 故障注入：!=0 时在写完该块后强制中止（仅测试用） */
} ota_agent_t;

/* 初始化：注入共享主密钥，清零状态 */
void OTA_Init(ota_agent_t *a, const uint8_t *master_key);

/* CBC-MAC（AES-128, IV=0，末块 0 填充），输出 16 字节签名 */
void OTA_CbcMac(const uint8_t *key, const uint8_t *data, uint32_t len, uint8_t *out16);

/**
 * 打包：把固件打成合法 OTA 升级包（含 CBC-MAC 签名）。
 * @return 包总长（= OTA_PKG_OVERHEAD + fw_len）；空间不足或参数非法返回 0。
 */
uint32_t OTA_BuildPackage(ota_agent_t *a, uint32_t fw_addr, uint16_t block_size,
                          const uint8_t *fw, uint32_t fw_len,
                          uint8_t *out_pkg, uint32_t max_pkg);

/* 验签：CBC-MAC 比对 + 基本格式/地址校验。返回 OTA_OK / 对应错误码。 */
ota_status_t OTA_Verify(const ota_agent_t *a, const uint8_t *pkg, uint32_t pkg_len);

/**
 * 完整刷写：验签 -> 编程会话+解锁 -> $34 -> $36×N -> $37 -> $31 FF00。
 * @param uds 车端 ECU 上下文（由调用方 UDS_Init 后传入；与 agent 共享同一主密钥）。
 */
ota_status_t OTA_Flash(ota_agent_t *a, uds_ctx_t *uds,
                       const uint8_t *pkg, uint32_t pkg_len);

#endif /* OTA_AGENT_H */
