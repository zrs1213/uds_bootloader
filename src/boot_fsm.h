/**
 * @file    boot_fsm.h
 * @brief   诊断会话状态机 + S3 服务器超时 + 安全等级管理。
 *
 * 对应 ISO 14229-1 的 diagnostic session 概念：
 *   - 默认会话(0x01)：上电/复位后的初始态，服务能力受限
 *   - 编程会话(0x02)：刷写固件专用
 *   - 扩展会话(0x03)：解锁更多诊断/标定服务
 * S3_Server：非默认会话下若超过 S3 时间无有效请求，自动回落默认会话。
 */
#ifndef UDS_BOOT_FSM_H
#define UDS_BOOT_FSM_H

#include <stdint.h>

typedef enum {
    UDS_SESSION_DEFAULT      = 0x01,
    UDS_SESSION_PROGRAMMING  = 0x02,
    UDS_SESSION_EXTENDED     = 0x03
} uds_session_id_t;

typedef struct {
    uds_session_id_t session;
    uint8_t          security_level;  /* 0=锁定, >0=已解锁对应等级 */
    uint32_t         last_request_tick;
    uint32_t         s3_timeout_ms;   /* 服务器侧 S3，默认 5000ms */
} uds_fsm_t;

void    UDS_FsmInit(uds_fsm_t *f, uint32_t s3_ms);
uds_session_id_t UDS_FsmSession(const uds_fsm_t *f);
uint8_t UDS_FsmSecurity(const uds_fsm_t *f);

/* 切换会话（成功返回 0，参数非法返回非 0） */
uint8_t UDS_FsmSetSession(uds_fsm_t *f, uds_session_id_t s, uint32_t tick);

/* 设置安全等级（解锁/锁定） */
void    UDS_FsmSetSecurity(uds_fsm_t *f, uint8_t level);

/* 记录一次有效请求（刷新 S3 计时） */
void    UDS_FsmNoteActivity(uds_fsm_t *f, uint32_t tick);

/* 每次 tick 调用：处理 S3 超时回落默认会话；返回 1 表示刚刚发生回落 */
uint8_t UDS_FsmTick(uds_fsm_t *f, uint32_t tick);

#endif /* UDS_BOOT_FSM_H */
