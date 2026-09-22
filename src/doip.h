/**
 * @file    doip.h
 * @brief   DoIP 传输层（ISO 13400-2）—— 让同一条 UDS 协议栈跑在 IP 上。
 *
 * 与 can_tp.h 平级的第二传输层：CAN-TP 解决"UDS 走 8 字节 CAN 帧"，
 * DoIP 解决"UDS 走 TCP/UDP"。两者都只做传输/分段/寻址，
 * 最终都汇入同一个 UDS_ProcessRequest()，应用层零改动。
 *
 * 实现范围（本项目支持的消息集）：
 *   0x0000 Vehicle announcement request      （UDP，ECU 主动/被动应答）
 *   0x0001 Vehicle identification request    （VIN 匹配则应答 0x0002）
 *   0x0002 Vehicle identification response   （68 字节标准布局）
 *   0x0005 Routing activation request        （诊断仪注册）
 *   0x0006 Routing activation response       （0x10 成功 / 0x11 已激活 / 拒绝码）
 *   0x0007/0x0008 Alive check                （保活探测）
 *   0x8001 Diagnostic message                （SA+TA+UDS PDU，TCP 承载）
 *   0x8002 Diagnostic message ACK            （先 ACK 后响应，两条报文）
 *   0x8003 Diagnostic message NACK           （SA/TA/激活状态校验失败）
 *
 * 设计约束（与全工程一致）：
 *   - 纯逻辑、零 I/O、零动态内存：TCP socket 由 examples/ 侧适配，
 *     本层只处理字节流 → 消息回调，可直接进 Cortex-M4 交叉编译。
 *   - 流式解帧器 DOIP_StreamFeed() 处理 TCP 粘包/半包，坏头逐字节重同步。
 *
 * 注：0x0002 载荷布局（VIN17+逻辑地址2+EID6+FurtherID6+GID/FID有效1+
 * GID8+FID8+IPv4 4+IPv6 16 = 68B）按业界通用实现，已在本工程 C↔Python
 * 两端互验；与第三方协议栈互通前建议对照 ISO 13400-2 原文逐字段核对。
 */
#ifndef DOIP_H
#define DOIP_H

#include <stdint.h>
#include "uds_service.h"

#define DOIP_VERSION      0x02u
#define DOIP_INV_VERSION  0xFDu
#define DOIP_HDR_LEN      8u

/* 单条消息载荷上限：诊断报文 = SA2+TA2+UDS4095，取整留余量 */
#define DOIP_MAX_PAYLOAD  4104u
#define DOIP_STREAM_BUF   (DOIP_HDR_LEN + DOIP_MAX_PAYLOAD)

/* ---------------- 载荷类型（ISO 13400-2 Table 10） ---------------- */
#define DOIP_PT_ANNOUNCEMENT_REQ   0x0000u
#define DOIP_PT_VEH_ID_REQ_VIN     0x0001u
#define DOIP_PT_VEH_ID_RESP        0x0002u
#define DOIP_PT_ROUTING_ACT_REQ    0x0005u
#define DOIP_PT_ROUTING_ACT_RESP   0x0006u
#define DOIP_PT_ALIVE_CHECK_REQ    0x0007u
#define DOIP_PT_ALIVE_CHECK_RESP   0x0008u
#define DOIP_PT_DIAG_MESSAGE       0x8001u
#define DOIP_PT_DIAG_MESSAGE_ACK   0x8002u
#define DOIP_PT_DIAG_MESSAGE_NACK  0x8003u

/* ---------------- 路由激活响应码（ISO 13400-2 Table 12 子集） -------- */
#define DOIP_RAR_REJECTED_UNSPEC      0x00u
#define DOIP_RAR_REJECTED_UNKNOWN_SA  0x01u /* 诊断仪逻辑地址非法(0x0000) */
#define DOIP_RAR_REJECTED_FORMAT      0x04u /* 请求长度/格式错误 */
#define DOIP_RAR_SUCCESS              0x10u
#define DOIP_RAR_ALREADY_ACTIVE       0x11u /* 同一 SA 重复激活 */

/* ---------------- 诊断消息 NACK 码（ISO 13400-2 Table 18 子集） ------ */
#define DOIP_NACK_INVALID_SA          0x01u /* 源地址 ≠ 已注册诊断仪 */
#define DOIP_NACK_UNKNOWN_TA          0x02u /* 目的地址 ≠ 本 ECU */
/* 0x04 为本工程自定义：未完成路由激活就发诊断报文（规范外取值，见 README） */
#define DOIP_NACK_NOT_ACTIVATED       0x04u

/* ---------------- 头部编解码 ---------------- */

/* 写入 8 字节头，返回 DOIP_HDR_LEN */
uint32_t DOIP_WriteHeader(uint8_t *out, uint16_t type, uint32_t payload_len);

/**
 * 解析 8 字节头。
 * @return 0 成功 | 1 数据不足需继续收 | -1 版本/反码非法（坏头，应丢 1 字节重同步）
 */
int DOIP_ReadHeader(const uint8_t *buf, uint32_t avail,
                    uint16_t *type, uint32_t *payload_len);

/* ---------------- TCP 流式解帧器（处理粘包/半包/坏头重同步） ---------- */

typedef struct {
    uint8_t  buf[DOIP_STREAM_BUF];
    uint32_t len;
} doip_stream_t;

void DOIP_StreamInit(doip_stream_t *s);

/* 每解析出一条完整消息回调一次 */
typedef void (*doip_msg_cb)(uint16_t type, const uint8_t *payload,
                            uint32_t len, void *user);

/**
 * 向流中喂入收到的字节；内部缓存并循环切分完整消息。
 * 超长/坏头按字节丢弃重同步，不会死锁。
 */
void DOIP_StreamFeed(doip_stream_t *s, const uint8_t *data, uint32_t n,
                     doip_msg_cb cb, void *user);

/* ---------------- ECU 侧消息分发 ---------------- */

/* 发送回调：交出一待发送的载荷（不含头），由适配层加 8 字节头写入 socket。
 * 载荷缓冲区所有权在协议层调用期间有效，适配层若需异步发送必须自行拷贝。 */
typedef void (*doip_tx_fn)(uint16_t type, const uint8_t *payload,
                           uint32_t len, void *user);

typedef struct {
    uint16_t ecu_addr;      /* 本 ECU 逻辑地址，如 0x0E80 */
    uint16_t tester_addr;   /* 已注册诊断仪地址，0=未注册 */
    uint8_t  routing_active;
    uint8_t  vin[17];       /* 用于 0x0001 VIN 匹配 */
} doip_ecu_t;

void DOIP_EcuInit(doip_ecu_t *d, uint16_t ecu_addr, const char *vin17);

/**
 * 处理一条完整接收消息（解帧器回调里直接转调本函数）。
 * 内部完成：路由激活状态机、诊断消息 SA/TA 校验、ACK/NACK、
 * 以及把 UDS PDU 交给 UDS_ProcessRequest() 并回封 0x8001。
 */
void DOIP_EcuOnMessage(doip_ecu_t *d, uds_ctx_t *uds,
                       uint16_t type, const uint8_t *p, uint32_t len,
                       doip_tx_fn tx, void *user);

/* ---------------- 便捷构建 ---------------- */

/* 构建一条诊断消息（头+SA+TA+UDS），返回总长 */
uint32_t DOIP_BuildDiagMessage(uint8_t *out, uint16_t sa, uint16_t ta,
                               const uint8_t *uds, uint32_t uds_len);

#endif /* DOIP_H */
