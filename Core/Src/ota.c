#include "ota.h"
#include "w25d64.h"
#include "sha256.h"
#include "ota_key.h"
#include "blackbox.h"
#include "light_ctrl.h"
#include "supervisor.h"
#ifdef USE_FREERTOS
#include "cmsis_os.h"
#endif
#include <string.h>
#include <stdio.h>

#define RING_BUF_SIZE 512
#define PKT_DATA_MAX  64        // 协议单包数据上限（与 pkt_buf/verify_buf 大小一致）
#define OTA_PKT_TIMEOUT_MS 500  // 包内字节间最大间隔，超时丢弃残包重新同步

extern UART_HandleTypeDef huart1;

static uint8_t ack = 0x06;        // ACK
static uint8_t nack = 0x15;       // NACK

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
    PKT_WAIT_SEQ,       // v2：2 字节序号（低在前）
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
static uint16_t pkt_seq = 0;                     // v2 包序号
static uint8_t pkt_buf[PKT_DATA_MAX];     // 最多64字节一次
static uint32_t pkt_start_tick = 0;       // 当前包第一个字节的时刻（超时重同步用）
static uint8_t crc_hi, crc_byte_idx;      // CRC 字段接收进度（fuzz 台 O4 发现：
                                          // 原为 case 块内 static，Pkt_Reset 够不着，
                                          // 半包 CRC 超时后残留进度会错杀下一帧合法包）

static void Pkt_Reset(void)
{
    pkt_state = PKT_WAIT_HEADER;
    pkt_idx = 0;
    crc_byte_idx = 0;
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
        pkt_state = PKT_WAIT_SEQ;   // v2：所有命令帧 LEN 后跟 2 字节 SEQ
        break;
    case PKT_WAIT_SEQ:
        // v2：DATA 帧在 LEN 之后有 2 字节小端序号
        if (pkt_idx == 0) {
            pkt_seq = byte;
            pkt_idx = 1;
        } else {
            pkt_seq |= (uint16_t)byte << 8;
            pkt_idx = 0;
            pkt_state = (pkt_len > 0) ? PKT_WAIT_DATA : PKT_WAIT_CRC;
        }
        break;
    case PKT_WAIT_DATA:
        if (pkt_idx < sizeof(pkt_buf)) pkt_buf[pkt_idx] = byte;
        pkt_idx++;
        if (pkt_idx >= pkt_len) pkt_state = PKT_WAIT_CRC;
        break;
    case PKT_WAIT_CRC:
        // 收到 2 字节 CRC16（高字节在前）
        if (crc_byte_idx == 0) {
            crc_hi = byte;            // 第 1 字节：CRC 高字节
            crc_byte_idx = 1;
        } else {
            uint16_t received_crc = (crc_hi << 8) | byte;  // 第 2 字节：CRC 低字节
            crc_byte_idx = 0;

            // 计算包内容的 CRC16（v2 覆盖 cmd+len+seq+data）
            uint8_t tmp[4 + PKT_DATA_MAX];
            tmp[0] = pkt_cmd;
            tmp[1] = pkt_len;
            tmp[2] = (uint8_t)(pkt_seq & 0xFF);
            tmp[3] = (uint8_t)(pkt_seq >> 8);
            memcpy(tmp + 4, pkt_buf, pkt_len);
            uint16_t calc_crc = crc16_compute(tmp, 4 + pkt_len);

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

// 会话状态
static uint32_t ota_fw_size = 0;        // 固件总大小（START 时记录）
static uint32_t ota_fw_crc32 = 0;       // 固件 CRC32（START 时记录）
static uint32_t ota_fw_version = 0;     // 固件版本（START 时记录）
static uint32_t ota_fw_build_ts = 0;    // v3：单调构建时间戳（防降级计数）
static uint8_t  ota_fw_hmac[16] = {0};  // v3：镜像 HMAC-SHA256-128 签名
static uint32_t ota_bytes_written = 0;  // 已写入字节数（DATA 时递增）

static void OTA_SendByte(uint8_t b)
{
    HAL_UART_Transmit(&huart1, &b, 1, 100);
}

static void OTA_SendBuf(const uint8_t *buf, uint16_t len)
{
    HAL_UART_Transmit(&huart1, buf, len, 100);
}

static void OTA_SendPacket(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    /* 满帧 = 1 头 + 1 cmd + 1 len + 2 seq + 64 data + 2 crc = 71B。
     * 原来写成 4+64+2=70，len=64 时越界写 1 字节（fuzz 台 D6 发现，
     * 现有响应均 ≤32B 未触发，属 Bug #11 同族的潜伏缺陷） */
    uint8_t frame[7 + PKT_DATA_MAX];
    uint8_t n = 0;
    frame[n++] = PKT_HEADER;
    frame[n++] = cmd;
    frame[n++] = len;
    frame[n++] = 0;              // SEQ 低（响应包不用）
    frame[n++] = 0;              // SEQ 高
    memcpy(frame + n, data, len);
    n += len;
    uint16_t crc = crc16_compute(frame + 1, 4u + len);   // CRC 覆盖 cmd+len+seq(2)+data
    frame[n++] = (uint8_t)(crc >> 8);
    frame[n++] = (uint8_t)(crc & 0xFF);
    OTA_SendBuf(frame, n);
}

// 处理一个完整包
static void OTA_HandlePacket(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    switch (cmd) {
    case CMD_OTA_START:
        // v3：START 载荷扩为 32B（+build_ts+hmac），旧格式 12B 一律拒绝——
        // 未签名镜像从会话源头挡掉（END 还有二次 HMAC 校验，双保险）
        if (len < 32) { OTA_SendByte(nack); break; }
        if (g_ota_backup_busy) { OTA_SendByte(nack); break; }   // 金固件备份中

        memcpy(&ota_fw_size, data, 4);
        memcpy(&ota_fw_crc32, data + 4, 4);
        memcpy(&ota_fw_version, data + 8, 4);
        memcpy(&ota_fw_build_ts, data + 12, 4);
        memcpy(ota_fw_hmac, data + 16, 16);

        // 大小必须合法：异常值会让擦除循环越界擦掉其他分区
        if (ota_fw_size == 0 || ota_fw_size > OTA_FW_MAX_SIZE) {
            ota_fw_size = 0;
            OTA_SendByte(nack);
            break;
        }
        // 防降级早拒：比已确认地板旧的镜像直接 NACK（省一整轮上传）
        if (ota_fw_build_ts < g_ota_flag_cache.min_build) {
            ota_fw_size = 0;
            OTA_SendByte(nack);
            break;
        }
        ota_bytes_written = 0;
        g_ota_session_active = 1;

        // 只擦槽 A 数据区实际用到的扇区。控制块/镜像头留给 END 一次性提交，
        // 会话中途断电不会破坏现有升级状态（上次 TESTING/金固件信息完好）
        for (uint32_t addr = OTA_FW_ADDR;
            addr < OTA_FW_ADDR + ota_fw_size;
            addr += W25_SECTOR_SIZE){
                W25_EraseSector(addr);
            }

        OTA_SendByte(ack);
        break;

    case CMD_OTA_DATA: {
        // v2 核心：按 SEQ 计算期望偏移，重传幂等
        if (!g_ota_session_active || len == 0) {
            OTA_SendByte(nack);
            break;
        }
        uint32_t expected_seq = ota_bytes_written / OTA_CHUNK_SIZE;

        if (pkt_seq < expected_seq) {
            // 已写过的包（上位机没收到 ACK 时的重传）：直接 ACK，不重复写
            OTA_SendByte(ack);
            break;
        }
        if (pkt_seq > expected_seq) {
            // 乱序/跳包：数据流已断，要求上位机从中断处重发
            OTA_SendByte(nack);
            break;
        }

        uint32_t offset = (uint32_t)pkt_seq * OTA_CHUNK_SIZE;
        if (offset + len > ota_fw_size) {   // 超出申报大小（含尾包超长）
            OTA_SendByte(nack);
            break;
        }

        W25_WritePage(OTA_FW_ADDR + offset, data, len);

        // 回读验证
        uint8_t verify_buf[PKT_DATA_MAX];
        W25_Read(OTA_FW_ADDR + offset, verify_buf, len);
        if (memcmp(data, verify_buf, len) != 0) {
            char dbg[40];
            int n = snprintf(dbg, sizeof(dbg),
                "W ERR@%lu: w=%02X%02X%02X r=%02X%02X%02X\r\n",
                (unsigned long)offset,
                data[0], data[1], data[2],
                verify_buf[0], verify_buf[1], verify_buf[2]);
            HAL_UART_Transmit(&huart1, (uint8_t *)dbg, (uint16_t)n, 100);
            g_ota_session_active = 0;    // 数据已错位，会话作废，必须重新 START
            OTA_SendByte(nack);
            break;
        }

        ota_bytes_written = offset + len;
        OTA_SendByte(ack);
        break;
    }

    case CMD_OTA_END: {
        if (g_ota_backup_busy) { OTA_SendByte(nack); break; }
        // 传输不完整（缺包/中途出错）不能进入校验流程
        if (!g_ota_session_active || ota_bytes_written != ota_fw_size) {
            g_ota_session_active = 0;
            OTA_SendByte(nack);
            break;
        }
        g_ota_session_active = 0;

        // 回读固件，CRC32 与 HMAC 签名一遍流式同过（省一整轮 43KB 读）
        OTA_ImageHdr_t hdr = {
            .magic    = OTA_IMG_MAGIC,
            .version  = ota_fw_version,
            .size     = ota_fw_size,
            .crc32    = ota_fw_crc32,
            .build_ts = ota_fw_build_ts,
        };
        uint8_t key[OTA_HMAC_KEY_LEN];
        uint8_t key_ok = ota_hmac_key_from_hex(OTA_HMAC_KEY_HEX, key, sizeof(key));
        HmacSha256 hm;
        if (key_ok) {
            hmac_sha256_begin(&hm, key, sizeof(key));
            sha256_update(&hm.inner, (const uint8_t *)&hdr, 20);  // 头前 20B 入签名
        }
        uint8_t read_buf[256];
        uint32_t pos = 0;
        uint32_t calc_crc = 0xFFFFFFFF;
        while (pos < ota_fw_size) {
            uint32_t chunk = (ota_fw_size - pos > 256) ? 256 : (ota_fw_size - pos);
            W25_Read(OTA_FW_ADDR + pos, read_buf, chunk);
            calc_crc = crc32_update(calc_crc, read_buf, chunk);  // 分段累加
            if (key_ok) sha256_update(&hm.inner, read_buf, chunk);
            pos += chunk;
        }
        calc_crc = ~calc_crc;   // 最终取反

        if (calc_crc != ota_fw_crc32 || !key_ok) {
            OTA_SendByte(nack);
            break;
        }
        uint8_t mac[32];
        hmac_sha256_end(&hm, mac);
        if (memcmp(mac, ota_fw_hmac, 16) != 0) {
            // 签名不符：拒绝安装（防未签名/篡改镜像，与 Bootloader 双保险）
            OTA_SendByte(nack);
            break;
        }
        memcpy(hdr.hmac, ota_fw_hmac, 16);

        // 校验通过：写槽 A 镜像头 + 控制块置 PENDING（保留 golden_valid/min_build）
        W25_EraseSector(OTA_HDR_A_ADDR);
        W25_WritePage(OTA_HDR_A_ADDR, (const uint8_t *)&hdr, sizeof(hdr));

        OTA_Flag_t flag;
        W25_Read(OTA_FLAG_ADDR, (uint8_t *)&flag, sizeof(flag));
        if (flag.magic != OTA_FLAG_MAGIC || flag.ver != OTA_FLAG_VER) {
            memset(&flag, 0, sizeof(flag));
            flag.magic = OTA_FLAG_MAGIC;
            flag.ver   = OTA_FLAG_VER;
        }
        flag.state     = OTA_STATE_PENDING;
        flag.retry_cnt = 0;
        W25_EraseSector(OTA_FLAG_ADDR);
        W25_WritePage(OTA_FLAG_ADDR, (const uint8_t *)&flag, sizeof(flag));

        OTA_SendByte(ack);
        #ifdef USE_FREERTOS
        osDelay(100);
        #endif
        Supervisor_IWDGFeed();   // 复位前喂满狗：IWDG 跨复位保留，给 Bootloader 搬运留足窗口
        NVIC_SystemReset();     // 进入 Bootloader 搬运
    } break;

    case CMD_QUERY: {
        OTA_QueryResp_t resp;
        resp.proto      = 2;
        resp.state      = g_ota_flag_cache.state;
        resp.golden     = g_ota_flag_cache.golden_valid;
        resp.retry      = (uint8_t)g_ota_flag_cache.retry_cnt;
        resp.version    = APP_VERSION;
        resp.uptime_sec = HAL_GetTick() / 1000u;
        OTA_SendPacket(CMD_QUERY, (const uint8_t *)&resp, sizeof(resp));
    } break;

    case CMD_GET_LOG: {
        /* 请求: which(1B, 1=黑匣子 2=统计) + idx(2B LE)；响应: 32B 记录 / 空=该槽无效 */
        if (g_ota_backup_busy) { OTA_SendByte(nack); break; }
        if (len < 3) { OTA_SendByte(nack); break; }
        uint8_t  which = data[0];
        uint16_t idx   = (uint16_t)(data[1] | ((uint16_t)data[2] << 8));
        LogRec_t rec;
        if (which != LOG_BLACKBOX && which != LOG_STATS) {
            OTA_SendByte(nack);
            break;
        }
        if (Log_Read(which, idx, &rec))
            OTA_SendPacket(CMD_GET_LOG, (const uint8_t *)&rec, sizeof(rec));
        else
            OTA_SendPacket(CMD_GET_LOG, (const uint8_t *)"", 0);
    } break;

    case CMD_LIGHT_CTRL: {
        /* DATA: enable(1B) + target_adc(2B LE, 0=默认 IDEAL 频带) */
        if (len < 3) { OTA_SendByte(nack); break; }
        uint8_t  en  = data[0];
        uint16_t tgt = (uint16_t)(data[1] | ((uint16_t)data[2] << 8));
        if (en > 1) { OTA_SendByte(nack); break; }
        LightCtrl_SetEnable(en, tgt);
        char msg[48];
        int n = snprintf(msg, sizeof(msg), "DIM %s\r\n", en ? "ON" : "OFF");
        HAL_UART_Transmit(&huart1, (uint8_t *)msg, (uint16_t)n, 100);
        OTA_SendByte(ack);
    } break;
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
