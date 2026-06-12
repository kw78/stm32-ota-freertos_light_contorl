#include "stm32f1xx_hal.h"
#include "w25d64.h"
#include "ota.h"
#include <string.h>

SPI_HandleTypeDef hspi2;

#define APP_START_ADDR 0x08002000

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


static void flash_erase_app(void){
    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef erase;
    uint32_t error;
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = APP_START_ADDR;
    erase.NbPages = 54;   // 54KB / 1KB = 54 页
    HAL_FLASHEx_Erase(&erase, &error);
    HAL_FLASH_Lock();
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

static void jump_to_app(void){
    uint32_t app_sp = *(volatile uint32_t *)APP_START_ADDR;       // 读栈顶
    uint32_t app_pc = *(volatile uint32_t *)(APP_START_ADDR + 4); // 读入口地址

    __disable_irq();              // 关中断
    SysTick->CTRL = 0;            // 关 SysTick
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;
    __set_MSP(app_sp);            // 设置栈指针
    SCB->VTOR = APP_START_ADDR;   // 重定向向量表
    __enable_irq();               // 开中断
    ((void(*)(void))app_pc)();    // 跳转
}

// 半字写入flash
static void copy_firmware(uint32_t src_addr, uint32_t dst_addr, uint32_t size){
    uint8_t buf[256];
    HAL_FLASH_Unlock();
    for(uint32_t pos = 0; pos < size; pos += 256){
        // 一页一页写入
        uint32_t chunk = (size - pos > 256) ? 256 : (size - pos);
        W25_Read(src_addr + pos, buf, chunk);
        for(uint32_t i = 0; i < chunk; i += 2){
            uint16_t halfword = buf[i] | (buf[i+1] << 8);
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, dst_addr + pos + i, halfword);            
        }
    }
    HAL_FLASH_Lock();
}

static uint32_t crc32_verify(uint32_t flash_addr, uint32_t size)
{
    return crc32_compute((const uint8_t *)flash_addr, size);
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    my_MX_GPIO_Init();
    MX_SPI2_Init();

    // 读 OTA 标志
    OTA_Flag_t flag;
    W25_Read(OTA_FLAG_ADDR, (uint8_t *)&flag, sizeof(flag));

    // 检查是否需要更新
    if (flag.magic == OTA_MAGIC && flag.state == OTA_STATE_PENDING
        && flag.fw_size <= OTA_FW_MAX_SIZE)
    {
        // 校验固件 CRC
        // 搬运到内部 Flash
        copy_firmware(OTA_FW_ADDR, APP_START_ADDR, flag.fw_size);

        // 回读内部 Flash 验证 CRC
        if (crc32_verify(APP_START_ADDR, flag.fw_size) == flag.fw_crc32)
        {
            // 清除 OTA 标志
            flag.state = OTA_STATE_IDLE;
            W25_EraseSector(OTA_FLAG_ADDR);
            W25_WritePage(OTA_FLAG_ADDR, (uint8_t *)&flag, sizeof(flag));
        }
    }

    jump_to_app();
    while(1) {}
}