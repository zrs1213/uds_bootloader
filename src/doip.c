/**
 * @file    doip.c
 * @brief   DoIP 传输层实现（ISO 13400-2 子集，见 doip.h 头注释）。
 *          纯逻辑零 I/O：socket 适配在 examples/doip_ecu_tcp.c。
 */
#include "doip.h"
#include <string.h>

/* ================ 头部编解码 ================ */

uint32_t DOIP_WriteHeader(uint8_t *out, uint16_t type, uint32_t payload_len)
{
    out[0] = (uint8_t)DOIP_VERSION;
    out[1] = (uint8_t)DOIP_INV_VERSION;
    out[2] = (uint8_t)(type >> 8);
    out[3] = (uint8_t)(type & 0xFFu);
    out[4] = (uint8_t)(payload_len >> 24);
    out[5] = (uint8_t)(payload_len >> 16);
    out[6] = (uint8_t)(payload_len >> 8);
    out[7] = (uint8_t)(payload_len & 0xFFu);
    return DOIP_HDR_LEN;
}

int DOIP_ReadHeader(const uint8_t *buf, uint32_t avail,
                    uint16_t *type, uint32_t *payload_len)
{
    if (avail < DOIP_HDR_LEN) {
        return 1; /* 半包，继续收 */
    }
    if (buf[0] != (uint8_t)DOIP_VERSION || buf[1] != (uint8_t)DOIP_INV_VERSION) {
        return -1; /* 坏头，丢 1 字节重同步 */
    }
    *type = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);
    *payload_len = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                   ((uint32_t)buf[6] << 8)  | (uint32_t)buf[7];
    return 0;
}

/* ================ 流式解帧 ================ */

void DOIP_StreamInit(doip_stream_t *s)
{
    s->len = 0;
}

/* 从缓存头部取出一条完整消息：返回 1=产出一条 0=数据不足 -1=坏头(已丢1字节) -2=超长(已清空) */
static int stream_take(doip_stream_t *s, uint16_t *type,
                       const uint8_t **payload, uint32_t *plen)
{
    uint16_t t;
    uint32_t l, need;
    int rc = DOIP_ReadHeader(s->buf, s->len, &t, &l);
    if (rc == 1) return 0;
    if (rc == -1) { /* 坏头：丢首字节 */
        memmove(s->buf, s->buf + 1, s->len - 1);
        s->len -= 1;
        return -1;
    }
    if (l > DOIP_MAX_PAYLOAD) { /* 声明超长：整缓冲丢弃重同步 */
        s->len = 0;
        return -2;
    }
    need = DOIP_HDR_LEN + l;
    if (s->len < need) return 0; /* 半包 */
    *type = t;
    *payload = s->buf + DOIP_HDR_LEN;
    *plen = l;
    /* 消费后前移（回调里可能再喂数据，先拷贝由调用方负责） */
    return 1;
}

void DOIP_StreamFeed(doip_stream_t *s, const uint8_t *data, uint32_t n,
                     doip_msg_cb cb, void *user)
{
    /* 追加进缓存 */
    if (n > 0) {
        if (s->len + n > DOIP_STREAM_BUF) {
            /* 缓冲被撑爆属异常对端：清空重同步，再收新数据 */
            s->len = 0;
            if (n > DOIP_STREAM_BUF) {
                data += (n - DOIP_STREAM_BUF);
                n = DOIP_STREAM_BUF;
            }
        }
        memcpy(s->buf + s->len, data, n);
        s->len += n;
    }

    /* 循环切分：每条消息先拷到局部缓冲再消费，避免回调重入踩内存 */
    for (;;) {
        uint16_t type;
        const uint8_t *payload;
        uint32_t plen, need;
        int rc = stream_take(s, &type, &payload, &plen);
        if (rc == 0) break;
        if (rc < 0) continue; /* 已丢字节/清空，继续尝试 */
        need = DOIP_HDR_LEN + plen;
        {
            uint8_t local[DOIP_STREAM_BUF];
            memcpy(local, payload, plen);
            /* 先消费缓存 */
            memmove(s->buf, s->buf + need, s->len - need);
            s->len -= need;
            cb(type, local, plen, user);
        }
    }
}

/* ================ 便捷构建 ================ */

static uint32_t put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; return 2; }

/* 交出一帧载荷（头由适配层拼装），本层零缓冲、零拷贝 */
static void tx_frame(doip_tx_fn tx, void *user, uint16_t type,
                     const uint8_t *payload, uint32_t plen)
{
    tx(type, payload, plen, user);
}

uint32_t DOIP_BuildDiagMessage(uint8_t *out, uint16_t sa, uint16_t ta,
                               const uint8_t *uds, uint32_t uds_len)
{
    uint32_t n = 0;
    n += DOIP_WriteHeader(out + n, DOIP_PT_DIAG_MESSAGE, 4u + uds_len);
    out[n++] = (uint8_t)(sa >> 8); out[n++] = (uint8_t)(sa & 0xFFu);
    out[n++] = (uint8_t)(ta >> 8); out[n++] = (uint8_t)(ta & 0xFFu);
    if (uds_len) memcpy(out + n, uds, uds_len);
    return n + uds_len;
}

/* ================ ECU 侧消息分发 ================ */

void DOIP_EcuInit(doip_ecu_t *d, uint16_t ecu_addr, const char *vin17)
{
    memset(d, 0, sizeof(*d));
    d->ecu_addr = ecu_addr;
    d->tester_addr = 0;
    d->routing_active = 0;
    if (vin17) memcpy(d->vin, vin17, 17);
}

/* 0x0002 车辆宣告应答：VIN17+LA2+EID6+FurtherID6+valid1+GID8+FID8+IPv4_4+IPv6_16 */
static void send_veh_id_resp(doip_ecu_t *d, doip_tx_fn tx, void *user)
{
    uint8_t p[68];
    uint32_t i = 0;
    memset(p, 0, sizeof(p));
    memcpy(p + i, d->vin, 17); i += 17;              /* VIN */
    i += put16(p + i, d->ecu_addr);                  /* 逻辑地址 */
    /* EID(6)/FurtherID(6)：仿真置 0 */
    i += 12;
    p[i++] = 0x01;                                   /* GID/FID 有效 */
    memset(p + i, 0x00, 8); i += 8;                  /* GID */
    memset(p + i, 0x00, 8); i += 8;                  /* FID */
    p[i++] = 192; p[i++] = 168; p[i++] = 1; p[i++] = 100; /* IPv4 演示地址 */
    memset(p + i, 0x00, 16); i += 16;                /* IPv6 */
    tx_frame(tx, user, DOIP_PT_VEH_ID_RESP, p, i);
}

static void send_ack(doip_tx_fn tx, void *user, uint16_t sa, uint16_t ta,
                     uint8_t code, uint8_t nack)
{
    uint8_t p[5];
    uint32_t n = 0;
    n += put16(p + n, sa);
    n += put16(p + n, ta);
    p[n++] = code;
    tx_frame(tx, user, nack ? DOIP_PT_DIAG_MESSAGE_NACK : DOIP_PT_DIAG_MESSAGE_ACK, p, n);
}

void DOIP_EcuOnMessage(doip_ecu_t *d, uds_ctx_t *uds,
                       uint16_t type, const uint8_t *p, uint32_t len,
                       doip_tx_fn tx, void *user)
{
    switch (type) {

    case DOIP_PT_ANNOUNCEMENT_REQ:
        send_veh_id_resp(d, tx, user);
        break;

    case DOIP_PT_VEH_ID_REQ_VIN:
        /* VIN 不匹配则不应答（ISO 13400-2 规定） */
        if (len >= 17 && memcmp(p, d->vin, 17) == 0) {
            send_veh_id_resp(d, tx, user);
        }
        break;

    case DOIP_PT_ROUTING_ACT_REQ: {
        uint8_t r[7];
        uint16_t sa;
        uint8_t code;
        if (len < 6) {
            r[0] = 0; r[1] = 0; r[2] = (uint8_t)(d->ecu_addr >> 8); r[3] = (uint8_t)d->ecu_addr;
            r[4] = DOIP_RAR_REJECTED_FORMAT;
            tx_frame(tx, user, DOIP_PT_ROUTING_ACT_RESP, r, 5);
            break;
        }
        sa = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
        if (sa == 0x0000u) {
            code = DOIP_RAR_REJECTED_UNKNOWN_SA;
        } else if (d->routing_active && d->tester_addr == sa) {
            code = DOIP_RAR_ALREADY_ACTIVE;
        } else {
            d->tester_addr = sa;
            d->routing_active = 1;
            code = DOIP_RAR_SUCCESS;
        }
        r[0] = (uint8_t)(sa >> 8); r[1] = (uint8_t)(sa & 0xFFu);
        r[2] = (uint8_t)(d->ecu_addr >> 8); r[3] = (uint8_t)(d->ecu_addr & 0xFFu);
        r[4] = code;
        r[5] = 0x00; r[6] = 0x00; /* reserved(2B 演示，规范为可选 OEM 字段) */
        tx_frame(tx, user, DOIP_PT_ROUTING_ACT_RESP, r, 7);
        break;
    }

    case DOIP_PT_ALIVE_CHECK_REQ: {
        uint8_t r[2];
        put16(r, d->ecu_addr);
        tx_frame(tx, user, DOIP_PT_ALIVE_CHECK_RESP, r, 2);
        break;
    }

    case DOIP_PT_DIAG_MESSAGE: {
        uint16_t sa, ta;
        uint8_t resp[512];
        uint16_t rlen = 0;
        if (len < 5) break; /* 至少 SA2+TA2+SID1，畸形直接丢弃 */
        sa = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
        ta = (uint16_t)(((uint16_t)p[2] << 8) | p[3]);
        if (!d->routing_active) {
            send_ack(tx, user, sa, ta, DOIP_NACK_NOT_ACTIVATED, 1);
            break;
        }
        if (sa != d->tester_addr) {
            send_ack(tx, user, sa, ta, DOIP_NACK_INVALID_SA, 1);
            break;
        }
        if (ta != d->ecu_addr) {
            send_ack(tx, user, sa, ta, DOIP_NACK_UNKNOWN_TA, 1);
            break;
        }
        /* 先回正 ACK，再回 UDS 响应（两条独立 DoIP 报文） */
        send_ack(tx, user, sa, ta, 0x00, 0);
        if (UDS_ProcessRequest(uds, p + 4, (uint16_t)(len - 4),
                               resp, &rlen) == 0 && rlen > 0) {
            uint8_t out[4 + sizeof(resp)];
            put16(out, d->ecu_addr);
            put16(out + 2, d->tester_addr);
            memcpy(out + 4, resp, rlen);
            tx_frame(tx, user, DOIP_PT_DIAG_MESSAGE, out, 4u + rlen);
        }
        break;
    }

    default:
        /* 未知载荷类型：静默丢弃（TCP 不断链，符合保守实现） */
        break;
    }
}
