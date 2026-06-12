#include "w25d64.h"
#include "spi.h"



static void CS_Low(void){
    HAL_GPIO_WritePin(FLASH_CS_GPIO_Port,FLASH_CS_Pin,GPIO_PIN_RESET);
}
static void CS_High(void){
    HAL_GPIO_WritePin(FLASH_CS_GPIO_Port,FLASH_CS_Pin,GPIO_PIN_SET);
}

static void W25_WaitBusy(void){
    uint8_t cmd = W25_CMD_READ_STATUS1;
    uint8_t status;
    do{
        CS_Low();
        HAL_SPI_Transmit(&hspi2, &cmd,1,100);
        HAL_SPI_Receive(&hspi2,&status,1,100);
        CS_High();
    }while(status & 0x01);
}

HAL_StatusTypeDef W25_Init(void){
    uint32_t id = W25_ReadID();
    if (id == 0xEF4017) {
        return HAL_OK;
    }
    return HAL_ERROR;
}

uint32_t W25_ReadID(void){
    uint8_t id[3];
    uint8_t cmd = W25_CMD_READ_JEDEC_ID;

    CS_Low();
    HAL_SPI_Transmit(&hspi2, &cmd,1,100);
    HAL_SPI_Receive(&hspi2,id,3,100);
    CS_High();

    return (id[0]<<16) | (id[1]<<8) | id[2];
}

void W25_Read(uint32_t addr, uint8_t *buf, uint32_t len){
    uint8_t cmd_1[4] = {
        W25_CMD_READ_DATA,
        (addr >> 16) & 0xFF,   // addr 高字节
        (addr >>  8) & 0xFF,   // addr 中字节
        (addr      ) & 0xFF    // addr 低字节
    };

    CS_Low();
    HAL_SPI_Transmit(&hspi2, cmd_1,4,100);
    HAL_SPI_Receive(&hspi2,buf,len,100);
    CS_High();
}

void W25_WritePage(uint32_t addr, const uint8_t *data, uint16_t len){
    if (len > 256) return;
    uint8_t cmd_0 = W25_CMD_WRITE_ENABLE;
    uint8_t cmd_1[4] = {
        W25_CMD_PAGE_PROGRAM,
        (addr >> 16) & 0xFF,   // addr 高字节
        (addr >>  8) & 0xFF,   // addr 中字节
        (addr      ) & 0xFF    // addr 低字节
    };

    W25_WaitBusy();
    CS_Low();
    HAL_SPI_Transmit(&hspi2, &cmd_0,1,100);
    CS_High();
    CS_Low();
    HAL_SPI_Transmit(&hspi2, cmd_1,4,100);
    HAL_SPI_Transmit(&hspi2,data,len,100);
    CS_High();
    W25_WaitBusy();
}

void W25_EraseSector(uint32_t addr){
    uint8_t cmd_0 = W25_CMD_WRITE_ENABLE;
    addr &= ~(W25_SECTOR_SIZE - 1);     // 地址对齐，把低 12 位清零，保留高位
    uint8_t cmd_1[4] = {
        W25_CMD_SECTOR_ERASE,
        (addr >> 16) & 0xFF,   // addr 高字节
        (addr >>  8) & 0xFF,   // addr 中字节
        (addr      ) & 0xFF    // addr 低字节
    };

    W25_WaitBusy();
    CS_Low();
    HAL_SPI_Transmit(&hspi2, &cmd_0,1,100);
    CS_High();
    CS_Low();
    HAL_SPI_Transmit(&hspi2, cmd_1,4,100);
    CS_High();
    W25_WaitBusy();
}