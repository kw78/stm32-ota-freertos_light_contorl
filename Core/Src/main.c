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
#include <string.h>
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
static const char *uart_msg = "Hello from STM32!\r\n";
static uint8_t uart_rx_buf[50];
static uint8_t rx_flag = 0;
static uint8_t change_message_flag = 0;
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

uint8_t my_itoa(uint32_t num, char *buf)
{
    uint8_t len = 0;
    if (num == 0)
        buf[len++] = '0';
    else
    {
        char temp[16];
        uint8_t i = 0;
        while (num > 0)
        {
            temp[i++] = num % 10 + '0';
            num /= 10;
        }
        while (i > 0)
            buf[len++] = temp[--i];
    }
    buf[len++] = '\r';
    buf[len++] = '\n';
    buf[len++] = '\0';
    return len;
}

#define ADC_BUF_SIZE 10
uint16_t adc_buf[ADC_BUF_SIZE];

#define DARK_EXIT 2800
#define DIM_DOWN 1800
#define DIM_UP 3000
#define IDEAL_DOWN 1000
#define IDEAL_UP 1500
#define GLARE_EXIT 800
#define N 5

volatile LightState_t state_now = STATE_DARK;

void LightState_Update(uint16_t adc_value)
{
    static LightState_t state_target = STATE_DARK;
    static uint8_t first_flag = 0;
    if (first_flag == 0)
    {
        if (adc_value < DARK_EXIT)
            state_target = state_now = STATE_DIM;
        else if (adc_value < DIM_DOWN)
            state_target = state_now = STATE_IDEAL;
        else if (adc_value < IDEAL_DOWN)
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
        if (adc_value < DIM_DOWN)
            state_target = STATE_IDEAL;
        break;
    case STATE_IDEAL:
        if (adc_value > IDEAL_UP)
            state_target = STATE_DIM;
        if (adc_value < IDEAL_DOWN)
            state_target = STATE_GLARE;
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
        if (change < N)
            change++;
        else
        {
            state_now = state_target;
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

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
    if (huart->Instance == USART1) {
        if (uart_rx_buf[0] == 'R') {
            rx_flag = 1;
        } else {
            rx_flag = 0;
        }
        change_message_flag = 1;
        memset(uart_rx_buf, 0, sizeof(uart_rx_buf));
        HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart_rx_buf, sizeof(uart_rx_buf));
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART1) {
        if (uart_rx_buf[0] == 'R') {
            rx_flag = 1;
        } else {
            rx_flag = 0;
        }
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
  MX_DMA_Init();
  MX_I2C1_Init();
  MX_SPI2_Init();
  MX_ADC1_Init();
  MX_TIM2_Init();
  MX_USART1_UART_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */
  OLED_Init();
  OLED_ShowString(0, 0, "Light Monitor");
  MPU6050_Init();

  HAL_TIM_Base_Start_IT(&htim3);
  HAL_TIM_Base_Start(&htim2);
  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buf, ADC_BUF_SIZE);

  memset(uart_rx_buf, 0, sizeof(uart_rx_buf));
  HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart_rx_buf, sizeof(uart_rx_buf));
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    static uint32_t last_oled_tick = 0;
    if (HAL_GetTick() - last_oled_tick > 200)
    {
      last_oled_tick = HAL_GetTick();

      HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);

      float ax, ay, az, Pitch, Roll;
      MPU6050_Read_Accel(&ax, &ay, &az);
      MPU6050_Cacul_Tangle(&Pitch, &Roll, &ax, &ay, &az);

      char *state_str;
      switch (state_now)
      {
      case STATE_DARK:  state_str = "Dark "; break;
      case STATE_DIM:   state_str = "Dim  "; break;
      case STATE_IDEAL: state_str = "Ideal"; break;
      case STATE_GLARE: state_str = "Glare"; break;
      default:          state_str = "ERROR"; break;
      }
      OLED_ShowString(0, 0, state_str);

      char line[22];
      char num[8];
      strcpy(line, "ax:");
      ftoa_2dp(num, ax);
      strcat(line, num);
      strcat(line, " P:");
      ftoa_2dp(num, Pitch);
      strcat(line, num);
      OLED_ShowString(0, 1, line);

      strcpy(line, "ay:");
      ftoa_2dp(num, ay);
      strcat(line, num);
      strcat(line, " R:");
      ftoa_2dp(num, Roll);
      strcat(line, num);
      OLED_ShowString(0, 2, line);

      strcpy(line, "az:");
      ftoa_2dp(num, az);
      strcat(line, num);
      char sec_buf[10];
      my_itoa(today_dark_sec, sec_buf);
      sec_buf[strlen(sec_buf) - 2] = '\0';
      strcat(line, " D:");
      strcat(line, sec_buf);
      OLED_ShowString(0, 3, line);
    }

    if (rx_flag && change_message_flag)
    {
      HAL_UART_Transmit(&huart1, (uint8_t *)uart_msg, strlen(uart_msg), HAL_MAX_DELAY);
      change_message_flag = 0;
    }
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
