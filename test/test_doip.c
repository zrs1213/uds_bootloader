/**
 * @file    test_doip.c
 * @brief   DoIP 传输层单元测试：头部编解码、流式解帧（粘包/半包/坏头重同步）、
 *          路由激活状态机、诊断消息 ACK/NACK 规则、UDS over DoIP 全链路。
 */
#include "minunit.h"
#include "doip.h"
#include <string.h>

#define ECU_ADDR    0x0E80u
#define TESTER_ADDR 0x0F00u
#define VIN17       "LSVUDS00000000000"

/* ---------------- mock 发送：捕获整帧 ---------------- */
#define CAP_MAX   8
#define CAP_DATA  600
typedef struct { uint16_t type; uint32_t len; uint8_t d[CAP_DATA]; } cap_t;
static cap_t g_cap[CAP_MAX];
static int   g_cap_n = 0;

static void cap_tx(uint16_t type, const uint8_t *payload, uint32_t len, void *user)
{
    cap_t *c;
    (void)user;
    if (g_cap_n >= CAP_MAX) return;
    c = &g_cap[g_cap_n++];
    c->type = type;
    c->len  = len;
    memcpy(c->d, payload, len < CAP_DATA ? len : CAP_DATA);
}
static void cap_reset(void) { g_cap_n = 0; memset(g_cap, 0, sizeof(g_cap)); }

/* 取捕获消息的载荷指针/长度 */
static const uint8_t *cap_payload(int i, uint32_t *plen)
{
    if (plen) *plen = g_cap[i].len;
    return g_cap[i].d;
}

/* ---------------- 被测上下文 ---------------- */
static uds_ctx_t   g_uds;
static doip_ecu_t  g_ecu;

static void setup(void)
{
    const uint8_t master[16] = {0xDE,0xAD,0xBE,0xEF,0x01,0x23,0x45,0x67,
                                0x89,0xAB,0xCD,0xEF,0xFE,0xDC,0xBA,0x98};
    UDS_Init(&g_uds);
    SEC_Init(master);
    DOIP_EcuInit(&g_ecu, ECU_ADDR, VIN17);
    cap_reset();
}

/* 直接投递一条完整消息 */
static void deliver(uint16_t type, const uint8_t *p, uint32_t len)
{
    DOIP_EcuOnMessage(&g_ecu, &g_uds, type, p, len, cap_tx, NULL);
}

static void route_activate(void)
{
    uint8_t req[6] = { (uint8_t)(TESTER_ADDR>>8), (uint8_t)TESTER_ADDR, 0,0,0,0 };
    cap_reset();
    deliver(DOIP_PT_ROUTING_ACT_REQ, req, 6);
}

/* ---------------- 头部编解码 ---------------- */
static void test_header_roundtrip(void)
{
    uint8_t b[8]; uint16_t t; uint32_t l;
    DOIP_WriteHeader(b, 0x8001, 4099);
    TEST_ASSERT_EQUAL(DOIP_ReadHeader(b, 8, &t, &l), 0);
    TEST_ASSERT_EQUAL_HEX(t, 0x8001);
    TEST_ASSERT_EQUAL(l, 4099);
    /* 半头 */
    TEST_ASSERT_EQUAL(DOIP_ReadHeader(b, 7, &t, &l), 1);
    /* 坏版本 */
    b[1] = 0x00;
    TEST_ASSERT_EQUAL(DOIP_ReadHeader(b, 8, &t, &l), -1);
}

/* ---------------- 流式解帧 ---------------- */
typedef struct { uint16_t type; uint32_t len; uint8_t first; uint8_t last; } seen_t;
static seen_t g_seen[8]; static int g_seen_n = 0;
static void seen_cb(uint16_t type, const uint8_t *p, uint32_t len, void *user)
{
    (void)user;
    if (g_seen_n < 8) {
        g_seen[g_seen_n].type = type;
        g_seen[g_seen_n].len  = len;
        g_seen[g_seen_n].first = len ? p[0] : 0;
        g_seen[g_seen_n].last  = len ? p[len-1] : 0;
        g_seen_n++;
    }
}

static void build_msg(uint8_t *out, uint16_t type, const uint8_t *p, uint32_t plen)
{
    uint32_t i;
    DOIP_WriteHeader(out, type, plen);
    for (i = 0; i < plen; i++) out[DOIP_HDR_LEN + i] = p[i];
}

static void test_stream_split(void)
{
    doip_stream_t s; uint8_t msg[16], payload[8] = {1,2,3,4,5,6,7,8};
    uint32_t n; int i;
    build_msg(msg, 0x8001, payload, 8);
    n = DOIP_HDR_LEN + 8;
    DOIP_StreamInit(&s);
    g_seen_n = 0;
    for (i = 0; i < (int)n; i += 3) {           /* 每次喂 3 字节 */
        uint32_t chunk = (n - (uint32_t)i) < 3 ? (n - (uint32_t)i) : 3;
        DOIP_StreamFeed(&s, msg + i, chunk, seen_cb, NULL);
    }
    TEST_ASSERT_EQUAL(g_seen_n, 1);
    TEST_ASSERT_EQUAL_HEX(g_seen[0].type, 0x8001);
    TEST_ASSERT_EQUAL(g_seen[0].len, 8);
    TEST_ASSERT_EQUAL(g_seen[0].first, 1);
    TEST_ASSERT_EQUAL(g_seen[0].last, 8);
}

static void test_stream_two_in_one(void)
{
    doip_stream_t s;
    uint8_t buf[64], p1[4] = {0xA1,2,3,4}, p2[5] = {0xB1,2,3,4,5};
    uint32_t off;
    DOIP_StreamInit(&s);
    g_seen_n = 0;
    build_msg(buf, 0x0005, p1, 4); off = 12;
    build_msg(buf + off, 0x0006, p2, 5); off += 13;
    DOIP_StreamFeed(&s, buf, off, seen_cb, NULL);
    TEST_ASSERT_EQUAL(g_seen_n, 2);
    TEST_ASSERT_EQUAL_HEX(g_seen[0].type, 0x0005);
    TEST_ASSERT_EQUAL_HEX(g_seen[1].type, 0x0006);
    TEST_ASSERT_EQUAL(g_seen[1].first, 0xB1);
}

static void test_stream_resync(void)
{
    doip_stream_t s;
    uint8_t buf[64], pl[2] = {0x10, 0x01};
    uint32_t off = 0;
    DOIP_StreamInit(&s);
    g_seen_n = 0;
    buf[off++] = 0xAA;                       /* 垃圾字节 */
    buf[off++] = 0x02; buf[off++] = 0xFE;    /* 坏反码头 */
    memset(buf + off, 0, 6); off += 6;
    build_msg(buf + off, 0x8001, pl, 2);     /* 之后才是合法消息 */
    off += 10;
    DOIP_StreamFeed(&s, buf, off, seen_cb, NULL);
    TEST_ASSERT_EQUAL(g_seen_n, 1);
    TEST_ASSERT_EQUAL_HEX(g_seen[0].type, 0x8001);
    TEST_ASSERT_EQUAL(g_seen[0].first, 0x10);
}

static void test_stream_oversize(void)
{
    doip_stream_t s; uint8_t hdr[8];
    DOIP_StreamInit(&s);
    g_seen_n = 0;
    DOIP_WriteHeader(hdr, 0x8001, 0xFFFFFFFFu);  /* 谎报超长 */
    DOIP_StreamFeed(&s, hdr, 8, seen_cb, NULL);
    TEST_ASSERT_EQUAL(g_seen_n, 0);
    TEST_ASSERT_EQUAL(s.len, 0);                 /* 缓冲已清空重同步 */
}

/* ---------------- 路由激活 ---------------- */
static void test_routing_activation(void)
{
    uint32_t plen; const uint8_t *p;
    setup();
    route_activate();
    TEST_ASSERT_EQUAL(g_cap_n, 1);
    TEST_ASSERT_EQUAL_HEX(g_cap[0].type, DOIP_PT_ROUTING_ACT_RESP);
    p = cap_payload(0, &plen);
    TEST_ASSERT_EQUAL(plen, 7);
    TEST_ASSERT_EQUAL_HEX(p[4], DOIP_RAR_SUCCESS);
    TEST_ASSERT_EQUAL_HEX((p[0] << 8) | p[1], TESTER_ADDR);
    TEST_ASSERT_EQUAL_HEX((p[2] << 8) | p[3], ECU_ADDR);
    /* 重复激活 -> 0x11 */
    cap_reset(); route_activate();
    p = cap_payload(0, &plen);
    TEST_ASSERT_EQUAL_HEX(p[4], DOIP_RAR_ALREADY_ACTIVE);
}

static void test_routing_reject(void)
{
    uint32_t plen; const uint8_t *p;
    uint8_t bad[3] = {0x0F, 0x00, 0x00};
    uint8_t zero_sa[6] = {0x00, 0x00, 0,0,0,0};
    setup();
    deliver(DOIP_PT_ROUTING_ACT_REQ, bad, 3);      /* 长度不足 */
    p = cap_payload(0, &plen);
    TEST_ASSERT_EQUAL_HEX(p[4], DOIP_RAR_REJECTED_FORMAT);
    cap_reset();
    deliver(DOIP_PT_ROUTING_ACT_REQ, zero_sa, 6);  /* SA=0 */
    p = cap_payload(0, &plen);
    TEST_ASSERT_EQUAL_HEX(p[4], DOIP_RAR_REJECTED_UNKNOWN_SA);
    TEST_ASSERT_EQUAL(g_ecu.routing_active, 0);
}

/* ---------------- 诊断消息 NACK 规则 ---------------- */
static void send_diag(uint16_t sa, uint16_t ta, const uint8_t *uds, uint32_t ulen)
{
    uint8_t buf[64]; uint32_t i;
    buf[0] = (uint8_t)(sa >> 8); buf[1] = (uint8_t)sa;
    buf[2] = (uint8_t)(ta >> 8); buf[3] = (uint8_t)ta;
    for (i = 0; i < ulen; i++) buf[4 + i] = uds[i];
    cap_reset();
    deliver(DOIP_PT_DIAG_MESSAGE, buf, 4 + ulen);
}

static void test_diag_before_activation(void)
{
    uint8_t req[2] = {0x10, 0x01}; uint32_t plen; const uint8_t *p;
    setup();
    send_diag(TESTER_ADDR, ECU_ADDR, req, 2);
    TEST_ASSERT_EQUAL(g_cap_n, 1);
    TEST_ASSERT_EQUAL_HEX(g_cap[0].type, DOIP_PT_DIAG_MESSAGE_NACK);
    p = cap_payload(0, &plen);
    TEST_ASSERT_EQUAL_HEX(p[4], DOIP_NACK_NOT_ACTIVATED);
}

static void test_diag_addr_checks(void)
{
    uint8_t req[2] = {0x10, 0x01}; uint32_t plen; const uint8_t *p;
    setup(); route_activate();
    send_diag(0x0EEE, ECU_ADDR, req, 2);           /* 错误 SA */
    p = cap_payload(0, &plen);
    TEST_ASSERT_EQUAL_HEX(g_cap[0].type, DOIP_PT_DIAG_MESSAGE_NACK);
    TEST_ASSERT_EQUAL_HEX(p[4], DOIP_NACK_INVALID_SA);
    cap_reset();
    send_diag(TESTER_ADDR, 0x0EEE, req, 2);        /* 错误 TA */
    p = cap_payload(0, &plen);
    TEST_ASSERT_EQUAL_HEX(p[4], DOIP_NACK_UNKNOWN_TA);
}

/* ---------------- UDS over DoIP 全链路 ---------------- */
static void test_diag_session_over_doip(void)
{
    uint8_t req[2] = {0x10, 0x01};
    uint32_t plen; const uint8_t *p;
    setup(); route_activate();
    send_diag(TESTER_ADDR, ECU_ADDR, req, 2);
    /* 期望两条：ACK 与诊断响应 */
    TEST_ASSERT_EQUAL(g_cap_n, 2);
    TEST_ASSERT_EQUAL_HEX(g_cap[0].type, DOIP_PT_DIAG_MESSAGE_ACK);
    p = cap_payload(0, &plen);
    TEST_ASSERT_EQUAL_HEX(p[4], 0x00);
    TEST_ASSERT_EQUAL_HEX(g_cap[1].type, DOIP_PT_DIAG_MESSAGE);
    p = cap_payload(1, &plen);
    TEST_ASSERT_EQUAL_HEX((p[0] << 8) | p[1], ECU_ADDR);     /* 回封 SA=ECU */
    TEST_ASSERT_EQUAL_HEX((p[2] << 8) | p[3], TESTER_ADDR);  /* TA=tester */
    TEST_ASSERT_EQUAL_HEX(p[4], 0x50);                       /* 0x10 正响应 */
    TEST_ASSERT_EQUAL_HEX(p[5], 0x01);
}

static void test_diag_read_did_over_doip(void)
{
    uint8_t req[3] = {0x22, 0xF1, 0x90};
    uint32_t plen; const uint8_t *p;
    setup(); route_activate();
    send_diag(TESTER_ADDR, ECU_ADDR, req, 3);
    TEST_ASSERT_EQUAL(g_cap_n, 2);
    p = cap_payload(1, &plen);
    TEST_ASSERT_EQUAL_HEX(p[4], 0x62);
    TEST_ASSERT_EQUAL_HEX(p[5], 0xF1);
    TEST_ASSERT_EQUAL_HEX(p[6], 0x90);
    TEST_ASSERT_EQUAL(plen, 4 + 3 + 16);   /* SA/TA + 62 F1 90 + VIN16 */
}

static void test_diag_neg_response_over_doip(void)
{
    uint8_t req[2] = {0x22, 0x11};   /* 长度不足 -> NRC 0x13/0x11 类 */
    uint32_t plen; const uint8_t *p;
    setup(); route_activate();
    send_diag(TESTER_ADDR, ECU_ADDR, req, 2);
    p = cap_payload(1, &plen);
    TEST_ASSERT_EQUAL_HEX(p[4], 0x7F);           /* 负响应 */
    TEST_ASSERT_EQUAL_HEX(p[5], 0x22);
}

/* ---------------- 保活与车辆宣告 ---------------- */
static void test_alive_check(void)
{
    uint32_t plen; const uint8_t *p;
    setup();
    deliver(DOIP_PT_ALIVE_CHECK_REQ, NULL, 0);
    TEST_ASSERT_EQUAL(g_cap_n, 1);
    TEST_ASSERT_EQUAL_HEX(g_cap[0].type, DOIP_PT_ALIVE_CHECK_RESP);
    p = cap_payload(0, &plen);
    TEST_ASSERT_EQUAL_HEX((p[0] << 8) | p[1], ECU_ADDR);
}

static void test_vehicle_id_vin(void)
{
    uint32_t plen; const uint8_t *p;
    setup();
    deliver(DOIP_PT_VEH_ID_REQ_VIN, (const uint8_t *)VIN17, 17);
    TEST_ASSERT_EQUAL(g_cap_n, 1);
    TEST_ASSERT_EQUAL_HEX(g_cap[0].type, DOIP_PT_VEH_ID_RESP);
    p = cap_payload(0, &plen);
    TEST_ASSERT_EQUAL(plen, 68);
    TEST_ASSERT_EQUAL_MEMORY(p, VIN17, 17);
    TEST_ASSERT_EQUAL_HEX((p[17] << 8) | p[18], ECU_ADDR);
    cap_reset();
    deliver(DOIP_PT_VEH_ID_REQ_VIN, (const uint8_t *)"XXXXXXXXXXXXXXXXXXX", 17);
    TEST_ASSERT_EQUAL(g_cap_n, 0);   /* VIN 不匹配不应答 */
}

void run_doip_tests(void)
{
    printf("\n[DoIP]\n");
    MU_RUN_TEST(test_header_roundtrip);
    MU_RUN_TEST(test_stream_split);
    MU_RUN_TEST(test_stream_two_in_one);
    MU_RUN_TEST(test_stream_resync);
    MU_RUN_TEST(test_stream_oversize);
    MU_RUN_TEST(test_routing_activation);
    MU_RUN_TEST(test_routing_reject);
    MU_RUN_TEST(test_diag_before_activation);
    MU_RUN_TEST(test_diag_addr_checks);
    MU_RUN_TEST(test_diag_session_over_doip);
    MU_RUN_TEST(test_diag_read_did_over_doip);
    MU_RUN_TEST(test_diag_neg_response_over_doip);
    MU_RUN_TEST(test_alive_check);
    MU_RUN_TEST(test_vehicle_id_vin);
}
