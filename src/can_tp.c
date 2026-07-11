/**
 * @file    can_tp.c
 * @brief   ISO 15765-2 传输层协议栈实现（见 can_tp.h）
 */
#include "can_tp.h"
#include <string.h>

/* ---------------- 低层帧构建 ---------------- */

cantp_status_t CANTP_BuildSF(const uint8_t *data, uint8_t len, can_msg_t *msg)
{
    if (len > 7) return CANTP_ERR_LENGTH;

    memset(msg, 0, sizeof(can_msg_t));
    msg->id  = CANTP_RX_ID;        /* 此处填对端 ID，实际由调用方覆盖 */
    msg->dlc = (uint8_t)(len + 1); /* 1 字节 PCI + N 字节数据 */
    msg->data[0] = (uint8_t)(len & 0x0F); /* SF: PCI_H = 0000 | length */
    if (len > 0 && data != NULL) {
        memcpy(&msg->data[1], data, len);
    }
    return CANTP_OK;
}

cantp_status_t CANTP_BuildFF(const uint8_t *data, uint16_t total_len, can_msg_t *msg)
{
    /* FF 用于 8~4095 字节的报文 */
    if (total_len < 8 || total_len > 4095) return CANTP_ERR_LENGTH;

    memset(msg, 0, sizeof(can_msg_t));
    msg->dlc = 8;
    /* PCI byte0: 0001 | length[11:8] */
    msg->data[0] = (uint8_t)(0x10 | ((total_len >> 8) & 0x0F));
    /* PCI byte1: length[7:0] */
    msg->data[1] = (uint8_t)(total_len & 0xFF);
    /* 前 6 字节数据 */
    memcpy(&msg->data[2], data, 6);
    return CANTP_OK;
}

cantp_status_t CANTP_BuildCF(uint8_t sn, const uint8_t *data, uint8_t len, can_msg_t *msg)
{
    if (len > 7) return CANTP_ERR_LENGTH;

    memset(msg, 0, sizeof(can_msg_t));
    msg->dlc = (uint8_t)(len + 1);
    /* PCI byte0: 0010 | sequence_number(0~15) */
    msg->data[0] = (uint8_t)(0x20 | (sn & 0x0F));
    if (len > 0 && data != NULL) {
        memcpy(&msg->data[1], data, len);
    }
    return CANTP_OK;
}

cantp_status_t CANTP_BuildFC(cantp_fs_t fs, uint8_t bs, uint8_t stmin, can_msg_t *msg)
{
    memset(msg, 0, sizeof(can_msg_t));
    msg->dlc = 3;
    /* PCI byte0: 0011 | FS */
    msg->data[0] = (uint8_t)(0x30 | (fs & 0x0F));
    msg->data[1] = bs;    /* Block Size */
    msg->data[2] = stmin; /* Separation Time */
    return CANTP_OK;
}

cantp_status_t CANTP_ParsePCI(const can_msg_t *msg, cantp_pci_type_t *type, uint16_t *info)
{
    if (msg == NULL || msg->dlc == 0) return CANTP_ERR_LENGTH;
    uint8_t pci = msg->data[0];
    uint8_t hi = (uint8_t)(pci >> 4);

    switch (hi) {
    case 0:
        *type = CANTP_PCI_SF;
        *info = (uint16_t)(pci & 0x0F);
        break;
    case 1:
        *type = CANTP_PCI_FF;
        *info = (uint16_t)(((pci & 0x0F) << 8) | msg->data[1]);
        break;
    case 2:
        *type = CANTP_PCI_CF;
        *info = (uint16_t)(pci & 0x0F);
        break;
    case 3:
        *type = CANTP_PCI_FC;
        *info = (uint16_t)(pci & 0x0F);
        break;
    default:
        return CANTP_ERR_UNEXPECTED;
    }
    return CANTP_OK;
}

/* ---------------- 接收端重组状态机 ---------------- */

void CANTP_RxInit(cantp_rx_t *ctx)
{
    memset(ctx, 0, sizeof(cantp_rx_t));
}

static void send_fc(cantp_rx_t *ctx, can_tx_callback_t fc_tx, uint8_t bs, uint8_t stmin)
{
    (void)ctx;
    if (fc_tx == NULL) return;
    can_msg_t fc;
    CANTP_BuildFC(CANTP_FS_CTS, bs, stmin, &fc);
    fc_tx(&fc);
}

cantp_status_t CANTP_RxOnFrame(cantp_rx_t *ctx, const can_msg_t *msg,
                               can_tx_callback_t fc_tx, uint8_t fc_bs, uint8_t fc_stmin,
                               uint8_t *out_buf, uint16_t *out_len,
                               uint16_t max_len, uint8_t *complete)
{
    cantp_pci_type_t type;
    uint16_t info;

    if (complete) *complete = 0;
    if (msg == NULL) return CANTP_ERR_LENGTH;

    cantp_status_t st = CANTP_ParsePCI(msg, &type, &info);
    if (st != CANTP_OK) return st;

    if (type == CANTP_PCI_SF) {
        if (ctx->active) return CANTP_ERR_BUSY; /* 正在收多帧，不能插单帧 */
        uint8_t len = (uint8_t)info;
        if (len == 0) return CANTP_ERR_LENGTH;
        if (len > max_len) return CANTP_ERR_OVERFLOW;
        memcpy(out_buf, &msg->data[1], len);
        *out_len = len;
        if (complete) *complete = 1;
        return CANTP_OK;
    }

    if (type == CANTP_PCI_FF) {
        uint16_t total = info;
        if (total < 8 || total > 4095) return CANTP_ERR_LENGTH;
        if (total > max_len) return CANTP_ERR_OVERFLOW;
        ctx->active    = 1;
        ctx->total_len = total;
        ctx->buf_len   = 6;
        memcpy(ctx->buf, &msg->data[2], 6);
        ctx->sn_next   = 1;
        ctx->bs        = fc_bs;
        ctx->cf_in_block = 0;
        send_fc(ctx, fc_tx, fc_bs, fc_stmin);
        return CANTP_OK;
    }

    if (type == CANTP_PCI_CF) {
        if (!ctx->active) return CANTP_ERR_UNEXPECTED;
        uint8_t sn = (uint8_t)info;
        if (sn != ctx->sn_next) {
            ctx->active = 0;
            return CANTP_ERR_SEQUENCE;
        }
        uint8_t payload = (uint8_t)(msg->dlc - 1);
        /* 末帧可能不足 7 字节，但不得超出 total_len */
        uint16_t remaining = (uint16_t)(ctx->total_len - ctx->buf_len);
        if (payload > remaining) payload = (uint8_t)remaining;

        if (ctx->buf_len + payload > max_len) {
            ctx->active = 0;
            return CANTP_ERR_OVERFLOW;
        }
        memcpy(&ctx->buf[ctx->buf_len], &msg->data[1], payload);
        ctx->buf_len += payload;
        ctx->sn_next  = (uint8_t)((sn + 1) & 0x0F);
        ctx->cf_in_block++;

        if (ctx->buf_len >= ctx->total_len) {
            memcpy(out_buf, ctx->buf, ctx->total_len);
            *out_len = ctx->total_len;
            if (complete) *complete = 1;
            ctx->active = 0;
            return CANTP_OK;
        }
        /* 块满（BS!=0 且已收 BS 个 CF）-> 回发 FC 请求下一块 */
        if (ctx->bs != 0 && ctx->cf_in_block >= ctx->bs) {
            ctx->cf_in_block = 0;
            send_fc(ctx, fc_tx, fc_bs, fc_stmin);
        }
        return CANTP_OK;
    }

    /* 不期望在接收路径收到 FC */
    return CANTP_ERR_UNEXPECTED;
}

/* ---------------- 发送端分段状态机 ---------------- */

void CANTP_TxInit(cantp_tx_t *ctx, const uint8_t *data, uint16_t len)
{
    memset(ctx, 0, sizeof(cantp_tx_t));
    ctx->data      = data;
    ctx->total_len = len;
    ctx->phase     = CANTP_TX_IDLE;
}

cantp_status_t CANTP_TxOnFlowControl(cantp_tx_t *ctx, uint8_t fs, uint8_t bs, uint8_t stmin)
{
    if (fs == CANTP_FS_WAIT) {
        ctx->wait_fc = 1;
        return CANTP_OK;
    }
    if (fs == CANTP_FS_OVFLW) {
        ctx->phase = CANTP_TX_IDLE;
        return CANTP_ERR_OVERFLOW;
    }
    /* CTS: 更新流控参数，清除等待 */
    ctx->bs        = bs;
    ctx->stmin_ms  = (stmin < 0x80) ? stmin : 1; /* 0xF1~0xF9 近似为 1ms */
    ctx->wait_fc   = 0;
    ctx->cf_in_block = 0;
    return CANTP_OK;
}

cantp_status_t CANTP_TxStep(cantp_tx_t *ctx, can_tx_callback_t tx,
                            cantp_get_tick_t get_tick, uint8_t *done)
{
    if (done) *done = 0;
    if (ctx->data == NULL) return CANTP_ERR_LENGTH;

    /* 等待 FC（WAIT）状态时不可发送 */
    if (ctx->wait_fc) return CANTP_ERR_PENDING;

    /* STmin 时序检查 */
    if (get_tick != NULL) {
        uint32_t now = get_tick();
        if (ctx->phase != CANTP_TX_IDLE) {
            uint32_t elapsed = (uint32_t)(now - ctx->last_tick);
            if (elapsed < ctx->stmin_ms) return CANTP_OK; /* 还没到时间 */
        }
    }

    if (ctx->phase == CANTP_TX_IDLE) {
        /* 首帧或单帧 */
        if (ctx->total_len <= 7) {
            can_msg_t m;
            CANTP_BuildSF(ctx->data, (uint8_t)ctx->total_len, &m);
            tx(&m);
            ctx->phase = CANTP_TX_DONE;
            ctx->last_tick = get_tick ? get_tick() : 0;
            if (done) *done = 1;
            return CANTP_OK;
        } else {
            can_msg_t m;
            CANTP_BuildFF(ctx->data, ctx->total_len, &m);
            tx(&m);
            ctx->offset    = 6;
            ctx->sn        = 1;
            ctx->cf_in_block = 0;
            ctx->phase     = CANTP_TX_FF_SENT;
            ctx->wait_fc   = 1; /* ISO-TP：FF 后必须等待 FC 才能发 CF */
            ctx->last_tick = get_tick ? get_tick() : 0;
            return CANTP_OK;
        }
    }

    if (ctx->phase == CANTP_TX_FF_SENT || ctx->phase == CANTP_TX_CF_SENT) {
        uint16_t remaining = (uint16_t)(ctx->total_len - ctx->offset);
        uint8_t  chunk = (remaining > 7) ? 7 : (uint8_t)remaining;
        can_msg_t m;
        CANTP_BuildCF(ctx->sn, &ctx->data[ctx->offset], chunk, &m);
        tx(&m);
        ctx->offset += chunk;
        ctx->sn = (uint8_t)((ctx->sn + 1) & 0x0F);
        ctx->cf_in_block++;
        ctx->last_tick = get_tick ? get_tick() : 0;
        ctx->phase = CANTP_TX_CF_SENT;

        if (ctx->offset >= ctx->total_len) {
            ctx->phase = CANTP_TX_DONE;
            if (done) *done = 1;
            return CANTP_OK;
        }
        /* 块满（BS!=0）-> 等待下个 FC */
        if (ctx->bs != 0 && ctx->cf_in_block >= ctx->bs) {
            ctx->wait_fc = 1;
        }
        return CANTP_OK;
    }

    if (ctx->phase == CANTP_TX_DONE) {
        if (done) *done = 1;
        return CANTP_OK;
    }
    return CANTP_OK;
}
