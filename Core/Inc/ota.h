#ifndef OTA_H
#define OTA_H
#include "main.h"
// SPI Flash 分区
#define OTA_FLAG_ADDR       0x00000000
#define OTA_FW_ADDR         0x00001000
#define OTA_FW_MAX_SIZE     (56 * 1024)

// OTA 标志
#define OTA_MAGIC           0x4F544131
#define OTA_STATE_IDLE      0x00
#define OTA_STATE_PENDING   0x01

typedef struct {
    uint32_t magic;
    uint32_t fw_size;
    uint32_t fw_crc32;
    uint8_t  state;
    uint8_t  reserved[3];
} OTA_Flag_t;

// 协议命令
#define PKT_HEADER    0xAA
#define CMD_OTA_START 0x01
#define CMD_OTA_DATA  0x02
#define CMD_OTA_END   0x03
#define CMD_QUERY     0x10

// CRC 函数声明
uint16_t crc16_compute(const uint8_t *data, uint16_t len);
uint32_t crc32_compute(const uint8_t *data, uint32_t len);
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

#endif 
