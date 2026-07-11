/**
 * @file    boot_fsm.c
 * @brief   诊断会话状态机实现。
 */
#include "boot_fsm.h"

void UDS_FsmInit(uds_fsm_t *f, uint32_t s3_ms) {
    f->session          = UDS_SESSION_DEFAULT;
    f->security_level   = 0;
    f->last_request_tick = 0;
    f->s3_timeout_ms    = s3_ms;
}

uds_session_id_t UDS_FsmSession(const uds_fsm_t *f) {
    return f->session;
}

uint8_t UDS_FsmSecurity(const uds_fsm_t *f) {
    return f->security_level;
}

uint8_t UDS_FsmSetSession(uds_fsm_t *f, uds_session_id_t s, uint32_t tick) {
    if (s != UDS_SESSION_DEFAULT &&
        s != UDS_SESSION_PROGRAMMING &&
        s != UDS_SESSION_EXTENDED) {
        return 1; /* 非法会话参数 */
    }
    f->session = s;
    /* 会话切换后安全等级复位（ISO 14229 要求） */
    f->security_level = 0;
    f->last_request_tick = tick;
    return 0;
}

void UDS_FsmSetSecurity(uds_fsm_t *f, uint8_t level) {
    f->security_level = level;
}

void UDS_FsmNoteActivity(uds_fsm_t *f, uint32_t tick) {
    f->last_request_tick = tick;
}

uint8_t UDS_FsmTick(uds_fsm_t *f, uint32_t tick) {
    if (f->session == UDS_SESSION_DEFAULT) return 0;

    uint32_t elapsed;
    if (tick >= f->last_request_tick) {
        elapsed = tick - f->last_request_tick;
    } else {
        /* tick 计数器回绕（32 位） */
        elapsed = (0xFFFFFFFFu - f->last_request_tick) + tick + 1;
    }

    if (elapsed >= f->s3_timeout_ms) {
        f->session = UDS_SESSION_DEFAULT;
        f->security_level = 0;
        return 1; /* 发生 S3 回落 */
    }
    return 0;
}
