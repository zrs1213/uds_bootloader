/**
 * @file    uds_io.h
 * @brief   存储 / DID 读写抽象层。
 *
 * 诊断栈不直接操作 Flash 寄存器，而是通过本层 API 读写。在 CANoe / Host 仿真
 * 环境下由 RAM 缓冲（uds_io.c 默认实现）模拟 Flash；移植到 STM32 时仅需把
 * 后端换成 HAL_FLASH_Erase / HAL_FLASH_Program，协议层代码不变。
 *
 * 同时提供 DID（数据标识符）读写表，支撑 $22 ReadDataByIdentifier 与
 * $2E WriteDataByIdentifier。
 */
#ifndef UDS_IO_H
#define UDS_IO_H

#include <stdint.h>

/* ---------------- Flash 抽象 ---------------- */
#define UDS_FLASH_SIZE  (64 * 1024)   /* 仿真 Flash 容量（Host：RAM 缓冲） */
#define UDS_FLASH_BASE  0x08010000u   /* 仿真 APP 区起始地址（对标 STM32 内部 Flash） */

typedef struct {
    uint8_t *buf;        /* 后端存储（Host=RAM；Target=映射后的 Flash 镜像） */
    uint32_t base;       /* 逻辑起始地址 */
    uint32_t size;       /* 容量 */
    uint8_t  erased;     /* 是否处于已擦除态（全 0xFF） */
} uds_flash_t;

void    UDS_FlashInit(uds_flash_t *f);
uint8_t UDS_FlashErase(uds_flash_t *f, uint32_t addr, uint32_t len);
uint8_t UDS_FlashWrite(uds_flash_t *f, uint32_t addr, const uint8_t *data, uint32_t len);
uint8_t UDS_FlashRead (uds_flash_t *f, uint32_t addr, uint8_t *data, uint32_t len);

/* ---------------- DID 表 ---------------- */
#define UDS_DID_MAX 8
typedef struct {
    uint16_t did;
    uint8_t  data[16];
    uint16_t len;
    uint8_t  writable;   /* $2E 是否允许写 */
} uds_did_entry_t;

extern uds_did_entry_t g_did_table[UDS_DID_MAX];

/* 返回 0=成功；非 0=未找到 DID */
uint8_t UDS_ReadDID (uint16_t did, uint8_t *out, uint16_t *len);
uint8_t UDS_WriteDID(uint16_t did, const uint8_t *data, uint16_t len);

#endif /* UDS_IO_H */
