/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "adc.h"
#include "dma.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "oled.h"
#include "MPU6050.h"
#include "w25d64.h"
#include "ota.h"
#include "supervisor.h"
#include "light_ctrl.h"
#include <string.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
uint8_t uart_rx_buf[128];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
extern TIM_HandleTypeDef htim3;
extern osSemaphoreId_t sem_adc_readyHandle;
extern osSemaphoreId_t sem_uart_rxHandle;
extern void OTA_RingBuf_Put(uint8_t byte);
// 声明要调用的freertos里的句柄

#define ADC_BUF_SIZE 100
uint16_t adc_buf[ADC_BUF_SIZE];

#define DARK_EXIT 2800
#define DIM_DOWN 1600
#define DIM_UP 3000
#define IDEAL_DOWN 1000
#define IDEAL_UP 1200
#define GLARE_EXIT 800
#define DEBOUNCE_COUNT 10

volatile LightState_t state_now = STATE_DARK;

void LightState_Update(uint16_t adc_value)
{
    static LightState_t state_target = STATE_DARK;
    static uint8_t first_flag = 0;
    if (first_flag == 0)
    {
        if (adc_value >= DARK_EXIT)
            state_target = state_now = STATE_DARK;
        else if (adc_value >= DIM_DOWN)
            state_target = state_now = STATE_DIM;
        else if (adc_value >= IDEAL_DOWN)
            state_target = state_now = STATE_IDEAL;
        else
            state_target = state_now = STATE_GLARE;
        first_flag++;
    }
    switch (state_now)
    {
    case STATE_DARK:
        if (adc_value < DARK_EXIT)
            state_target = STATE_DIM;
        break;
    case STATE_DIM:
        if (adc_value > DIM_UP)
            state_target = STATE_DARK;
        else if (adc_value < IDEAL_UP)
            state_target = STATE_IDEAL;
        break;
    case STATE_IDEAL:
        if (adc_value < IDEAL_DOWN)
            state_target = STATE_GLARE;
        else if (adc_value > DIM_DOWN)
            state_target = STATE_DIM;
        break;
    case STATE_GLARE:
        if (adc_value > GLARE_EXIT)
            state_target = STATE_IDEAL;
        break;
    }
    static uint8_t change = 0;
    if (state_target == state_now)
        change = 0;
    else
    {
        if (change < DEBOUNCE_COUNT)
            change++;
        else
        {
            state_now = state_target;
            state_target = state_now;
            change = 0;
        }
    }
}

volatile uint32_t today_dark_sec = 0;
volatile uint32_t today_dim_sec = 0;
volatile uint32_t today_ideal_sec = 0;
volatile uint32_t today_glare_sec = 0;
volatile uint32_t seconds_today = 0;

void ftoa_2dp(char *buf, float val) {
    int negative = 0;
    if (val < 0.0f) { negative = 1; val = -val; }
    int int_part = (int)val;
    int frac_part = (int)((val - (float)int_part) * 100.0f + 0.5f);
    if (frac_part >= 100) { int_part++; frac_part = 0; }
    char *p = buf;
    if (negative) *p++ = '-';
    if (int_part >= 100) { *p++ = '0' + (int_part / 100) % 10; }
    if (int_part >= 10)  { *p++ = '0' + (int_part / 10) % 10; }
    *p++ = '0' + (int_part % 10);
    *p++ = '.';
    *p++ = '0' + (frac_part / 10) % 10;
    *p++ = '0' + (frac_part % 10);
    *p = '\0';
}


void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART1) {
        OTA_RingBuf_Put(uart_rx_buf[0]);
        osSemaphoreRelease(sem_uart_rxHandle);
        HAL_UART_Receive_IT(&huart1, uart_rx_buf, 1);
    }
}

// UART 出错（溢出/帧错误等）时 HAL 会停掉接收中断且默认回调为空，
// 不重新挂起接收的话 OTA 通道会永久失效（一次 ORE 就足以触发）
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART1) {
        __HAL_UART_CLEAR_OREFLAG(huart);
        __HAL_UART_CLEAR_NEFLAG(huart);
        __HAL_UART_CLEAR_FEFLAG(huart);
        HAL_UART_Receive_IT(&huart1, uart_rx_buf, 1);
    }
}



void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc == &hadc1)
  {
    osSemaphoreRelease(sem_adc_readyHandle);
  }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* 第一行就武装看门狗（长超时 ~26s）：main 之前（SystemInit/数据段搬运）之外的
   * 整个启动过程都在保护圈内，跑挂最多一次 IWDG 复位，不会无限僵死。
   * IWDG 一旦启动只能复位停止，supervisor 运行后收紧到 8s 并按健康喂狗 */
  Supervisor_IWDGEarlyArm();
  Supervisor_PVDInit();      /* 欠压探测尽早武装：启动早期的电源毛刺也要留痕 */

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();

  // SPI2 初始化：在 DMA 之前，避免 DMA 干扰轮询模式 SPI
  __HAL_RCC_SPI2_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  GPIO_InitTypeDef spi_gpio = {0};
  spi_gpio.Pin = GPIO_PIN_13 | GPIO_PIN_15;
  spi_gpio.Mode = GPIO_MODE_AF_PP;
  spi_gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &spi_gpio);
  spi_gpio.Pin = GPIO_PIN_14;
  spi_gpio.Mode = GPIO_MODE_INPUT;
  spi_gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &spi_gpio);
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
  if (HAL_SPI_Init(&hspi2) != HAL_OK) { Error_Handler(); }

  // SPI Flash 软件复位 + 验证读取（在 DMA 初始化之前完成）
  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET);
  uint8_t flash_rst = 0x66;
  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET);
  HAL_SPI_Transmit(&hspi2, &flash_rst, 1, 100);
  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET);
  flash_rst = 0x99;
  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET);
  HAL_SPI_Transmit(&hspi2, &flash_rst, 1, 100);
  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET);
  HAL_Delay(5);

  uint32_t spi_id = W25_ReadID();
  uint8_t flag[4];
  W25_Read(OTA_FLAG_ADDR, flag, 4);

  // SPI 验证通过后再初始化其他外设（DMA、I2C 等）
  MX_DMA_Init();

  // I2C 总线恢复：OTA 复位可能发生在 I2C 通信中途，OLED/MPU6050 的 SDA 被拉低导致总线锁死
  {
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOB_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_6;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &gpio);
    for (int i = 0; i < 9; i++) {
      HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET);
      for (volatile int d = 0; d < 50; d++);
      HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
      for (volatile int d = 0; d < 50; d++);
    }
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_6);
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_7);
  }

  MX_I2C1_Init();
  MX_ADC1_Init();
  MX_TIM2_Init();
  MX_USART1_UART_Init();
  MX_TIM3_Init();

  // 调试信息：SPI Flash ID + OTA 标志 + 固件版本（UART 初始化之后才发得出去）
  {
    char id_buf[60];
    int id_n = snprintf(id_buf, sizeof(id_buf),
        "APP v2 %08lX %s | ID:%06lX FLAG:%02X%02X%02X%02X\r\n",
        (unsigned long)APP_VERSION, __DATE__,
        spi_id, flag[0], flag[1], flag[2], flag[3]);
    HAL_UART_Transmit(&huart1, (uint8_t *)id_buf, (uint16_t)id_n, 100);
  }

#ifdef BAD_FW_TEST
  /* 回滚验收专用镜像（BAD_FW_TEST 构建）：banner 打完立即写空指针 HardFault。
   * 预期行为：崩溃现场进 noinit 邮箱 → 关中断等 IWDG(26s) → Bootloader
   * 计数 >3 → 自动回滚金固件，设备恢复已知良好版本 */
  {
    volatile uint32_t *null_ptr = (volatile uint32_t *)0x00000000;
    *null_ptr = 0xDEADBEEF;
  }
#endif
  /* USER CODE BEGIN 2 */
  OLED_Init();
  OLED_ShowString(0, 0, "Light Monitor");
  MPU6050_Init();

  HAL_TIM_Base_Start_IT(&htim3);
  HAL_TIM_Base_Start(&htim2);

  /* TIM3 重构为 1kHz PWM 载波（调光用），统计回调改为千分频计数（见下方回调） */
  LightCtrl_Init();

  // UART RX 中断启动
  memset(uart_rx_buf, 0, sizeof(uart_rx_buf));
  HAL_UART_Receive_IT(&huart1, uart_rx_buf, 1);
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Supervisor：IWDG 喂狗 + OTA 确认启动。优先级介于 Task_UART(High) 与
   * Task_SPIFlash/LED(Low) 之间——金固件备份期间靠 g_ota_backup_busy 挡住
   * 新的 OTA 会话（Task_UART 可抢占 supervisor，看到 busy 即 NACK，无双向竞争）。
   * 栈：最深路径 golden_backup 的 buf[256]+HAL/SPI 调用帧 ≈500B，768B 留余量；
   * FreeRTOS 堆仅 8KB（6 任务+定时器任务+队列已占 ~7KB），不能再大 */
  {
    static osThreadAttr_t sup_attr = {
      .name = "Task_Supervisor",
      .stack_size = 768,
      .priority = (osPriority_t) osPriorityBelowNormal5,
    };
    if (osThreadNew(StartTaskSupervisor, NULL, &sup_attr) == NULL) {
      /* 堆耗尽等极端情况：supervisor 起不来 = 没人喂狗，26s 后看门狗复位。
       * 显式报错让现场一眼定位，而不是表现为"设备周期性重启" */
      char err[] = "FATAL: supervisor task create failed\r\n";
      HAL_UART_Transmit(&huart1, (uint8_t *)err, sizeof(err) - 1, 100);
    }
  }

  /* 调光控制定时器（默认关闭，等 CMD_LIGHT_CTRL 指令才介入） */
  LightCtrl_Start();

  /* Start ADC DMA after FreeRTOS init (semaphores must exist before ISR fires) */
  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buf, ADC_BUF_SIZE);

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
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

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM4 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */
  if (htim->Instance == TIM3)
  {
    /* TIM3 已重构为 1kHz（LightCtrl_Init），千分频恢复 1 秒节拍 */
    static uint16_t tim3_sub = 0;
    if (++tim3_sub >= 1000)
    {
      tim3_sub = 0;
      switch (state_now)
      {
      case STATE_DARK:
        today_dark_sec++;
        break;
      case STATE_DIM:
        today_dim_sec++;
        break;
      case STATE_IDEAL:
        today_ideal_sec++;
        break;
      case STATE_GLARE:
        today_glare_sec++;
        break;
      }
      seconds_today++;
      if (seconds_today >= 86400)
      {
        seconds_today = 0;
        today_dark_sec = 0;
        today_dim_sec = 0;
        today_ideal_sec = 0;
        today_glare_sec = 0;
      }
    }
  }
  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM4)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
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
