/**
 * @file    uds_service.c
 * @brief   UDS 应用层服务分发实现（ISO 14229-1）。
 */
#include "uds_service.h"
#include "nrc.h"
#include <string.h>

/* ---------- CRC32 (IEEE 802.3, 用于刷写完整性校验) ---------- */
static uint32_t crc32_table[256];
static uint8_t  crc32_init_done = 0;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_init_done = 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len) {
    if (!crc32_init_done) crc32_init();
    crc = crc ^ 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)((crc ^ data[i]) & 0xFF);
        crc = crc32_table[idx] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---------- 负响应构造 ---------- */
static void uds_build_neg(uint8_t *resp, uint16_t *resp_len, uint8_t sid, uint8_t nrc) {
    resp[0] = 0x7F;
    resp[1] = sid;
    resp[2] = nrc;
    *resp_len = 3;
}

/* ---------- 会话权限表 ---------- */
/* 返回 1 表示该 SID 在给定会话下允许；否则 0 */
static uint8_t uds_session_allowed(uint8_t sid, uds_session_id_t s) {
    switch (sid) {
        case 0x10: case 0x11: case 0x22: case 0x3E:
            return 1; /* 所有会话可用 */
        case 0x27: case 0x2E: case 0x31: case 0x28: case 0x85:
            return (s == UDS_SESSION_EXTENDED || s == UDS_SESSION_PROGRAMMING);
        case 0x34: case 0x36: case 0x37:
            return (s == UDS_SESSION_PROGRAMMING);
        default:
            return 0;
    }
}

void UDS_Init(uds_ctx_t *c) {
    UDS_FsmInit(&c->fsm, UDS_S3_SERVER_MS);
    UDS_FlashInit(&c->flash);
    c->sec_attempts = 0;
    c->seed_valid   = 0;
    c->dtc_enabled  = 1;
    c->comm_disabled = 0;
    memset(c->last_seed, 0, SEC_SEED_LEN);
    memset(&c->dl, 0, sizeof(c->dl));
}

/* ============================================================
 *  服务处理子函数（返回 0 表示已产生响应；1 表示抑制/无响应）
 * ============================================================ */

/* $10 诊断会话控制 */
static uint8_t svc_session(uds_ctx_t *c, const uint8_t *req, uint16_t len,
                           uint8_t *resp, uint16_t *resp_len) {
    if (len < 2) { uds_build_neg(resp, resp_len, 0x10, NRC_INCORRECT_MESSAGE_LENGTH); return 0; }
    uint8_t sub = req[1];
    uint8_t suppress = sub & 0x80;
    sub &= 0x7F;
    uds_session_id_t target = (uds_session_id_t)sub;
    if (target != UDS_SESSION_DEFAULT && target != UDS_SESSION_PROGRAMMING &&
        target != UDS_SESSION_EXTENDED) {
        uds_build_neg(resp, resp_len, 0x10, NRC_SUB_FUNCTION_NOT_SUPPORTED);
        return 0;
    }
    UDS_FsmSetSession(&c->fsm, target, 0);
    if (suppress) return 1;
    resp[0] = 0x50;
    resp[1] = sub;
    /* P2server / P2*server（单位 ms），固定值仅作演示 */
    resp[2] = 0x00; resp[3] = 0x32;   /* P2 = 50ms */
    resp[4] = 0x01; resp[5] = 0xF4;   /* P2* = 500ms */
    *resp_len = 6;
    return 0;
}

/* $11 ECU 复位 */
static uint8_t svc_ecu_reset(uds_ctx_t *c, const uint8_t *req, uint16_t len,
                             uint8_t *resp, uint16_t *resp_len) {
    if (len < 2) { uds_build_neg(resp, resp_len, 0x11, NRC_INCORRECT_MESSAGE_LENGTH); return 0; }
    uint8_t sub = req[1];
    uint8_t suppress = sub & 0x80;
    sub &= 0x7F;
    if (sub < 0x01 || sub > 0x03) {
        uds_build_neg(resp, resp_len, 0x11, NRC_SUB_FUNCTION_NOT_SUPPORTED);
        return 0;
    }
    if (suppress) return 1;
    resp[0] = 0x51; resp[1] = sub;
    *resp_len = 2;
    /* 仿真环境：复位即回落默认会话、清安全等级（真实 ECU 此处触发 WD/复位） */
    UDS_FsmSetSession(&c->fsm, UDS_SESSION_DEFAULT, 0);
    return 0;
}

/* $85 控制 DTC 设置（刷写前关、刷写后开） */
static uint8_t svc_control_dtc(uds_ctx_t *c, const uint8_t *req, uint16_t len,
                               uint8_t *resp, uint16_t *resp_len) {
    if (len < 2) { uds_build_neg(resp, resp_len, 0x85, NRC_INCORRECT_MESSAGE_LENGTH); return 0; }
    uint8_t sub = req[1] & 0x7F;
    if (sub == 0x01) {
        c->dtc_enabled = 1;            /* 开启 DTC 设置 */
        resp[0] = 0xC5; resp[1] = 0x01; *resp_len = 2; return 0;
    } else if (sub == 0x02) {
        c->dtc_enabled = 0;            /* 关闭 DTC 设置（抑制故障码记录） */
        resp[0] = 0xC5; resp[1] = 0x02; *resp_len = 2; return 0;
    }
    uds_build_neg(resp, resp_len, 0x85, NRC_SUB_FUNCTION_NOT_SUPPORTED);
    return 0;
}

/* $28 通信控制（刷写前关闭非诊断通信，刷写后恢复） */
static uint8_t svc_comm_control(uds_ctx_t *c, const uint8_t *req, uint16_t len,
                                uint8_t *resp, uint16_t *resp_len) {
    if (len < 2) { uds_build_neg(resp, resp_len, 0x28, NRC_INCORRECT_MESSAGE_LENGTH); return 0; }
    uint8_t sub = req[1] & 0x7F;
    /* 0x00=使能Rx&Tx 0x01=禁Rx 0x02=禁Tx 0x03=禁Rx&Tx */
    if (sub > 0x03) {
        uds_build_neg(resp, resp_len, 0x28, NRC_SUB_FUNCTION_NOT_SUPPORTED);
        return 0;
    }
    c->comm_disabled = sub;
    resp[0] = 0x68; resp[1] = sub; *resp_len = 2;
    return 0;
}

/* $3E TesterPresent */
static uint8_t svc_tester_present(uds_ctx_t *c, const uint8_t *req, uint16_t len,
                                  uint8_t *resp, uint16_t *resp_len) {
    (void)c;
    if (len < 2) { uds_build_neg(resp, resp_len, 0x3E, NRC_INCORRECT_MESSAGE_LENGTH); return 0; }
    uint8_t sub = req[1];
    uint8_t suppress = sub & 0x80;
    if ((sub & 0x7F) != 0x00) {
        uds_build_neg(resp, resp_len, 0x3E, NRC_SUB_FUNCTION_NOT_SUPPORTED);
        return 0;
    }
    if (suppress) return 1;
    resp[0] = 0x7E; resp[1] = 0x00;
    *resp_len = 2;
    return 0;
}

/* $22 按 ID 读数据（支持单条/多条 DID） */
static uint8_t svc_read_did(uds_ctx_t *c, const uint8_t *req, uint16_t len,
                            uint8_t *resp, uint16_t *resp_len) {
    (void)c;
    if (len < 3 || ((len - 1) % 2) != 0) {
        uds_build_neg(resp, resp_len, 0x22, NRC_INCORRECT_MESSAGE_LENGTH);
        return 0;
    }
    uint16_t out = 1;
    resp[0] = 0x62;
    uint16_t idx = 1;
    while (idx + 1 < len) {
        uint16_t did = ((uint16_t)req[idx] << 8) | req[idx + 1];
        uint8_t  data[16];
        uint16_t dlen = 0;
        if (UDS_ReadDID(did, data, &dlen) != 0) {
            uds_build_neg(resp, resp_len, 0x22, NRC_REQUEST_OUT_OF_RANGE);
            return 0;
        }
        if (out + 2 + dlen > 512) { /* 防缓冲溢出 */
            uds_build_neg(resp, resp_len, 0x22, NRC_REQUEST_OUT_OF_RANGE);
            return 0;
        }
        resp[out++] = (uint8_t)(did >> 8);
        resp[out++] = (uint8_t)(did & 0xFF);
        memcpy(resp + out, data, dlen);
        out += dlen;
        idx += 2;
    }
    *resp_len = out;
    return 0;
}

/* $2E 按 ID 写数据 */
static uint8_t svc_write_did(uds_ctx_t *c, const uint8_t *req, uint16_t len,
                             uint8_t *resp, uint16_t *resp_len) {
    (void)c;
    if (len < 4) { uds_build_neg(resp, resp_len, 0x2E, NRC_INCORRECT_MESSAGE_LENGTH); return 0; }
    uint16_t did = ((uint16_t)req[1] << 8) | req[2];
    uint16_t dlen = len - 3;
    uint8_t rc = UDS_WriteDID(did, req + 3, dlen);
    if (rc == 1)      { uds_build_neg(resp, resp_len, 0x2E, NRC_REQUEST_OUT_OF_RANGE); return 0; }
    if (rc == 2)      { uds_build_neg(resp, resp_len, 0x2E, NRC_CONDITIONS_NOT_CORRECT); return 0; }
    if (rc == 3)      { uds_build_neg(resp, resp_len, 0x2E, NRC_INCORRECT_MESSAGE_LENGTH); return 0; }
    resp[0] = 0x6E; resp[1] = (uint8_t)(did >> 8); resp[2] = (uint8_t)(did & 0xFF);
    *resp_len = 3;
    return 0;
}

/* $27 安全访问 (Seed&Key) */
static uint8_t svc_security(uds_ctx_t *c, const uint8_t *req, uint16_t len,
                            uint8_t *resp, uint16_t *resp_len) {
    if (len < 2) { uds_build_neg(resp, resp_len, 0x27, NRC_INCORRECT_MESSAGE_LENGTH); return 0; }
    uint8_t sub = req[1];
    uint8_t suppress = sub & 0x80;
    sub &= 0x7F;

    if (sub == 0x01) {
        /* 请求 seed */
        if (c->sec_attempts >= SEC_MAX_ATTEMPTS) {
            uds_build_neg(resp, resp_len, 0x27, NRC_EXCEED_NUMBER_OF_ATTEMPTS);
            return 0;
        }
        SEC_GenSeed(c->last_seed);
        c->seed_valid = 1;
        if (suppress) return 1;
        resp[0] = 0x67; resp[1] = 0x01;
        memcpy(resp + 2, c->last_seed, SEC_SEED_LEN);
        *resp_len = 2 + SEC_SEED_LEN;
        return 0;
    } else if (sub == 0x02) {
        /* 提交 key */
        if (!c->seed_valid) {
            uds_build_neg(resp, resp_len, 0x27, NRC_REQUEST_SEQUENCE_ERROR);
            return 0;
        }
        if (len < 2 + SEC_KEY_LEN) {
            uds_build_neg(resp, resp_len, 0x27, NRC_INCORRECT_MESSAGE_LENGTH);
            return 0;
        }
        uint8_t ok = SEC_CheckKey(c->last_seed, req + 2);
        c->seed_valid = 0;
        if (ok) {
            c->sec_attempts = 0;
            UDS_FsmSetSecurity(&c->fsm, 1);
            resp[0] = 0x67; resp[1] = 0x02;
            *resp_len = 2;
            return 0;
        } else {
            c->sec_attempts++;
            uds_build_neg(resp, resp_len, 0x27, NRC_INVALID_KEY);
            return 0;
        }
    }
    uds_build_neg(resp, resp_len, 0x27, NRC_SUB_FUNCTION_NOT_SUPPORTED);
    return 0;
}

/* $31 例程控制（实现 0xFF00=计算Flash CRC，0x0201=擦除APP区） */
static uint8_t svc_routine(uds_ctx_t *c, const uint8_t *req, uint16_t len,
                           uint8_t *resp, uint16_t *resp_len) {
    if (len < 4) { uds_build_neg(resp, resp_len, 0x31, NRC_INCORRECT_MESSAGE_LENGTH); return 0; }
    uint8_t  rtype = req[1];
    uint16_t rid   = ((uint16_t)req[2] << 8) | req[3];
    if (rtype != 0x01 && rtype != 0x02 && rtype != 0x03) {
        uds_build_neg(resp, resp_len, 0x31, NRC_SUB_FUNCTION_NOT_SUPPORTED);
        return 0;
    }
    if (rid == 0xFF00) {
        /* 计算整片 Flash 的 CRC32 作为例程结果 */
        uint32_t crc = crc32_update(0, c->flash.buf, c->flash.size);
        resp[0] = 0x71; resp[1] = rtype;
        resp[2] = (uint8_t)(rid >> 8); resp[3] = (uint8_t)(rid & 0xFF);
        resp[4] = 0x00; /* routineStatus: 完成 */
        resp[5] = (uint8_t)(crc >> 24); resp[6] = (uint8_t)(crc >> 16);
        resp[7] = (uint8_t)(crc >> 8);  resp[8] = (uint8_t)(crc & 0xFF);
        *resp_len = 9;
        return 0;
    } else if (rid == 0xFF01) {
        /* 检查预编程条件（电压/故障状态/安全等级等），仿真环境返回条件满足 */
        resp[0] = 0x71; resp[1] = rtype;
        resp[2] = (uint8_t)(rid >> 8); resp[3] = (uint8_t)(rid & 0xFF);
        resp[4] = 0x00;  /* routineStatus: 所有预编程条件满足 */
        *resp_len = 5;
        return 0;
    } else if (rid == 0x0201) {
        /* 擦除 APP 区（编程前准备） */
        UDS_FlashErase(&c->flash, c->flash.base, c->flash.size);
        resp[0] = 0x71; resp[1] = rtype;
        resp[2] = (uint8_t)(rid >> 8); resp[3] = (uint8_t)(rid & 0xFF);
        resp[4] = 0x00;
        *resp_len = 5;
        return 0;
    }
    uds_build_neg(resp, resp_len, 0x31, NRC_REQUEST_OUT_OF_RANGE);
    return 0;
}

/* 解析 lengthFormatIdentifier 中的地址/长度字段（大端） */
static uint32_t parse_be(const uint8_t *p, uint8_t n) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

/* $34 请求下载 */
static uint8_t svc_request_download(uds_ctx_t *c, const uint8_t *req, uint16_t len,
                                    uint8_t *resp, uint16_t *resp_len) {
    if (len < 4) { uds_build_neg(resp, resp_len, 0x34, NRC_INCORRECT_MESSAGE_LENGTH); return 0; }
    uint8_t dfi = req[1];              /* dataFormatIdentifier（0x00=无压缩/加密） */
    uint8_t lfi = req[2];              /* lengthFormatIdentifier */
    uint8_t addr_len = (lfi >> 4) & 0x0F;
    uint8_t size_len = lfi & 0x0F;
    if (addr_len == 0 || addr_len > 4 || size_len == 0 || size_len > 4) {
        uds_build_neg(resp, resp_len, 0x34, NRC_INCORRECT_MESSAGE_LENGTH);
        return 0;
    }
    uint16_t need = 3 + addr_len + size_len;
    if (len < need) { uds_build_neg(resp, resp_len, 0x34, NRC_INCORRECT_MESSAGE_LENGTH); return 0; }
    uint32_t addr = parse_be(req + 3, addr_len);
    uint32_t size = parse_be(req + 3 + addr_len, size_len);
    (void)dfi;

    /* 地址必须落在 Flash 区间 */
    if (addr < c->flash.base || (addr + size) > (c->flash.base + c->flash.size)) {
        uds_build_neg(resp, resp_len, 0x34, NRC_REQUEST_OUT_OF_RANGE);
        return 0;
    }
    /* 擦除目标区（先擦后写） */
    if (UDS_FlashErase(&c->flash, addr, size) != 0) {
        uds_build_neg(resp, resp_len, 0x34, NRC_REQUEST_OUT_OF_RANGE);
        return 0;
    }
    c->dl.active    = 1;
    c->dl.addr      = addr;
    c->dl.remaining = size;
    c->dl.bsc       = 1;     /* 首个 $36 期望 BSC = 0x01 */
    c->dl.crc       = 0;

    resp[0] = 0x74;
    resp[1] = 0x20;          /* 长度格式：后续 2 字节 maxNumberOfBlockLength */
    resp[2] = (uint8_t)(DL_BLOCK_MAX >> 8);
    resp[3] = (uint8_t)(DL_BLOCK_MAX & 0xFF);
    *resp_len = 4;
    return 0;
}

/* $36 数据传输 */
static uint8_t svc_transfer_data(uds_ctx_t *c, const uint8_t *req, uint16_t len,
                                 uint8_t *resp, uint16_t *resp_len) {
    if (len < 2) { uds_build_neg(resp, resp_len, 0x36, NRC_INCORRECT_MESSAGE_LENGTH); return 0; }
    if (!c->dl.active) {
        uds_build_neg(resp, resp_len, 0x36, NRC_REQUEST_SEQUENCE_ERROR);
        return 0;
    }
    uint8_t bsc = req[1];
    if (bsc != c->dl.bsc) {
        uds_build_neg(resp, resp_len, 0x36, NRC_REQUEST_SEQUENCE_ERROR);
        return 0;
    }
    uint16_t datalen = len - 2;
    if (datalen > c->dl.remaining) datalen = (uint16_t)c->dl.remaining;
    if (UDS_FlashWrite(&c->flash, c->dl.addr, req + 2, datalen) != 0) {
        uds_build_neg(resp, resp_len, 0x36, NRC_GENERAL_REJECT);
        return 0;
    }
    c->dl.crc = crc32_update(c->dl.crc, req + 2, datalen);
    c->dl.addr += datalen;
    c->dl.remaining -= datalen;
    c->dl.bsc = (c->dl.bsc == 0xFF) ? 0x00 : (uint8_t)(c->dl.bsc + 1);

    resp[0] = 0x76; resp[1] = bsc;
    *resp_len = 2;
    return 0;
}

/* $37 请求退出传输 */
static uint8_t svc_request_exit(uds_ctx_t *c, const uint8_t *req, uint16_t len,
                                uint8_t *resp, uint16_t *resp_len) {
    (void)req; (void)len;
    if (!c->dl.active) {
        uds_build_neg(resp, resp_len, 0x37, NRC_REQUEST_SEQUENCE_ERROR);
        return 0;
    }
    if (c->dl.remaining != 0) {
        /* 数据未传完 */
        uds_build_neg(resp, resp_len, 0x37, NRC_REQUEST_SEQUENCE_ERROR);
        return 0;
    }
    resp[0] = 0x77;
    *resp_len = 1;
    c->dl.active = 0;
    /* 仿真环境：此处可跳转 APP（设置跳转标志），省略 */
    return 0;
}

/* ============================================================
 *  顶层分发
 * ============================================================ */
uint8_t UDS_ProcessRequest(uds_ctx_t *c,
                           const uint8_t *req, uint16_t req_len,
                           uint8_t *resp, uint16_t *resp_len) {
    if (req_len < 1) { uds_build_neg(resp, resp_len, 0x00, NRC_INCORRECT_MESSAGE_LENGTH); return 0; }

    uint8_t sid = req[0];
    uds_session_id_t cur = UDS_FsmSession(&c->fsm);

    /* 刷新 S3（任何合法请求都重置服务器计时） */
    UDS_FsmNoteActivity(&c->fsm, 0);

    /* 会话权限校验 */
    if (!uds_session_allowed(sid, cur)) {
        uds_build_neg(resp, resp_len, sid, NRC_SERVICE_NOT_SUPPORTED_IN_SESSION);
        return 0;
    }

    switch (sid) {
        case 0x10: return svc_session(c, req, req_len, resp, resp_len);
        case 0x11: return svc_ecu_reset(c, req, req_len, resp, resp_len);
        case 0x3E: return svc_tester_present(c, req, req_len, resp, resp_len);
        case 0x22: return svc_read_did(c, req, req_len, resp, resp_len);
        case 0x2E: return svc_write_did(c, req, req_len, resp, resp_len);
        case 0x27: return svc_security(c, req, req_len, resp, resp_len);
        case 0x28: return svc_comm_control(c, req, req_len, resp, resp_len);
        case 0x31: return svc_routine(c, req, req_len, resp, resp_len);
        case 0x34:
            if (UDS_FsmSecurity(&c->fsm) == 0) {
                uds_build_neg(resp, resp_len, sid, NRC_SECURITY_ACCESS_DENIED);
                return 0;
            }
            return svc_request_download(c, req, req_len, resp, resp_len);
        case 0x36:
            if (UDS_FsmSecurity(&c->fsm) == 0) {
                uds_build_neg(resp, resp_len, sid, NRC_SECURITY_ACCESS_DENIED);
                return 0;
            }
            return svc_transfer_data(c, req, req_len, resp, resp_len);
        case 0x37:
            if (UDS_FsmSecurity(&c->fsm) == 0) {
                uds_build_neg(resp, resp_len, sid, NRC_SECURITY_ACCESS_DENIED);
                return 0;
            }
            return svc_request_exit(c, req, req_len, resp, resp_len);
        case 0x85: return svc_control_dtc(c, req, req_len, resp, resp_len);
        default:
            uds_build_neg(resp, resp_len, sid, NRC_SERVICE_NOT_SUPPORTED);
            return 0;
    }
}
