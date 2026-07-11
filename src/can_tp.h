/**
 * @file    can_tp.h
 * @brief   ISO 15765-2 (CAN Transport Layer) 传输层协议栈实现。
 *
 * 提供：
 *   - 四种帧（SF/FF/CF/FC）的构建与 PCI 解析
 *   - 接收端多帧重组状态机（含 BS 流控、SN 序列号校验）
 *   - 发送端分段状态机（含 STmin 帧间隔、BS 流控、FC 等待）
 *
 * 所有帧的实际收发通过 can_tx_callback_t 解耦，协议层不直接触碰 CAN 寄存器。
 */
#ifndef CAN_TP_H
#define CAN_TP_H

#include "types.h"

/* 接收缓冲最大长度（ISO-TP 单条报文上限 4095 字节，留余量） */
#define CANTP_RX_BUF_SIZE 4096

/* PCI 高 4 位：帧类型 */
typedef enum {
    CANTP_PCI_SF = 0,  /* Single Frame   0x0x */
    CANTP_PCI_FF = 1,  /* First Frame    0x1x */
    CANTP_PCI_CF = 2,  /* Consecutive    0x2x */
    CANTP_PCI_FC = 3   /* Flow Control   0x3x */
} cantp_pci_type_t;

/* Flow Control 流控状态 */
typedef enum {
    CANTP_FS_CTS   = 0, /* Clear To Send —— 继续发送 */
    CANTP_FS_WAIT  = 1, /* Wait          —— 暂停，等待下个 FC */
    CANTP_FS_OVFLW = 2  /* Overflow      —— 缓冲溢出，中止 */
} cantp_fs_t;

typedef enum {
    CANTP_OK            = 0,
    CANTP_ERR_LENGTH,    /* SF 数据超 7 字节 / 总长超过 4095 */
    CANTP_ERR_UNEXPECTED,/* 状态机在不该收到某帧类型时收到 */
    CANTP_ERR_SEQUENCE,  /* CF 序列号不连续 */
    CANTP_ERR_OVERFLOW,  /* 接收缓冲溢出 */
    CANTP_ERR_BUSY,      /* 正在接收上一帧，不能开始新消息 */
    CANTP_ERR_PENDING    /* 发送端处于 FC WAIT，需等待 */
} cantp_status_t;

/* ---------------- 低层帧构建（白板题核心） ---------------- */

/* 单帧：data 0~7 字节，DLC = len+1 */
cantp_status_t CANTP_BuildSF(const uint8_t *data, uint8_t len, can_msg_t *msg);

/* 首帧：total_len 为整条报文长度(8~4095)，前 6 字节填入 msg */
cantp_status_t CANTP_BuildFF(const uint8_t *data, uint16_t total_len, can_msg_t *msg);

/* 连续帧：sn 0~15 循环，data 为 7 字节载荷（末帧可不足） */
cantp_status_t CANTP_BuildCF(uint8_t sn, const uint8_t *data, uint8_t len, can_msg_t *msg);

/* 流控帧：fs 流控状态，bs 块大小，stmin 帧间隔(ms 或 0xF1~0xF9) */
cantp_status_t CANTP_BuildFC(cantp_fs_t fs, uint8_t bs, uint8_t stmin, can_msg_t *msg);

/* 解析 PCI 首字节：返回帧类型与 info
 *   SF -> info = 数据长度
 *   FF -> info = 整条报文总长度
 *   CF -> info = 序列号 SN
 *   FC -> info = 流控状态 FS
 */
cantp_status_t CANTP_ParsePCI(const can_msg_t *msg, cantp_pci_type_t *type, uint16_t *info);

/* ---------------- 接收端重组状态机 ---------------- */

typedef struct {
    uint8_t  buf[CANTP_RX_BUF_SIZE];
    uint16_t buf_len;     /* 已重组字节数 */
    uint16_t total_len;   /* 期望总字节数 */
    uint8_t  sn_next;     /* 下一个期望的 CF 序列号 */
    uint8_t  bs;          /* 对方请求的块大小 */
    uint8_t  cf_in_block; /* 当前块已收 CF 数 */
    uint8_t  active;      /* 是否正在接收多帧 */
} cantp_rx_t;

void CANTP_RxInit(cantp_rx_t *ctx);

/**
 * 处理一帧收到的 CAN 报文。
 * @param fc_tx      收到 FF / 块满时用于回发 FC 的回调（可为 NULL）
 * @param fc_bs      我们作为接收方下发的块大小（0=不限制）
 * @param fc_stmin   我们作为接收方下发的帧间隔(ms)
 * @param out_buf    重组完成后的完整报文（调用方提供 >= CANTP_RX_BUF_SIZE）
 * @param out_len    重组完成后的长度
 * @param complete   本帧处理后是否已完成一条完整报文
 */
cantp_status_t CANTP_RxOnFrame(cantp_rx_t *ctx, const can_msg_t *msg,
                               can_tx_callback_t fc_tx, uint8_t fc_bs, uint8_t fc_stmin,
                               uint8_t *out_buf, uint16_t *out_len,
                               uint16_t max_len, uint8_t *complete);

/* ---------------- 发送端分段状态机 ---------------- */

typedef enum {
    CANTP_TX_IDLE = 0,
    CANTP_TX_FF_SENT,
    CANTP_TX_CF_SENT,
    CANTP_TX_DONE
} cantp_tx_phase_t;

typedef struct {
    const uint8_t *data;
    uint16_t total_len;
    uint16_t offset;     /* 已发送字节数（不含 PCI） */
    uint8_t  sn;         /* 下一个 CF 序列号 */
    uint8_t  bs;         /* 对方允许的块大小 */
    uint8_t  stmin_ms;   /* 帧间隔(ms) */
    uint8_t  cf_in_block;
    uint8_t  wait_fc;    /* 收到 WAIT，需等待下个 FC */
    cantp_tx_phase_t phase;
    uint32_t last_tick;
} cantp_tx_t;

void CANTP_TxInit(cantp_tx_t *ctx, const uint8_t *data, uint16_t len);

/* 收到对方 FlowControl 时调用，更新 bs/stmin，清除 WAIT */
cantp_status_t CANTP_TxOnFlowControl(cantp_tx_t *ctx, uint8_t fs, uint8_t bs, uint8_t stmin);

/**
 * 由上层在每次 tick 调用驱动发送。
 * 若满足时序(STmin)且未处于 WAIT，则通过 tx 发出下一帧；done=1 时表示全部发完。
 */
cantp_status_t CANTP_TxStep(cantp_tx_t *ctx, can_tx_callback_t tx,
                            cantp_get_tick_t get_tick, uint8_t *done);

#endif /* CAN_TP_H */
