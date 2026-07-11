/**
 * @file    uds_io.c
 * @brief   存储 / DID 读写抽象层的 Host（CANoe 仿真）实现。
 *
 * Flash 后端为一页静态 RAM 缓冲，模拟 STM32 内部 Flash 的"先擦后写"语义：
 *   - Erase：把区间置 0xFF
 *   - Write：仅在目标字节为 0xFF 时可写（否则视为写入未擦除区，返回错误）
 *   - Read ：直接回读
 * 该语义能让 $34/$36/$37 刷写流程的"未擦除即写入"错误分支被单元测试覆盖。
 */
#include "uds_io.h"
#include <string.h>

static uint8_t g_flash_buf[UDS_FLASH_SIZE];

uds_did_entry_t g_did_table[UDS_DID_MAX] = {
    /* VIN —— 只读 */
    { .did = 0xF190, .data = {'L','S','V','U','D','S','0','0','0','0','0','0','0','0','0','0'}, .len = 16, .writable = 0 },
    /* ECU 硬件版本 —— 只读 */
    { .did = 0xF195, .data = {'B','L','_','V','1','.','0','.','0'}, .len = 9, .writable = 0 },
    /* Bootloader 软件版本 —— 只读 */
    { .did = 0x0100, .data = {'2','0','2','6','.','0','7'}, .len = 7, .writable = 0 },
    /* 标定参数 1 —— 可读写（演示 $2E） */
    { .did = 0x0201, .data = {0x00,0x00,0x00,0x00}, .len = 4, .writable = 1 },
    /* 其余槽位预留 */
    { .did = 0x0000, .data = {0}, .len = 0, .writable = 0 },
    { .did = 0x0000, .data = {0}, .len = 0, .writable = 0 },
    { .did = 0x0000, .data = {0}, .len = 0, .writable = 0 },
    { .did = 0x0000, .data = {0}, .len = 0, .writable = 0 },
};

void UDS_FlashInit(uds_flash_t *f) {
    f->buf  = g_flash_buf;
    f->base = UDS_FLASH_BASE;
    f->size = UDS_FLASH_SIZE;
    f->erased = 0;
    memset(f->buf, 0xFF, f->size);
    f->erased = 1;
}

uint8_t UDS_FlashErase(uds_flash_t *f, uint32_t addr, uint32_t len) {
    if (addr < f->base) return 1;
    uint32_t off = addr - f->base;
    if (off + len > f->size) return 1;
    memset(f->buf + off, 0xFF, len);
    f->erased = 1;
    return 0;
}

uint8_t UDS_FlashWrite(uds_flash_t *f, uint32_t addr, const uint8_t *data, uint32_t len) {
    if (addr < f->base) return 1;
    uint32_t off = addr - f->base;
    if (off + len > f->size) return 1;
    /* 模拟 Flash 写入约束：目标字节必须为 0xFF（已擦除） */
    for (uint32_t i = 0; i < len; i++) {
        if (f->buf[off + i] != 0xFF) return 2; /* 未擦除区写入 */
    }
    memcpy(f->buf + off, data, len);
    return 0;
}

uint8_t UDS_FlashRead(uds_flash_t *f, uint32_t addr, uint8_t *data, uint32_t len) {
    if (addr < f->base) return 1;
    uint32_t off = addr - f->base;
    if (off + len > f->size) return 1;
    memcpy(data, f->buf + off, len);
    return 0;
}

uint8_t UDS_ReadDID(uint16_t did, uint8_t *out, uint16_t *len) {
    for (int i = 0; i < UDS_DID_MAX; i++) {
        if (g_did_table[i].did == did && g_did_table[i].len > 0) {
            memcpy(out, g_did_table[i].data, g_did_table[i].len);
            *len = g_did_table[i].len;
            return 0;
        }
    }
    return 1; /* DID 不存在 */
}

uint8_t UDS_WriteDID(uint16_t did, const uint8_t *data, uint16_t len) {
    for (int i = 0; i < UDS_DID_MAX; i++) {
        if (g_did_table[i].did == did) {
            if (!g_did_table[i].writable) return 2; /* 只读 DID */
            if (len > sizeof(g_did_table[i].data)) return 3;
            memcpy(g_did_table[i].data, data, len);
            g_did_table[i].len = len;
            return 0;
        }
    }
    return 1; /* DID 不存在 */
}
