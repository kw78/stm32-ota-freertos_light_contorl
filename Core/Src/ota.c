#include "ota.h"
#include "w25d64.h"
#ifdef USE_FREERTOS
#include "cmsis_os.h"
#endif
#include <string.h>
#include <stdio.h>

#define RING_BUF_SIZE 512
#define PKT_DATA_MAX  64        // 协议单包数据上限（与 pkt_buf/verify_buf 大小一致）
#define OTA_PKT_TIMEOUT_MS 500  // 包内字节间最大间隔，超时丢弃残包重新同步

extern UART_HandleTypeDef huart1;

static uint8_t ack = 0x06;        

// 循环缓冲区
typedef struct {
    uint8_t  buf[RING_BUF_SIZE];
    volatile uint16_t head;     // 加入volatile防止编译器在多文件的时候优化
    volatile uint16_t tail;
} RingBuf_t;

// 数据指令包
typedef enum {
    PKT_WAIT_HEADER,
    PKT_WAIT_CMD,
    PKT_WAIT_LEN,
    PKT_WAIT_DATA,
    PKT_WAIT_CRC
} PktState_t;

// CRC16（包校验用）
uint16_t crc16_compute(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

// CRC32（固件校验用），参考开源实现
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return crc;
}

// 一次性计算（整块数据）
uint32_t crc32_compute(const uint8_t *data, uint32_t len)
{
    return ~crc32_update(0xFFFFFFFF, data, len);
}

// 循环缓冲区
static RingBuf_t ring_buf;

// ISR 里调用：写入一个字节
void OTA_RingBuf_Put(uint8_t byte)
{
    uint16_t next = (ring_buf.head + 1) % RING_BUF_SIZE;
    if (next != ring_buf.tail) {
        ring_buf.buf[ring_buf.head] = byte;
        ring_buf.head = next;
    }
}

// 任务里调用：读取一个字节
static int RingBuf_Get(uint8_t *byte)       // 无需外部访问
{
    if (ring_buf.head == ring_buf.tail) return 0;       // 初始状态
    *byte = ring_buf.buf[ring_buf.tail];        // 取出字段tail写入byte
    ring_buf.tail = (ring_buf.tail + 1) % RING_BUF_SIZE;        
    return 1;
}

// 循环缓冲状态机简化
static PktState_t pkt_state = PKT_WAIT_HEADER;
static uint8_t pkt_cmd, pkt_len, pkt_idx;
static uint8_t pkt_buf[PKT_DATA_MAX];     // 最多64字节一次
static uint32_t pkt_start_tick = 0;       // 当前包第一个字节的时刻（超时重同步用）

static void Pkt_Reset(void)
{
    pkt_state = PKT_WAIT_HEADER;
    pkt_idx = 0;
}

// 每收到一个字节调用一次，返回 1 表示收到完整包
static int Pkt_ParseByte(uint8_t byte)
{
    // 半途而废的包（丢字节）不能永远等下去：超时后丢弃，从当前字节重新找帧头
    if (pkt_state != PKT_WAIT_HEADER &&
        HAL_GetTick() - pkt_start_tick > OTA_PKT_TIMEOUT_MS) {
        Pkt_Reset();
    }

    switch (pkt_state) {
    case PKT_WAIT_HEADER:
        if (byte == 0xAA) {
            pkt_start_tick = HAL_GetTick();
            pkt_state = PKT_WAIT_CMD;
        }
        break;
    case PKT_WAIT_CMD:
        pkt_cmd = byte;
        pkt_state = PKT_WAIT_LEN;
        break;
    case PKT_WAIT_LEN:
        // 长度超过缓冲区容量的非法包直接丢弃，否则后续 memcpy 会越界写栈
        if (byte > PKT_DATA_MAX) {
            Pkt_Reset();
            break;
        }
        pkt_len = byte;
        pkt_idx = 0;
        pkt_state = (pkt_len > 0) ? PKT_WAIT_DATA : PKT_WAIT_CRC;
        break;
    case PKT_WAIT_DATA:
        if (pkt_idx < sizeof(pkt_buf)) pkt_buf[pkt_idx] = byte;
        pkt_idx++;
        if (pkt_idx >= pkt_len) pkt_state = PKT_WAIT_CRC;
        break;
    case PKT_WAIT_CRC:
        // 收到 2 字节 CRC16（高字节在前）
        static uint8_t crc_hi, crc_byte_idx;
        if (crc_byte_idx == 0) {
            crc_hi = byte;            // 第 1 字节：CRC 高字节
            crc_byte_idx = 1;
        } else {
           uint16_t received_crc = (crc_hi << 8) | byte;  // 第 2 字节：CRC 低字节
            crc_byte_idx = 0;

           // 计算包内容的 CRC16
            uint8_t tmp[2 + PKT_DATA_MAX];
            tmp[0] = pkt_cmd;
            tmp[1] = pkt_len;
            memcpy(tmp + 2, pkt_buf, pkt_len);
            uint16_t calc_crc = crc16_compute(tmp, 2 + pkt_len);

            // 无论校验是否通过都必须回到帧头等待状态，
            // 否则状态机卡死在 PKT_WAIT_CRC，后续所有包都被当作 CRC 字节吞掉
            pkt_state = PKT_WAIT_HEADER;
            if (calc_crc == received_crc) {
                return 1;   // CRC 正确，完整包
            }
           // CRC 错误，丢弃
        }
        break;
    }
    return 0;
}

// 处理一个完整包
static uint32_t ota_fw_size = 0;        // 固件总大小（START 时记录）
static uint32_t ota_fw_crc32 = 0;       // 固件 CRC32（START 时记录）
static uint32_t ota_bytes_written = 0;  // 已写入字节数（DATA 时递增）
static uint8_t  ota_active = 0;         // 会话标志：START 置位，END/出错复位

static void OTA_SendByte(uint8_t b)
{
    HAL_UART_Transmit(&huart1, &b, 1, 100);
}

static void OTA_HandlePacket(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    switch (cmd) {
    case CMD_OTA_START:
        if(len < 8) break;
        memcpy(&ota_fw_size, data, 4);
        memcpy(&ota_fw_crc32, data + 4, 4);

        // 大小必须合法：异常值会让下面的擦除循环越界擦掉其他分区
        if (ota_fw_size == 0 || ota_fw_size > OTA_FW_MAX_SIZE) {
            ota_fw_size = 0;
            OTA_SendByte(0x15);     // NACK，让上位机立刻失败而不是等超时
            break;
        }
        ota_bytes_written = 0;
        ota_active = 1;

        // 预备 SPI Flash 空间：先清 OTA 标志区（丢弃上次残留的 PENDING），
        // 再擦除固件暂存区实际用到的扇区
        W25_EraseSector(OTA_FLAG_ADDR);
        for (uint32_t addr = OTA_FW_ADDR;
            addr < OTA_FW_ADDR + ota_fw_size;
            addr += W25_SECTOR_SIZE){
                W25_EraseSector(addr);
            }

        // 回复ACK
        OTA_SendByte(ack);
        break;
    case CMD_OTA_DATA:
        // 没有会话、空包或超出容量都要 NACK，静默丢弃会让上位机等到超时
        if (!ota_active || len == 0) {
            OTA_SendByte(0x15);
            break;
        }
        if(ota_bytes_written + len > OTA_FW_MAX_SIZE) {
            OTA_SendByte(0x15);
            break;
        }

        W25_WritePage(OTA_FW_ADDR + ota_bytes_written, data, len);

        // 回读验证
        uint8_t verify_buf[PKT_DATA_MAX];
        W25_Read(OTA_FW_ADDR + ota_bytes_written, verify_buf, len);
        if (memcmp(data, verify_buf, len) != 0) {
            // 打印前 8 字节对比，给出错误
            char dbg[40];
            int n = snprintf(dbg, sizeof(dbg),
                "W ERR@%lu: w=%02X%02X%02X r=%02X%02X%02X\r\n",
                (unsigned long)ota_bytes_written,
                data[0], data[1], data[2],
                verify_buf[0], verify_buf[1], verify_buf[2]);
            HAL_UART_Transmit(&huart1, (uint8_t *)dbg, (uint16_t)n, 100);
            ota_active = 0;        // 数据已错位，会话作废，必须重新 START
            OTA_SendByte(0x15);
            break;
        }

        ota_bytes_written += len;

        // 回复ACK
        OTA_SendByte(ack);
        break;
    case CMD_OTA_END:
        do {
            // 传输不完整（缺包/中途出错）不能进入校验流程
            if (!ota_active || ota_bytes_written != ota_fw_size) {
                OTA_SendByte(0x15);
                break;
            }
            ota_active = 0;

            // 回读固件，分段计算 CRC32
            uint8_t read_buf[256];
            uint32_t pos = 0;
            uint32_t calc_crc = 0xFFFFFFFF;
            while (pos < ota_fw_size) {
                uint32_t chunk = (ota_fw_size - pos > 256) ? 256 : (ota_fw_size - pos);
                W25_Read(OTA_FW_ADDR + pos, read_buf, chunk);
                calc_crc = crc32_update(calc_crc, read_buf, chunk);  // 分段累加
                pos += chunk;
            }
            calc_crc = ~calc_crc;   // 最终取反
            if (calc_crc == ota_fw_crc32) {
                OTA_Flag_t flag = {
                .magic    = OTA_MAGIC,
                .fw_size  = ota_fw_size,
                .fw_crc32 = ota_fw_crc32,
                .state    = OTA_STATE_PENDING,
                };

                W25_EraseSector(OTA_FLAG_ADDR);
                W25_WritePage(OTA_FLAG_ADDR, (uint8_t *)&flag, sizeof(flag));
                OTA_SendByte(ack);
                #ifdef USE_FREERTOS
                osDelay(100);
                #endif
                NVIC_SystemReset();
            } else {
                OTA_SendByte(0x15);
            }
        } while (0);
    break;
    }
}


// 从环形缓冲区读取并解析
void OTA_Process(void)
{
    uint8_t byte;
    while (RingBuf_Get(&byte)) {
        if (Pkt_ParseByte(byte)) {
            OTA_HandlePacket(pkt_cmd, pkt_buf, pkt_len);
        }
    }
}