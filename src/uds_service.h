/**
 * @file    uds_service.h
 * @brief   UDS 应用层（ISO 14229-1）服务分发与协议状态机。
 *
 * 覆盖服务：
 *   $10 诊断会话控制        $11 ECU 复位
 *   $22 按 ID 读数据        $27 安全访问(Seed&Key)
 *   $2E 按 ID 写数据        $31 例程控制
 *   $28 通信控制(刷写前关非诊断通信)
 *   $34 请求下载            $36 数据传输            $37 请求退出传输
 *   $3E 诊断仪在线(TesterPresent)  $85 DTC 设置控制(刷写前关 DTC)
 *   —— 全部对齐工业级 UDS Bootloader 预编程(22 步)流程 ——
 *
 * 设计要点：
 *   - 每条请求经"会话权限 + 安全等级 + 长度"三重校验后再分发；
 *   - 负响应统一经 uds_build_neg() 生成 0x7F+SID+NRC；
 *   - 支持 sub-function 抑制位(0x80)，对应服务可抑制正响应。
 *   - 纯逻辑、无硬件依赖，可直接用字节数组单元测试。
 */
#ifndef UDS_SERVICE_H
#define UDS_SERVICE_H

#include <stdint.h>
#include "boot_fsm.h"
#include "uds_io.h"
#include "security.h"

#define UDS_S3_SERVER_MS   5000
#define SEC_MAX_ATTEMPTS   3
#define DL_BLOCK_MAX       32      /* 单个 $36 块最大净数据字节数 */

/* 一次下载传输的状态 */
typedef struct {
    uint8_t  active;
    uint32_t addr;
    uint32_t remaining;
    uint8_t  bsc;          /* 期望的下一个 blockSequenceCounter */
    uint32_t crc;          /* 运行中的 CRC32（刷写完整性校验） */
} uds_download_t;

/* 诊断栈上下文：会话状态 + 存储 + 安全 + 传输状态 */
typedef struct {
    uds_fsm_t      fsm;
    uds_flash_t    flash;
    uint8_t        sec_attempts;
    uint8_t        seed_valid;
    uint8_t        last_seed[SEC_SEED_LEN];
    uds_download_t dl;
    uint8_t        dtc_enabled;   /* 1=开启 DTC 设置(默认) 0=已关闭($85 02) */
    uint8_t        comm_disabled; /* 0=通信正常; 非0=非诊断通信被禁用($28 设置) */
} uds_ctx_t;

void UDS_Init(uds_ctx_t *c);

/**
 * 处理一条完整的 UDS 请求 PDU，产出响应 PDU。
 * @return 0  响应已写入 resp（正响应或负响应）
 *         1  无响应（sub-function 抑制位生效，或无需响应）
 */
uint8_t UDS_ProcessRequest(uds_ctx_t *c,
                           const uint8_t *req, uint16_t req_len,
                           uint8_t *resp, uint16_t *resp_len);

#endif /* UDS_SERVICE_H */
