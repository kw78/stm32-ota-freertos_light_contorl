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

// 搬运固件，带回读校验，返回 0=成功, -1=写入失败, -2=回读校验失败
static int copy_firmware(uint32_t src_addr, uint32_t dst_addr, uint32_t size){
    uint8_t buf[256];
    uint8_t readback[256];
    HAL_FLASH_Unlock();
    for(uint32_t pos = 0; pos < size; pos += 256){
        uint32_t chunk = (size - pos > 256) ? 256 : (size - pos);
        // 从 SPI Flash 读取
        W25_Read(src_addr + pos, buf, chunk);
        // 写入内部 Flash（半字）
        for(uint32_t i = 0; i < chunk; i += 2){
            uint16_t halfword = buf[i] | (buf[i+1] << 8);
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

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    my_MX_GPIO_Init();
    MX_SPI2_Init();

    // 验证 SPI Flash 是否可用
    uint32_t spi_id = W25_ReadID();
    if (spi_id == 0x000000 || spi_id == 0xFFFFFF) {
        // SPI Flash 不响应，直接跳转 App
        jump_to_app();
        while(1) {}
    }

    // 读 OTA 标志
    OTA_Flag_t flag;
    W25_Read(OTA_FLAG_ADDR, (uint8_t *)&flag, sizeof(flag));

    // 检查是否需要更新
    if (flag.magic == OTA_MAGIC && flag.state == OTA_STATE_PENDING
        && flag.fw_size <= OTA_FW_MAX_SIZE)
    {
        // 先校验 SPI Flash 中固件的 CRC32（不拷贝）
        uint8_t crc_buf[256];
        uint32_t crc_pos = 0;
        uint32_t calc_crc = 0xFFFFFFFF;
        while (crc_pos < flag.fw_size) {
            uint32_t chunk = (flag.fw_size - crc_pos > 256) ? 256 : (flag.fw_size - crc_pos);
            W25_Read(OTA_FW_ADDR + crc_pos, crc_buf, chunk);
            calc_crc = crc32_update(calc_crc, crc_buf, chunk);
            crc_pos += chunk;
        }
        calc_crc = ~calc_crc;

        // 合法性检查：栈顶地址必须在 RAM 范围内
        uint32_t app_sp;
        W25_Read(OTA_FW_ADDR, (uint8_t *)&app_sp, 4);
        int sp_valid = (app_sp >= 0x20000000 && app_sp <= 0x20005000);

        if (calc_crc == flag.fw_crc32 && sp_valid)
        {
            // CRC 匹配且固件合法
            int copy_ret = copy_firmware(OTA_FW_ADDR, APP_START_ADDR, flag.fw_size);
            if (copy_ret == 0) {
                // 搬运成功且校验通过
                flag.state = OTA_STATE_IDLE;
                W25_EraseSector(OTA_FLAG_ADDR);
                W25_WritePage(OTA_FLAG_ADDR, (uint8_t *)&flag, sizeof(flag));
                // 重写flag
            }
            // copy_ret != 0 时保留 PENDING 标志，下次复位重试
        }
        else
        {
            // CRC 不匹配或固件非法
            flag.state = OTA_STATE_IDLE;
            W25_EraseSector(OTA_FLAG_ADDR);
            W25_WritePage(OTA_FLAG_ADDR, (uint8_t *)&flag, sizeof(flag));
        }
    }

    jump_to_app();
    while(1) {}
}