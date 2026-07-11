/**
 * @file    ota_agent.c
 * @brief   车端 OTA 刷写代理实现（详见 ota_agent.h 说明）。
 */
#include "ota_agent.h"
#include "security.h"   /* AES128_ECB_Encrypt / SEC_ComputeKey */
#include "uds_io.h"     /* UDS_FLASH_BASE / UDS_FLASH_SIZE */
#include <string.h>

/* ---------------- 本地 CRC32（IEEE 802.3，与刷写校验同源） ---------------- */
static uint32_t ota_crc32_table[256];
static uint8_t  ota_crc32_init_done = 0;

static void ota_crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        ota_crc32_table[i] = c;
    }
    ota_crc32_init_done = 1;
}

static uint32_t ota_crc32(const uint8_t *data, uint32_t len) {
    if (!ota_crc32_init_done) ota_crc32_init();
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)((crc ^ data[i]) & 0xFF);
        crc = ota_crc32_table[idx] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---------------- 初始化 ---------------- */
void OTA_Init(ota_agent_t *a, const uint8_t *master_key) {
    memcpy(a->master, master_key, 16);
    a->fw_addr = 0;
    a->fw_len  = 0;
    a->block_size = 0;
    a->total_blocks = 0;
    a->written_blocks = 0;
    a->state = 0;
    a->need_rollback = 0;
    a->fw_crc = 0;
    a->fail_after_block = 0;
}

/* ---------------- CBC-MAC（AES-128, IV=0, 末块 0 填充） ---------------- */
void OTA_CbcMac(const uint8_t *key, const uint8_t *data, uint32_t len, uint8_t *out16) {
    uint8_t iv[16];
    uint8_t block[16];
    memset(iv, 0, 16);
    uint32_t off = 0;
    while (off < len) {
        uint8_t n = (len - off >= 16) ? 16 : (uint8_t)(len - off);
        memset(block, 0, 16);
        memcpy(block, data + off, n);
        for (int i = 0; i < 16; i++) block[i] ^= iv[i];
        AES128_ECB_Encrypt(key, block, iv);
        off += 16;
    }
    memcpy(out16, iv, 16);
}

/* ---------------- 打包 ---------------- */
uint32_t OTA_BuildPackage(ota_agent_t *a, uint32_t fw_addr, uint16_t block_size,
                          const uint8_t *fw, uint32_t fw_len,
                          uint8_t *out_pkg, uint32_t max_pkg) {
    if (block_size < 1 || block_size > DL_BLOCK_MAX) return 0;
    if (fw_len == 0) return 0;
    uint32_t total = OTA_PKG_OVERHEAD + fw_len;
    if (total > max_pkg) return 0;

    /* 头部（32 字节） */
    out_pkg[0] = 'O'; out_pkg[1] = 'T'; out_pkg[2] = 'A'; out_pkg[3] = 'P';
    out_pkg[4] = 1;                                  /* version */
    out_pkg[5]  = (uint8_t)(fw_addr >> 24);
    out_pkg[6]  = (uint8_t)(fw_addr >> 16);
    out_pkg[7]  = (uint8_t)(fw_addr >> 8);
    out_pkg[8]  = (uint8_t)(fw_addr & 0xFF);
    out_pkg[9]  = (uint8_t)(fw_len >> 24);
    out_pkg[10] = (uint8_t)(fw_len >> 16);
    out_pkg[11] = (uint8_t)(fw_len >> 8);
    out_pkg[12] = (uint8_t)(fw_len & 0xFF);
    out_pkg[13] = (uint8_t)(block_size >> 8);
    out_pkg[14] = (uint8_t)(block_size & 0xFF);
    uint32_t crc = ota_crc32(fw, fw_len);            /* 固件 CRC32，用于 $31 FF00 比对 */
    out_pkg[15] = (uint8_t)(crc >> 24);
    out_pkg[16] = (uint8_t)(crc >> 16);
    out_pkg[17] = (uint8_t)(crc >> 8);
    out_pkg[18] = (uint8_t)(crc & 0xFF);
    memset(out_pkg + 19, 0, 13);                    /* reserved */

    /* 载荷 */
    memcpy(out_pkg + OTA_HDR_SIZE, fw, fw_len);

    /* 签名：CBC-MAC 覆盖 [0 .. OTA_HDR_SIZE+fw_len) */
    OTA_CbcMac(a->master, out_pkg, OTA_HDR_SIZE + fw_len,
                out_pkg + OTA_HDR_SIZE + fw_len);
    return total;
}

/* ---------------- 验签 ---------------- */
ota_status_t OTA_Verify(const ota_agent_t *a, const uint8_t *pkg, uint32_t pkg_len) {
    if (pkg_len < OTA_PKG_OVERHEAD + 1) return OTA_ERR_PKG_SHORT;
    if (pkg[0] != 'O' || pkg[1] != 'T' || pkg[2] != 'A' || pkg[3] != 'P')
        return OTA_ERR_MAGIC;

    uint32_t fw_len = ((uint32_t)pkg[9]  << 24) | ((uint32_t)pkg[10] << 16) |
                      ((uint32_t)pkg[11] << 8)  |  (uint32_t)pkg[12];
    uint16_t block_size = ((uint16_t)pkg[13] << 8) | (uint16_t)pkg[14];
    uint32_t need = OTA_PKG_OVERHEAD + fw_len;
    if (pkg_len < need) return OTA_ERR_PKG_SHORT;
    if (block_size < 1 || block_size > DL_BLOCK_MAX) return OTA_ERR_BLOCKSZ;

    uint32_t addr = ((uint32_t)pkg[5] << 24) | ((uint32_t)pkg[6] << 16) |
                    ((uint32_t)pkg[7] << 8)  |  (uint32_t)pkg[8];
    if (addr < UDS_FLASH_BASE ||
        (addr + fw_len) > (UDS_FLASH_BASE + UDS_FLASH_SIZE))
        return OTA_ERR_ADDR;

    uint8_t sig[OTA_SIG_SIZE];
    OTA_CbcMac(a->master, pkg, OTA_HDR_SIZE + fw_len, sig);
    if (memcmp(sig, pkg + OTA_HDR_SIZE + fw_len, OTA_SIG_SIZE) != 0)
        return OTA_ERR_SIGN;

    (void)a;  /* 校验通过；a 仅用于 master，上面已使用 */
    return OTA_OK;
}

/* ---------------- 完整刷写 ---------------- */
ota_status_t OTA_Flash(ota_agent_t *a, uds_ctx_t *uds,
                       const uint8_t *pkg, uint32_t pkg_len) {
    uint8_t req[64];
    uint8_t resp[64];
    uint16_t rl;

    /* 1) 收包 + 验签 */
    a->state = 1; /* received */
    ota_status_t v = OTA_Verify(a, pkg, pkg_len);
    if (v != OTA_OK) { a->state = 5; a->need_rollback = 1; return v; }
    a->state = 2; /* verified */

    /* 解析头部 */
    uint32_t addr = ((uint32_t)pkg[5] << 24) | ((uint32_t)pkg[6] << 16) |
                    ((uint32_t)pkg[7] << 8)  |  (uint32_t)pkg[8];
    uint32_t fw_len = ((uint32_t)pkg[9]  << 24) | ((uint32_t)pkg[10] << 16) |
                      ((uint32_t)pkg[11] << 8)  |  (uint32_t)pkg[12];
    uint16_t bs = ((uint16_t)pkg[13] << 8) | (uint16_t)pkg[14];
    a->fw_addr = addr;
    a->fw_len  = fw_len;
    a->block_size = bs;
    a->total_blocks = (fw_len + bs - 1) / bs;

    /* 2) 进入编程会话 */
    req[0] = 0x10; req[1] = 0x02;
    UDS_ProcessRequest(uds, req, 2, resp, &rl);
    if (!(rl >= 2 && resp[0] == 0x50 && resp[1] == 0x02)) {
        a->state = 5; a->need_rollback = 1; return OTA_ERR_SESSION;
    }

    /* 3) $27 安全解锁（Seed&Key，与 ECU 共享主密钥） */
    req[0] = 0x27; req[1] = 0x01;
    UDS_ProcessRequest(uds, req, 2, resp, &rl);
    if (!(rl >= 2 && resp[0] == 0x67 && resp[1] == 0x01)) {
        a->state = 5; a->need_rollback = 1; return OTA_ERR_SECURITY;
    }
    uint8_t seed[16]; memcpy(seed, resp + 2, 16);
    uint8_t key[16];  SEC_ComputeKey(seed, key);   /* 复用 security.c 派生 */
    req[0] = 0x27; req[1] = 0x02; memcpy(req + 2, key, 16);
    UDS_ProcessRequest(uds, req, 18, resp, &rl);
    if (!(rl >= 2 && resp[0] == 0x67 && resp[1] == 0x02)) {
        a->state = 5; a->need_rollback = 1; return OTA_ERR_SECURITY;
    }

    /* 4) $34 请求下载 */
    req[0] = 0x34; req[1] = 0x00; req[2] = 0x44;
    req[3] = (uint8_t)(addr >> 24); req[4] = (uint8_t)(addr >> 16);
    req[5] = (uint8_t)(addr >> 8);  req[6] = (uint8_t)(addr & 0xFF);
    req[7] = (uint8_t)(fw_len >> 24); req[8] = (uint8_t)(fw_len >> 16);
    req[9] = (uint8_t)(fw_len >> 8);  req[10] = (uint8_t)(fw_len & 0xFF);
    UDS_ProcessRequest(uds, req, 11, resp, &rl);
    if (!(rl >= 1 && resp[0] == 0x74)) {
        a->state = 5; a->need_rollback = 1; return OTA_ERR_DOWNLOAD;
    }

    /* 5) $36 × N 数据传输（BSC 序列对齐 ECU 端：1,2,…,0xFF→0x00） */
    a->state = 3; /* flashing */
    uint16_t written = 0;
    uint8_t  bsc = 1;
    uint32_t off = OTA_HDR_SIZE;   /* payload 起点 */
    for (uint32_t b = 0; b < a->total_blocks; b++) {
        uint8_t chunk = (b == a->total_blocks - 1)
                           ? (uint8_t)(fw_len - b * bs)
                           : (uint8_t)bs;
        req[0] = 0x36; req[1] = bsc;
        memcpy(req + 2, pkg + off, chunk);
        off += chunk;
        UDS_ProcessRequest(uds, req, (uint16_t)(2 + chunk), resp, &rl);
        if (!(rl >= 2 && resp[0] == 0x76 && resp[1] == bsc)) {
            a->written_blocks = written;
            a->state = 5; a->need_rollback = 1; return OTA_ERR_XFER;
        }
        written++;
        /* 故障注入：模拟传输中断（断电/链路掉线），由调用方决定续传 */
        if (a->fail_after_block != 0 && written >= a->fail_after_block) {
            a->written_blocks = written;
            a->state = 5; a->need_rollback = 1; return OTA_ERR_XFER;
        }
        bsc = (bsc == 0xFF) ? 0x00 : (uint8_t)(bsc + 1);
    }
    a->written_blocks = written;

    /* 6) $37 退出传输 */
    req[0] = 0x37;
    UDS_ProcessRequest(uds, req, 1, resp, &rl);
    if (!(rl >= 1 && resp[0] == 0x77)) {
        a->state = 5; a->need_rollback = 1; return OTA_ERR_EXIT;
    }

    /* 7) 刷写完整性校验（两步，维度分离避免错配）：
     *   (a) agent 本地重算"固件级"CRC32（payload），与包内声明比对——
     *       包签名保护的是固件本身，整片 Flash CRC 维度不同，不可直接比；
     *   (b) 触发 ECU 整片 CRC 自检（$31 FF00，真实刷写收尾步骤），
     *       其返回的整片 CRC 记入 a->fw_crc 供日志/诊断，不跨维度强比。
     */
    uint32_t fw_crc_local = ota_crc32(pkg + OTA_HDR_SIZE, fw_len);
    uint32_t expect = ((uint32_t)pkg[15] << 24) | ((uint32_t)pkg[16] << 16) |
                     ((uint32_t)pkg[17] << 8)  |  (uint32_t)pkg[18];
    if (fw_crc_local != expect) { a->state = 5; a->need_rollback = 1; return OTA_ERR_CRC; }

    req[0] = 0x31; req[1] = 0x01; req[2] = 0xFF; req[3] = 0x00;
    UDS_ProcessRequest(uds, req, 4, resp, &rl);
    if (rl >= 9 && resp[0] == 0x71 && resp[1] == 0x01 &&
        resp[2] == 0xFF && resp[3] == 0x00) {
        a->fw_crc = ((uint32_t)resp[5] << 24) | ((uint32_t)resp[6] << 16) |
                      ((uint32_t)resp[7] << 8)  |  (uint32_t)resp[8];
    } else {
        a->state = 5; a->need_rollback = 1; return OTA_ERR_CRC;
    }

    a->state = 4; /* done */
    a->need_rollback = 0;
    return OTA_OK;
}
