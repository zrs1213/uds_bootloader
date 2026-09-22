/**
 * @file    doip_ecu_tcp.c
 * @brief   DoIP ECU 侧演示程序（Windows/Winsock2）：
 *          在 TCP 13400 端口模拟一台支持 DoIP 的诊断 ECU，
 *          诊断请求经 src/doip.c 解帧后直接进入现有 UDS 协议栈
 *          （与 CAN 链路共用同一个 UDS_ProcessRequest，应用层零改动）。
 *
 * 用法：
 *   build\doip_ecu.exe [port]        默认 13400
 *   python tools\doip_client.py --tcp 127.0.0.1   另一端跑完整刷写演示
 *
 * 说明：演示用单连接阻塞式 select 循环，重点是链路正确性而非并发；
 *       移植到 RTOS 时把 recv/send 换成 lwIP socket 即可，协议层不变。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "doip.h"
#include "security.h"

#define ECU_LADDR 0x0E80u
#define VIN17     "LSVUDS00000000000"

static uds_ctx_t    g_uds;
static doip_ecu_t   g_ecu;
static doip_stream_t g_stream;
static SOCKET       g_client = INVALID_SOCKET;

static const char *pt_name(uint16_t t)
{
    switch (t) {
    case DOIP_PT_ANNOUNCEMENT_REQ:  return "AnnounceReq";
    case DOIP_PT_VEH_ID_REQ_VIN:    return "VehIDReq(VIN)";
    case DOIP_PT_VEH_ID_RESP:       return "VehIDResp";
    case DOIP_PT_ROUTING_ACT_REQ:   return "RoutingActReq";
    case DOIP_PT_ROUTING_ACT_RESP:  return "RoutingActResp";
    case DOIP_PT_ALIVE_CHECK_REQ:   return "AliveChkReq";
    case DOIP_PT_ALIVE_CHECK_RESP:  return "AliveChkResp";
    case DOIP_PT_DIAG_MESSAGE:      return "DiagMessage";
    case DOIP_PT_DIAG_MESSAGE_ACK:  return "DiagACK";
    case DOIP_PT_DIAG_MESSAGE_NACK: return "DiagNACK";
    default: return "Unknown";
    }
}

static void hexdump(const uint8_t *p, uint32_t n, uint32_t cap)
{
    uint32_t i;
    if (n > cap) n = cap;
    for (i = 0; i < n; i++) printf("%02X ", p[i]);
    if (n < cap && n) printf(" ");
}

/* tx 回调：拼 8 字节头后写回 socket */
static void ecu_tx(uint16_t type, const uint8_t *payload, uint32_t len, void *user)
{
    static uint8_t frame[DOIP_HDR_LEN + DOIP_MAX_PAYLOAD];
    uint32_t total;
    (void)user;
    printf("[TX] %-14s len=%u payload: ", pt_name(type), len);
    hexdump(payload, len, 24);
    printf("\n");
    if (g_client == INVALID_SOCKET || len > DOIP_MAX_PAYLOAD) return;
    DOIP_WriteHeader(frame, type, len);
    if (len) memcpy(frame + DOIP_HDR_LEN, payload, len);
    total = DOIP_HDR_LEN + len;
    {
        uint32_t sent = 0;
        while (sent < total) {
            int r = send(g_client, (const char *)frame + sent, (int)(total - sent), 0);
            if (r <= 0) { printf("[TX] socket error\n"); return; }
            sent += (uint32_t)r;
        }
    }
}

/* 解帧回调：交给 ECU 分发器 */
static void on_msg(uint16_t type, const uint8_t *p, uint32_t len, void *user)
{
    (void)user;
    printf("[RX] %-14s len=%u payload: ", pt_name(type), len);
    hexdump(p, len, 24);
    printf("\n");
    DOIP_EcuOnMessage(&g_ecu, &g_uds, type, p, len, ecu_tx, NULL);
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    SOCKET lsock;
    struct sockaddr_in addr;
    u_long nonblock = 1;
    uint16_t port = 13400;
    const uint8_t master[16] = {0xDE,0xAD,0xBE,0xEF,0x01,0x23,0x45,0x67,
                                0x89,0xAB,0xCD,0xEF,0xFE,0xDC,0xBA,0x98};

    if (argc > 1) port = (uint16_t)atoi(argv[1]);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    UDS_Init(&g_uds);
    SEC_Init(master);
    SEC_RngSeed((uint32_t)GetTickCount());
    DOIP_EcuInit(&g_ecu, ECU_LADDR, VIN17);
    DOIP_StreamInit(&g_stream);

    lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("bind :%u failed (占用或权限)\n", port);
        return 1;
    }
    listen(lsock, 2);
    printf("DoIP ECU demo listening on TCP :%u  (ECU addr 0x%04X, VIN %s)\n",
           port, ECU_LADDR, VIN17);
    printf("master key = DEADBEEF01234567 89ABCDEF FEDCBA98 (与 test 一致)\n");

    for (;;) {
        fd_set rfds;
        SOCKET maxfd = lsock;
        struct timeval tv = { 0, 100000 };  /* 100ms 轮询，Ctrl+C 可退出 */
        FD_ZERO(&rfds);
        FD_SET(lsock, &rfds);
        if (g_client != INVALID_SOCKET) { FD_SET(g_client, &rfds); if (g_client > maxfd) maxfd = g_client; }
        if (select((int)maxfd + 1, &rfds, NULL, NULL, &tv) > 0) {
            if (FD_ISSET(lsock, &rfds)) {
                SOCKET c = accept(lsock, NULL, NULL);
                if (c != INVALID_SOCKET) {
                    if (g_client != INVALID_SOCKET) closesocket(g_client);  /* 单连接：踢掉旧客户端 */
                    g_client = c;
                    DOIP_StreamInit(&g_stream);
                    ioctlsocket(c, FIONBIO, &nonblock);
                    printf("[LINK] tester connected\n");
                }
            }
            if (g_client != INVALID_SOCKET && FD_ISSET(g_client, &rfds)) {
                uint8_t buf[2048];
                int r = recv(g_client, (char *)buf, sizeof(buf), 0);
                if (r > 0) {
                    DOIP_StreamFeed(&g_stream, buf, (uint32_t)r, on_msg, NULL);
                } else {
                    printf("[LINK] tester disconnected\n");
                    closesocket(g_client);
                    g_client = INVALID_SOCKET;
                }
            }
        }
    }
    /* 演示程序常驻，Ctrl+C 结束 */
    return 0;
}
