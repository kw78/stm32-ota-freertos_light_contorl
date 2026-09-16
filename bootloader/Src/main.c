#include "stm32f1xx_hal.h"
#include "w25d64.h"
#include "ota.h"
#include <string.h>

SPI_HandleTypeDef hspi2;

#define APP_START_ADDR 0x08002000

void SysTick_Handler(void) { HAL_IncTick(); }

void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */


void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL16;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

void MX_SPI2_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_SPI2_CLK_ENABLE();

  GPIO_InitStruct.Pin = GPIO_PIN_13|GPIO_PIN_15;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_14;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  hspi2.Instance = SPI2;
  hspi2.Init.Mode = SPI_MODE_MASTER;
  hspi2.Init.Direction = SPI_DIRECTION_2LINES;
  hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi2.Init.NSS = SPI_NSS_SOFT;
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi2.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi2) != HAL_OK)
  {
    Error_Handler();
  }


}


static void my_MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOA, FLASH_CS_Pin, GPIO_PIN_RESET);

  GPIO_InitStruct.Pin = FLASH_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

}

static void __attribute__((naked)) jump_to_app(void){
    __asm volatile(
        "cpsid i                    \n"
        "ldr r0, [%0]              \n"
        "ldr r1, [%0, #4]          \n"
        "msr MSP, r0               \n"
        "movw r2, #0xED08           \n"
        "movt r2, #0xE000           \n"
        "str %0, [r2]              \n"
        "cpsie i                    \n"
        "bx r1                     \n"
        :
        : "r" (APP_START_ADDR)
        : "r0", "r1", "r2"
    );
}
// debug得出

// 搬运固件，带回读校验，返回 0=成功, -1=写入失败, -2=回读校验失败, -3=擦除失败
static int copy_firmware(uint32_t src_addr, uint32_t dst_addr, uint32_t size){    uint8_t buf[256];
    uint8_t readback[256];
    HAL_FLASH_Unlock();

    // F1 内部 Flash 只能 1->0 编程，必须先按页擦除目标区域，
    // 否则旧 App 残留的 0 位会让回读校验失败（且 CRC 与预期不符）
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_err = 0;
    erase.TypeErase    = FLASH_TYPEERASE_PAGES;
    erase.Banks        = FLASH_BANK_1;
    erase.PageAddress  = dst_addr;
    erase.NbPages      = (size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    if (HAL_FLASHEx_Erase(&erase, &page_err) != HAL_OK) {
        HAL_FLASH_Lock();
        return -3;
    }

    for(uint32_t pos = 0; pos < size; pos += 256){
        uint32_t chunk = (size - pos > 256) ? 256 : (size - pos);
        // 从 SPI Flash 读取
        W25_Read(src_addr + pos, buf, chunk);
        // 写入内部 Flash（半字），奇数字节的补位写 0xFF（保持擦除态）
        for(uint32_t i = 0; i < chunk; i += 2){
            uint16_t halfword = buf[i] | ((i + 1 < chunk ? buf[i+1] : 0xFF) << 8);
            if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, dst_addr + pos + i, halfword) != HAL_OK){
                HAL_FLASH_Lock();
                return -1;
            }
        }
        // 回读内部 Flash 并对比
        memcpy(readback, (void *)(dst_addr + pos), chunk);
        if(memcmp(buf, readback, chunk) != 0){
            // 如果不同，返回-2
            HAL_FLASH_Lock();
            return -2;
        }
    }
    HAL_FLASH_Lock();
    return 0;
}

// ---- OTA v2：控制块与槽位操作 ----

static void flag_read(OTA_Flag_t *f)
{
    W25_Read(OTA_FLAG_ADDR, (uint8_t *)f, sizeof(*f));
}

static void flag_write(const OTA_Flag_t *f)
{
    W25_EraseSector(OTA_FLAG_ADDR);
    W25_WritePage(OTA_FLAG_ADDR, (const uint8_t *)f, sizeof(*f));
}

// 校验一个槽位：镜像头合法 + 固件 CRC32 匹配 + 栈顶在 RAM 范围内。
// 返回 1 可用；size_out 可为 NULL
static int slot_verify(uint32_t hdr_addr, uint32_t data_addr, uint32_t *size_out)
{
    OTA_ImageHdr_t hdr;
    W25_Read(hdr_addr, (uint8_t *)&hdr, sizeof(hdr));
    if (hdr.magic != OTA_IMG_MAGIC || hdr.size == 0 || hdr.size > OTA_FW_MAX_SIZE)
        return 0;

    uint8_t buf[256];
    uint32_t pos = 0;
    uint32_t crc = 0xFFFFFFFF;
    while (pos < hdr.size) {
        uint32_t chunk = (hdr.size - pos > 256) ? 256 : (hdr.size - pos);
        W25_Read(data_addr + pos, buf, chunk);
        crc = crc32_update(crc, buf, chunk);
        pos += chunk;
    }
    if ((~crc) != hdr.crc32)
        return 0;

    // 合法性检查：栈顶地址必须在 RAM 范围内
    uint32_t app_sp;
    W25_Read(data_addr, (uint8_t *)&app_sp, 4);
    if (app_sp < 0x20000000 || app_sp > 0x20005000)
        return 0;

    if (size_out) *size_out = hdr.size;
    return 1;
}

int main(void)
{
    // 第一件事读复位原因：IWDG 复位是"候选固件跑挂"的证据，用于回滚计数。
    // 用户按 NRST / 断电重启不算固件的错，不计入。
    uint8_t reset_by_iwdg = ((RCC->CSR & RCC_CSR_IWDGRSTF) != 0) ? 1 : 0;

    HAL_Init();
    SystemClock_Config();
    my_MX_GPIO_Init();
    MX_SPI2_Init();

    // 验证 SPI Flash 是否可用
    uint32_t spi_id = W25_ReadID();
    if (spi_id == 0x000000 || spi_id == 0xFFFFFF) {
        // SPI Flash 不响应，直接跳转 App
        SysTick->CTRL = 0;   // 关闭 Bootloader 的 SysTick，避免跳转后误触发
        jump_to_app();
        while(1) {}
    }

    OTA_Flag_t flag;
    flag_read(&flag);
    uint8_t flag_dirty = 0;
    uint32_t size = 0;

    // 只认 v2 契约的标志（v1 旧标志或杂数据一律视为无升级活动，直接跳 App；
    // 旧标志由 App 侧 supervisor 完成一次性迁移）
    if (flag.magic == OTA_FLAG_MAGIC && flag.ver == OTA_FLAG_VER) {
        if (flag.state == OTA_STATE_PENDING) {
            if (slot_verify(OTA_HDR_A_ADDR, OTA_FW_ADDR, &size)) {
                // 暂存镜像有效：搬运到内部 Flash，进入 TESTING 等 App 确认
                if (copy_firmware(OTA_FW_ADDR, APP_START_ADDR, size) == 0) {
                    flag.state = OTA_STATE_TESTING;
                    flag.retry_cnt = 0;
                } else if (flag.golden_valid &&
                           slot_verify(OTA_HDR_B_ADDR, OTA_GOLDEN_ADDR, &size)) {
                    // 搬运失败（内部 Flash 写入异常）：退回金固件
                    copy_firmware(OTA_GOLDEN_ADDR, APP_START_ADDR, size);
                    flag.state = OTA_STATE_IDLE;
                    flag.retry_cnt = 0;
                } else {
                    // 搬运失败且无金固件：维持现状（当前 App 可能仍完整）
                    flag.state = OTA_STATE_IDLE;
                    flag.retry_cnt = 0;
                }
                flag_dirty = 1;
            } else {
                // 暂存镜像损坏：不动内部 Flash，当前 App 原样运行；
                // 有金固件则顺带回滚，让设备回到已知良好状态
                if (flag.golden_valid &&
                    slot_verify(OTA_HDR_B_ADDR, OTA_GOLDEN_ADDR, &size)) {
                    copy_firmware(OTA_GOLDEN_ADDR, APP_START_ADDR, size);
                }
                flag.state = OTA_STATE_IDLE;
                flag.retry_cnt = 0;
                flag_dirty = 1;
            }
        } else if (flag.state == OTA_STATE_TESTING && reset_by_iwdg) {
            // 候选固件看门狗复位：计数，超限回滚
            if (flag.retry_cnt < 0xFFFF) flag.retry_cnt++;
            if (flag.retry_cnt > OTA_ROLLBACK_LIMIT && flag.golden_valid &&
                slot_verify(OTA_HDR_B_ADDR, OTA_GOLDEN_ADDR, &size)) {
                copy_firmware(OTA_GOLDEN_ADDR, APP_START_ADDR, size);
                flag.state = OTA_STATE_IDLE;
                flag.retry_cnt = 0;
            }
            flag_dirty = 1;
        }

        // 复位原因标志保留给 App 侧 supervisor（转录黑匣子后由它清除）：
        // App 每次开机都清标志，本 bootloader 开头读到的永远是"本次复位"的原因
        if (flag_dirty) flag_write(&flag);
    }

    SysTick->CTRL = 0;   // 关闭 Bootloader 的 SysTick，避免跳转后误触发
    jump_to_app();
    while(1) {}
}