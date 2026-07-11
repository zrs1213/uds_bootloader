/**
 * @file    test_can_tp.c
 * @brief   CAN-TP 传输层单元测试：SF/FF/CF/FC 构建与解析、接收重组、发送分段、STmin/BS。
 */
#include "minunit.h"
#include "can_tp.h"
#include <string.h>

/* ---------- mock 基础设施 ---------- */
static can_msg_t g_tx[128];
static int g_tx_n = 0;
static void mock_tx(const can_msg_t *m) { if (g_tx_n < 128) g_tx[g_tx_n++] = *m; }
static void reset_tx(void) { g_tx_n = 0; memset(g_tx, 0, sizeof(g_tx)); }

static uint32_t g_tick = 0;
static uint32_t mock_tick(void) { return g_tick; }

/* 把一个单帧/多帧序列发给接收端，返回最终完整报文 */
static cantp_status_t feed_and_reassemble(const can_msg_t *frames, int n,
                                           uint8_t *out, uint16_t *out_len)
{
    cantp_rx_t rx;
    CANTP_RxInit(&rx);
    cantp_status_t st = CANTP_OK;
    uint8_t complete = 0;
    for (int i = 0; i < n; i++) {
        st = CANTP_RxOnFrame(&rx, &frames[i], mock_tx, 0, 0, out, out_len, 4096, &complete);
        if (st != CANTP_OK) break;
        if (complete) break;
    }
    return st;
}

/* ===================== SF ===================== */
static void test_sf_zero_byte(void)
{
    uint8_t d[7] = {0};
    can_msg_t m;
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_BuildSF(d, 0, &m));
    TEST_ASSERT_EQUAL_HEX(0x00, m.data[0]);
    TEST_ASSERT_EQUAL(1, m.dlc);
}

static void test_sf_max_payload(void)
{
    uint8_t d[7] = {1,2,3,4,5,6,7};
    can_msg_t m;
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_BuildSF(d, 7, &m));
    TEST_ASSERT_EQUAL_HEX(0x07, m.data[0]);
    TEST_ASSERT_EQUAL(8, m.dlc);
    TEST_ASSERT_EQUAL_MEMORY(d, &m.data[1], 7);
}

static void test_sf_too_long(void)
{
    uint8_t d[8] = {0};
    can_msg_t m;
    TEST_ASSERT_EQUAL(CANTP_ERR_LENGTH, CANTP_BuildSF(d, 8, &m));
}

/* ===================== FF ===================== */
static void test_ff_8_bytes(void)
{
    uint8_t d[8] = {0,1,2,3,4,5,6,7};
    can_msg_t m;
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_BuildFF(d, 8, &m));
    TEST_ASSERT_EQUAL(8, m.dlc);
    TEST_ASSERT_EQUAL_HEX(0x10, m.data[0]); /* 0001 | 0 */
    TEST_ASSERT_EQUAL_HEX(0x08, m.data[1]); /* length low */
    TEST_ASSERT_EQUAL_MEMORY(d, &m.data[2], 6);
}

static void test_ff_4095(void)
{
    uint8_t d[8];
    for (int i=0;i<8;i++) d[i]=(uint8_t)i;
    can_msg_t m;
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_BuildFF(d, 4095, &m));
    TEST_ASSERT_EQUAL_HEX(0x1F, m.data[0]); /* 0001 | 0x0F */
    TEST_ASSERT_EQUAL_HEX(0xFF, m.data[1]);
}

static void test_ff_too_short(void)
{
    uint8_t d[8] = {0};
    can_msg_t m;
    TEST_ASSERT_EQUAL(CANTP_ERR_LENGTH, CANTP_BuildFF(d, 7, &m));
}

/* ===================== CF ===================== */
static void test_cf_sn_wrap(void)
{
    uint8_t d[7] = {9,9,9,9,9,9,9};
    can_msg_t m;
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_BuildCF(0, d, 7, &m));
    TEST_ASSERT_EQUAL_HEX(0x20, m.data[0]);
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_BuildCF(15, d, 7, &m));
    TEST_ASSERT_EQUAL_HEX(0x2F, m.data[0]);
}

/* ===================== ParsePCI ===================== */
static void test_parse_pci(void)
{
    cantp_pci_type_t t; uint16_t info;
    can_msg_t m;
    CANTP_BuildSF((uint8_t[]){1,2,3}, 3, &m);
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_ParsePCI(&m, &t, &info));
    TEST_ASSERT_EQUAL(CANTP_PCI_SF, t); TEST_ASSERT_EQUAL(3, info);

    CANTP_BuildFF((uint8_t[]){0,1,2,3,4,5,6,7}, 100, &m);
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_ParsePCI(&m, &t, &info));
    TEST_ASSERT_EQUAL(CANTP_PCI_FF, t); TEST_ASSERT_EQUAL(100, info);

    CANTP_BuildCF(5, (uint8_t[]){0}, 1, &m);
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_ParsePCI(&m, &t, &info));
    TEST_ASSERT_EQUAL(CANTP_PCI_CF, t); TEST_ASSERT_EQUAL(5, info);

    CANTP_BuildFC(CANTP_FS_CTS, 4, 10, &m);
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_ParsePCI(&m, &t, &info));
    TEST_ASSERT_EQUAL(CANTP_PCI_FC, t); TEST_ASSERT_EQUAL(CANTP_FS_CTS, info);
}

/* ===================== 接收端重组 ===================== */
static void test_rx_sf(void)
{
    uint8_t payload[7] = {'H','E','L','L','O',0,0};
    can_msg_t m; CANTP_BuildSF(payload, 5, &m);
    uint8_t out[64]; uint16_t out_len = 0;
    TEST_ASSERT_EQUAL(CANTP_OK, feed_and_reassemble(&m, 1, out, &out_len));
    TEST_ASSERT_EQUAL(5, out_len);
    TEST_ASSERT_EQUAL_MEMORY("HELLO", out, 5);
}

static void test_rx_multiframe_20(void)
{
    uint8_t data[20];
    for (int i=0;i<20;i++) data[i]=(uint8_t)(i+1);
    can_msg_t frames[4];
    CANTP_BuildFF(data, 20, &frames[0]);
    CANTP_BuildCF(1, &data[6],  7, &frames[1]);
    CANTP_BuildCF(2, &data[13], 7, &frames[2]); /* 20-6-7-7 = 0? wait: 6+7+7=20, last cf has 7 -> offset 13..19 =7 bytes */
    /* recompute: FF=6 bytes (0..5), CF1=7 (6..12), CF2=7 (13..19) total 20 ok */
    uint8_t out[64]; uint16_t out_len = 0;
    TEST_ASSERT_EQUAL(CANTP_OK, feed_and_reassemble(frames, 3, out, &out_len));
    TEST_ASSERT_EQUAL(20, out_len);
    TEST_ASSERT_EQUAL_MEMORY(data, out, 20);
}

static void test_rx_sequence_error(void)
{
    uint8_t data[20];
    for (int i=0;i<20;i++) data[i]=(uint8_t)i;
    can_msg_t frames[4];
    CANTP_BuildFF(data, 20, &frames[0]);
    CANTP_BuildCF(1, &data[6], 7, &frames[1]);
    CANTP_BuildCF(3, &data[13], 7, &frames[2]); /* 期望 SN=2，实际 3 -> 序列号错误 */
    uint8_t out[64]; uint16_t out_len = 0;
    cantp_rx_t rx; CANTP_RxInit(&rx); uint8_t comp=0;
    CANTP_RxOnFrame(&rx, &frames[0], mock_tx, 0, 0, out, &out_len, 4096, &comp);
    CANTP_RxOnFrame(&rx, &frames[1], mock_tx, 0, 0, out, &out_len, 4096, &comp);
    cantp_status_t st = CANTP_RxOnFrame(&rx, &frames[2], mock_tx, 0, 0, out, &out_len, 4096, &comp);
    TEST_ASSERT_EQUAL(CANTP_ERR_SEQUENCE, st);
}

/* ===================== 发送端分段 ===================== */
static void test_tx_single(void)
{
    reset_tx();
    uint8_t data[5] = {10,20,30,40,50};
    cantp_tx_t tx_ctx; CANTP_TxInit(&tx_ctx, data, 5);
    uint8_t done = 0;
    g_tick = 1000;
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_TxStep(&tx_ctx, mock_tx, mock_tick, &done));
    TEST_ASSERT_EQUAL(1, g_tx_n);
    TEST_ASSERT_EQUAL(1, done);
    TEST_ASSERT_EQUAL_HEX(0x05, g_tx[0].data[0]);
    TEST_ASSERT_EQUAL_MEMORY(data, &g_tx[0].data[1], 5);
}

static void test_tx_multiframe(void)
{
    reset_tx();
    uint8_t data[20];
    for (int i=0;i<20;i++) data[i]=(uint8_t)(i+1);
    cantp_tx_t tx_ctx; CANTP_TxInit(&tx_ctx, data, 20);
    uint8_t done = 0; g_tick = 0;
    /* 1) 发 FF（之后必须等 FC） */
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_TxStep(&tx_ctx, mock_tx, mock_tick, &done));
    TEST_ASSERT_EQUAL(1, g_tx_n);
    TEST_ASSERT_EQUAL(0, done);
    TEST_ASSERT_EQUAL_HEX(0x10, g_tx[0].data[0]);
    /* 2) 未收到 FC，再 step 应处于 PENDING，不新增帧 */
    TEST_ASSERT_EQUAL(CANTP_ERR_PENDING, CANTP_TxStep(&tx_ctx, mock_tx, mock_tick, &done));
    TEST_ASSERT_EQUAL(1, g_tx_n);
    /* 3) 收到 FC(CTS, BS=0, STmin=0) */
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_TxOnFlowControl(&tx_ctx, CANTP_FS_CTS, 0, 0));
    /* 4) 继续发送直到 done */
    int guard = 0;
    while (!done && guard++ < 50) {
        g_tick += 5; /* 满足 STmin=0 */
        CANTP_TxStep(&tx_ctx, mock_tx, mock_tick, &done);
    }
    TEST_ASSERT_EQUAL(1, done);
    /* 期望 1 FF + 2 CF = 3 帧 */
    TEST_ASSERT_EQUAL(3, g_tx_n);
    TEST_ASSERT_EQUAL_HEX(0x21, g_tx[1].data[0]); /* CF sn=1 */
    TEST_ASSERT_EQUAL_HEX(0x22, g_tx[2].data[0]); /* CF sn=2 */
    /* 重组验证 */
    uint8_t out[64]; uint16_t out_len=0;
    TEST_ASSERT_EQUAL(CANTP_OK, feed_and_reassemble(g_tx, 3, out, &out_len));
    TEST_ASSERT_EQUAL(20, out_len);
    TEST_ASSERT_EQUAL_MEMORY(data, out, 20);
}

/* 用 BS=1 验证块流控：每发 1 个 CF 需等待一次 FC */
static void test_tx_bs_flowcontrol(void)
{
    reset_tx();
    uint8_t data[20];
    for (int i=0;i<20;i++) data[i]=(uint8_t)(i+1);
    cantp_tx_t tx_ctx; CANTP_TxInit(&tx_ctx, data, 20);
    uint8_t done = 0; g_tick = 0;
    CANTP_TxStep(&tx_ctx, mock_tx, mock_tick, &done); /* FF */
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_TxOnFlowControl(&tx_ctx, CANTP_FS_CTS, 1, 0)); /* FC bs=1 */
    g_tick += 5; CANTP_TxStep(&tx_ctx, mock_tx, mock_tick, &done); /* CF1 */
    TEST_ASSERT_EQUAL(2, g_tx_n);
    /* 未收到新 FC -> PENDING */
    TEST_ASSERT_EQUAL(CANTP_ERR_PENDING, CANTP_TxStep(&tx_ctx, mock_tx, mock_tick, &done));
    TEST_ASSERT_EQUAL(2, g_tx_n);
    /* 收到新 FC -> 继续 */
    TEST_ASSERT_EQUAL(CANTP_OK, CANTP_TxOnFlowControl(&tx_ctx, CANTP_FS_CTS, 1, 0));
    g_tick += 5; CANTP_TxStep(&tx_ctx, mock_tx, mock_tick, &done); /* CF2 */
    TEST_ASSERT_EQUAL(3, g_tx_n);
}

void run_can_tp_tests(void)
{
    printf("\n[CAN-TP]\n");
    MU_RUN_TEST(test_sf_zero_byte);
    MU_RUN_TEST(test_sf_max_payload);
    MU_RUN_TEST(test_sf_too_long);
    MU_RUN_TEST(test_ff_8_bytes);
    MU_RUN_TEST(test_ff_4095);
    MU_RUN_TEST(test_ff_too_short);
    MU_RUN_TEST(test_cf_sn_wrap);
    MU_RUN_TEST(test_parse_pci);
    MU_RUN_TEST(test_rx_sf);
    MU_RUN_TEST(test_rx_multiframe_20);
    MU_RUN_TEST(test_rx_sequence_error);
    MU_RUN_TEST(test_tx_single);
    MU_RUN_TEST(test_tx_multiframe);
    MU_RUN_TEST(test_tx_bs_flowcontrol);
}
