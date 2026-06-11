#ifndef W25D64_H

#define W25D64_H

#include"main.h"

// 指令操作码
#define W25_CMD_READ_JEDEC_ID   0x9F
#define W25_CMD_WRITE_ENABLE    0x06
#define W25_CMD_READ_STATUS1    0x05
#define W25_CMD_READ_DATA       0x03
#define W25_CMD_PAGE_PROGRAM    0x02
#define W25_CMD_SECTOR_ERASE    0x20

// 芯片参数
#define W25_PAGE_SIZE    256
#define W25_SECTOR_SIZE  4096

// 对外接口函数
void     W25_Init(void);
uint32_t W25_ReadID(void);
void     W25_Read(uint32_t addr, uint8_t *buf, uint32_t len);
void     W25_WritePage(uint32_t addr, const uint8_t *data, uint16_t len);
void     W25_EraseSector(uint32_t addr);


#endif 