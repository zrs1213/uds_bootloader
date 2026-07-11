/**
 * @file    types.h
 * @brief   公共类型定义：与 STM32 CAN 外设邮箱格式完全兼容的报文结构体，
 *          以及 CAN-TP / UDS 层用于解耦底层硬件的发送/接收回调抽象。
 *
 * 设计意图：协议栈不直接操作 CAN 寄存器，所有帧的实际收发都通过上层注册的
 * 回调函数完成。在 CANoe 验证时由 CAPL 脚本实现这两个回调；移植到 STM32
 * 时由 HAL_CAN 驱动实现。接口不变，协议层代码一行不动。
 */
#ifndef UDS_TYPES_H
#define UDS_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* 标准 CAN 数据帧最大字节数（CAN 2.0B 数据场为 8 字节） */
#define CAN_MAX_DLC 8

/* 11-bit 标准帧 ID 范围 */
typedef uint32_t can_id_t;

/**
 * CAN 报文结构体 —— 字段布局刻意对齐 STM32 HAL 库的 CAN_TxHeaderTypeDef /
 * 邮箱寄存器：8 字节 data、4 字节 ID、1 字节 DLC。
 */
typedef struct {
    can_id_t id;        /* CAN ID (11-bit 标准帧，或 29-bit 扩展帧) */
    uint8_t  dlc;       /* Data Length Code (0-8) */
    uint8_t  data[CAN_MAX_DLC];
    uint32_t timestamp; /* 时间戳（仿真/测试用，真实 MCU 由硬件填充） */
} can_msg_t;

/* 发送回调：协议栈调用它把一帧 CAN 报文真正发出去 */
typedef void (*can_tx_callback_t)(const can_msg_t *msg);
/* 接收回调：底层 CAN 驱动收到一帧后调用它交给协议栈处理 */
typedef void (*can_rx_callback_t)(const can_msg_t *msg);

/* 定时器接口：返回当前毫秒计数（由平台提供，测试时可替换为 mock 时钟） */
typedef uint32_t (*cantp_get_tick_t)(void);

/* CAN-TP 默认寻址 ID（物理寻址，与方案 DBC 一致） */
#define CANTP_TX_ID 0x7E0   /* 诊断仪 -> ECU */
#define CANTP_RX_ID 0x7E8   /* ECU -> 诊断仪 */

#endif /* UDS_TYPES_H */
