#include "supervisor.h"
#include "blackbox.h"
#include "w25d64.h"
#include "cmsis_os.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

/* ---- 共享运行时状态（ota.c 引用；bootloader 不链接本文件） ---- */
volatile uint8_t g_ota_backup_busy = 0;
volatile uint8_t g_ota_session_active = 0;
OTA_Flag_t g_ota_flag_cache;

extern UART_HandleTypeDef huart1;
extern volatile uint32_t seconds_today;     /* main.c：TIM3 1Hz 统计计数，健康探针 */
extern volatile uint32_t today_dark_sec;    /* main.c：光照统计（黑匣子快照用） */
extern volatile uint32_t today_dim_sec;
extern volatile uint32_t today_ideal_sec;
extern volatile uint32_t today_glare_sec;
extern volatile uint32_t oled_heartbeat;    /* oled.c：Task_OLED 活性探针 */

/* ---- IWDG：直接寄存器操作（工程未启用 HAL IWDG 驱动，寄存器本身就几个） ----
 * LSI 30~60kHz（典型 40kHz），预分频 /256：
 *   早期长超时 RLR=4095 → 17.5~35s：覆盖上电初始化 + OTA 复位后 Bootloader 搬运窗口
 *   运行期收紧 RLR=1250 → 5.3~10.7s（典型 8s），健康时每 2s 喂一次
 * IWDG 一旦启动只能靠复位停止——这是特性不是缺陷：跑挂的固件无处可逃
 */
#define IWDG_KEY_ENABLE   0xCCCCu
#define IWDG_KEY_FEED     0xAAAAu
#define IWDG_KEY_UNLOCK   0x5555u
#define IWDG_PRESC_256    6u
#define IWDG_RLR_LONG     4095u
#define IWDG_RLR_RUN      1250u

static void iwdg_reload_config(uint16_t rlr)
{
    IWDG->KR  = IWDG_KEY_UNLOCK;
    IWDG->PR  = IWDG_PRESC_256;
    IWDG->RLR = rlr;
    IWDG->KR  = IWDG_KEY_FEED;
}

void Supervisor_IWDGEarlyArm(void)
{
    iwdg_reload_config(IWDG_RLR_LONG);
    IWDG->KR = IWDG_KEY_ENABLE;
}

void Supervisor_IWDGFeed(void)
{
    IWDG->KR = IWDG_KEY_FEED;
}

/* ---- PVD 欠压探测（EXTI16）：good2 调查 + 电源健康监控 ----
 * F1 没有欠压复位标志（无 BOR），只能中断捕捉：VDD 跌破 2.9V 时 PVDO
 * 置位触发 EXTI16。ISR 只写 .noinit RAM——欠压瞬间 SPI/Flash 操作不可靠。
 * dip 后电源恢复（未走到复位）：supervisor 运行期轮询补记 REC_PVD；
 * dip 导致复位：SRAM 内容在 VDD≥~1.5V 期间保持，下次开机转录。
 * "黑匣子扫描中途设备复位"（HANDOFF good2 调查）的头号嫌疑就是供电毛刺，
 * 这条记录用于证实/证伪。 */
#define PVD_DIP_MAGIC   0x50564431u   /* 'PVD1' */

typedef struct {
    uint32_t magic;
    uint32_t count;                     /* 捕捉到的欠压次数（转录成功后清零） */
    uint32_t uptime;                    /* 最近一次欠压时的开机毫秒 */
} PvdMailbox_t;

__attribute__((section(".noinit"))) static PvdMailbox_t g_pvd_mailbox;

void PVD_IRQHandler(void)
{
    if (EXTI->PR & (1u << 16)) {
        EXTI->PR = (1u << 16);          /* 写 1 清挂起 */
        if (g_pvd_mailbox.magic != PVD_DIP_MAGIC) {
            g_pvd_mailbox.magic  = PVD_DIP_MAGIC;
            g_pvd_mailbox.count  = 0;
            g_pvd_mailbox.uptime = 0;
        }
        g_pvd_mailbox.count++;
        g_pvd_mailbox.uptime = HAL_GetTick();
    }
}

void Supervisor_PVDInit(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR = (PWR->CR & ~PWR_CR_PLS) | PWR_CR_PLS_LEV6 | PWR_CR_PVDE;  /* 阈值 2.9V */
    EXTI->IMR  |=  (1u << 16);          /* EXTI16 = PVD 输出 */
    EXTI->RTSR |=  (1u << 16);          /* 上升沿：VDD 跌入阈值以下 */
    EXTI->FTSR &= ~(1u << 16);
    EXTI->PR = (1u << 16);
    HAL_NVIC_SetPriority(PVD_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(PVD_IRQn);
}

/* ---- 参数 ---- */
#define SUPERVISOR_PERIOD_MS   2000
#define CONFIRM_DELAY_MS       10000
#define SUPERVISOR_STACK_SIZE  (512 * 4)
#define STATS_INTERVAL_MS      (30u * 60u * 1000u)   /* 统计快照周期 */
#define OLED_STALE_MS          10000                 /* OLED 心跳失效阈值 */
#define OLED_GRACE_MS          15000                 /* 开机宽限（首次刷屏前不判死） */

static void log_line(const char *s)
{
    HAL_UART_Transmit(&huart1, (const uint8_t *)s, (uint16_t)strlen(s), 100);
}

/* 健康判定：
 *   a) HAL tick 在走 → TIM4（HAL 时基）中断活着
 *   b) seconds_today 在涨 → TIM3 统计中断 + 主循环状态机路径活着
 *   c) oled_heartbeat 在涨 → Task_OLED 及其 I2C/OLED 链路活着（开机宽限期后）
 *   （本任务能被调度本身就证明 FreeRTOS 内核活着）
 * 任何一项不动 → 返回 0，本轮不喂狗
 */
static int health_check(void)
{
    static uint32_t last_tick = 0;
    static uint32_t last_sec = 0;
    static uint32_t last_oled_hb = 0;
    uint32_t now = HAL_GetTick();
    uint32_t sec = seconds_today;
    uint32_t hb = oled_heartbeat;
    int ok = 1;

    if (now - last_tick < SUPERVISOR_PERIOD_MS / 2) ok = 0;   /* tick 停了 */
    if (sec == last_sec) ok = 0;                              /* 秒计数停了 */
    if (now > OLED_GRACE_MS && hb == last_oled_hb) ok = 0;    /* OLED 卡死 */

    last_tick = now;
    last_sec = sec;
    last_oled_hb = hb;
    return ok;
}

/* 把当前控制块写回 SPI Flash 并同步缓存 */
static void flag_commit(OTA_Flag_t *f)
{
    W25_EraseSector(OTA_FLAG_ADDR);
    W25_WritePage(OTA_FLAG_ADDR, (const uint8_t *)f, sizeof(*f));
    memcpy(&g_ota_flag_cache, f, sizeof(*f));
}

/* 启动时加载控制块：v2 直接用；v1 旧标志或杂数据 → 一次性迁移为干净的 v2 IDLE */
static void flag_boot_init(void)
{
    OTA_Flag_t f;
    W25_Read(OTA_FLAG_ADDR, (uint8_t *)&f, sizeof(f));

    if (f.magic == OTA_FLAG_MAGIC && f.ver == OTA_FLAG_VER) {
        memcpy(&g_ota_flag_cache, &f, sizeof(f));
        return;
    }

    /* v1 旧标志（'OTA1'）或擦除态/损坏数据：全部归一化为 v2 IDLE。
     * 注：v1 bootloader 看到 v2 magic 会直接跳过升级逻辑，双方向兼容 */
    memset(&f, 0, sizeof(f));
    f.magic = OTA_FLAG_MAGIC;
    f.ver   = OTA_FLAG_VER;
    f.state = OTA_STATE_IDLE;
    flag_commit(&f);
}

/* 备份槽 A → 槽 B 金固件。返回 0 成功。
 * 长操作（擦 14 扇区 + 写 56KB + 校验），过程中按块喂狗。 */
static int golden_backup(void)
{
    OTA_ImageHdr_t hdr;
    W25_Read(OTA_HDR_A_ADDR, (uint8_t *)&hdr, sizeof(hdr));
    if (hdr.magic != OTA_IMG_MAGIC || hdr.size == 0 || hdr.size > OTA_FW_MAX_SIZE)
        return -1;

    /* 备份前再校验一次槽 A（Bootloader 搬运前校验过，这里防搬运期间的位翻转） */
    uint8_t buf[256];
    uint32_t pos = 0, crc = 0xFFFFFFFF;
    while (pos < hdr.size) {
        uint32_t chunk = (hdr.size - pos > 256) ? 256 : (hdr.size - pos);
        W25_Read(OTA_FW_ADDR + pos, buf, chunk);
        crc = crc32_update(crc, buf, chunk);
        pos += chunk;
    }
    if ((~crc) != hdr.crc32)
        return -1;

    /* 擦槽 B（镜像头扇区 + 数据扇区），逐扇区喂狗防看门狗超时 */
    W25_EraseSector(OTA_HDR_B_ADDR);
    Supervisor_IWDGFeed();
    for (uint32_t addr = OTA_GOLDEN_ADDR;
         addr < OTA_GOLDEN_ADDR + hdr.size;
         addr += W25_SECTOR_SIZE) {
        W25_EraseSector(addr);
        Supervisor_IWDGFeed();
    }

    /* 逐块拷贝 A → B */
    for (pos = 0; pos < hdr.size; pos += 256) {
        uint16_t chunk = (uint16_t)((hdr.size - pos > 256) ? 256 : (hdr.size - pos));
        W25_Read(OTA_FW_ADDR + pos, buf, chunk);
        W25_WritePage(OTA_GOLDEN_ADDR + pos, buf, chunk);
        if ((pos & 0x7FF) == 0)          /* 每 2KB 喂一次 */
            Supervisor_IWDGFeed();
    }

    /* 回读槽 B 校验，确认金固件真正落盘 */
    crc = 0xFFFFFFFF;
    for (pos = 0; pos < hdr.size; pos += 256) {
        uint32_t chunk = (hdr.size - pos > 256) ? 256 : (hdr.size - pos);
        W25_Read(OTA_GOLDEN_ADDR + pos, buf, chunk);
        crc = crc32_update(crc, buf, chunk);
    }
    if ((~crc) != hdr.crc32)
        return -2;

    /* 写槽 B 镜像头 */
    W25_WritePage(OTA_HDR_B_ADDR, (const uint8_t *)&hdr, sizeof(hdr));
    return 0;
}

/* 开机黑匣子流程：记录本次复位原因 + 转录上次崩溃现场 + 清复位标志 */
static void blackbox_boot_log(void)
{
    /* 先取证据，再做任何 SPI 操作（bootloader 不清 CSR，原因只有这里读得到） */
    uint32_t reset_csr = RCC->CSR;
    int have_crash = (g_crash_mailbox.magic == CRASH_MAGIC);
    CrashMailbox_t crash;
    if (have_crash)
        memcpy(&crash, &g_crash_mailbox, sizeof(crash));

    g_ota_backup_busy = 1;          /* SPI 长操作期间挡住 OTA/日志查询 */
    Supervisor_IWDGFeed();
    Log_Init();
    Supervisor_IWDGFeed();

    /* 开机记录：版本 / 复位原因 / 升级状态 */
    uint32_t f[6] = {0};
    f[0] = APP_VERSION;
    f[1] = reset_csr;
    f[2] = ((uint32_t)g_ota_flag_cache.state << 24)
         | ((uint32_t)g_ota_flag_cache.golden_valid << 16)
         | g_ota_flag_cache.retry_cnt;
    Log_Write(LOG_BLACKBOX, REC_BOOT, f);

    /* 崩溃现场转录（转录成功才清邮箱，失败保留到下次开机重试） */
    if (have_crash) {
        f[0] = crash.cfsr;
        f[1] = crash.hfsr;
        f[2] = crash.bfar;
        f[3] = crash.pc;
        f[4] = crash.lr;
        f[5] = crash.uptime;
        if (Log_Write(LOG_BLACKBOX, REC_FAULT, f) == 0)
            g_crash_mailbox.magic = 0;
    }

    /* PVD 欠压转录：跨复位存活的邮箱 → REC_PVD（转录成功才清计数） */
    if (g_pvd_mailbox.magic == PVD_DIP_MAGIC && g_pvd_mailbox.count != 0) {
        memset(f, 0, sizeof(f));
        f[0] = g_pvd_mailbox.count;
        f[1] = g_pvd_mailbox.uptime;
        f[2] = (PWR->CSR & PWR_CSR_PVDO) ? 1u : 0u;   /* 开机瞬间是否仍欠压 */
        if (Log_Write(LOG_BLACKBOX, REC_PVD, f) == 0)
            g_pvd_mailbox.count = 0;
    }
    Supervisor_IWDGFeed();
    g_ota_backup_busy = 0;

    /* 证据入柜后清复位标志，保证下次读到的是新一次复位的原因 */
    __HAL_RCC_CLEAR_RESET_FLAGS();
}

void StartTaskSupervisor(void *argument)
{
    (void)argument;

    flag_boot_init();
    blackbox_boot_log();

    uint32_t healthy_since = 0;          /* 0 = 尚未开始连续健康计时 */
    uint8_t  confirmed = 0;              /* 本次开机只确认一次 */
    uint32_t last_stats_tick = HAL_GetTick();
    uint32_t pvd_logged_count = 0;       /* 已转录的欠压计数（防重复落盘） */

    /* 运行期收紧看门狗：从 26s 早期窗口收紧到 8s（LSI 典型值） */
    iwdg_reload_config(IWDG_RLR_RUN);
    Supervisor_IWDGFeed();

    for (;;) {
        osDelay(SUPERVISOR_PERIOD_MS);

        if (health_check()) {
            Supervisor_IWDGFeed();
            if (healthy_since == 0)
                healthy_since = HAL_GetTick();
        } else {
            healthy_since = 0;           /* 失守，健康计时清零重新来 */
        }

        /* 光照统计快照：断电最多丢最后一个周期 */
        if (HAL_GetTick() - last_stats_tick >= STATS_INTERVAL_MS) {
            last_stats_tick = HAL_GetTick();
            g_ota_backup_busy = 1;
            uint32_t f[6] = {
                HAL_GetTick() / 1000u,
                today_dark_sec, today_dim_sec, today_ideal_sec, today_glare_sec,
                APP_VERSION,
            };
            Log_Write(LOG_STATS, REC_STATS, f);
            g_ota_backup_busy = 0;
            Supervisor_IWDGFeed();
        }

        /* 欠压 dip 未引发复位时运行期补记（复位路径由 blackbox_boot_log 转录） */
        if (g_pvd_mailbox.magic == PVD_DIP_MAGIC &&
            g_pvd_mailbox.count != pvd_logged_count) {
            uint32_t f[6] = {0};
            f[0] = g_pvd_mailbox.count;
            f[1] = g_pvd_mailbox.uptime;
            g_ota_backup_busy = 1;
            if (Log_Write(LOG_BLACKBOX, REC_PVD, f) == 0)
                pvd_logged_count = g_pvd_mailbox.count;
            g_ota_backup_busy = 0;
            Supervisor_IWDGFeed();
        }

        /* TESTING 固件健康跑满观察期 → 确认启动（两阶段提交的第二阶段） */
        if (!confirmed &&
            healthy_since != 0 &&
            HAL_GetTick() - healthy_since >= CONFIRM_DELAY_MS &&
            g_ota_flag_cache.state == OTA_STATE_TESTING &&
            !g_ota_session_active) {     /* 传输进行中不打扰 */

            g_ota_backup_busy = 1;       /* 挡住新的 OTA START/END（见优先级分析） */
            int ret = golden_backup();
            g_ota_backup_busy = 0;
            Supervisor_IWDGFeed();

            if (ret == 0) {
                OTA_Flag_t f;
                memcpy(&f, &g_ota_flag_cache, sizeof(f));
                f.state = OTA_STATE_IDLE;
                f.golden_valid = 1;
                f.retry_cnt = 0;
                flag_commit(&f);
                confirmed = 1;
                char msg[48];
                int n = snprintf(msg, sizeof(msg),
                    "OTA confirmed, golden=v%08lX\r\n",
                    (unsigned long)APP_VERSION);
                HAL_UART_Transmit(&huart1, (uint8_t *)msg, (uint16_t)n, 100);
            } else {
                /* 备份失败：保持 TESTING（还能靠回滚计数保护），下轮重试 */
                log_line("golden backup FAIL\r\n");
            }
        }
    }
}
