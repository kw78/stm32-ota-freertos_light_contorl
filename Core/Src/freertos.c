/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "oled.h"
#include "MPU6050.h"
#include <string.h>
#include "w25d64.h"
#include "usart.h"

extern uint16_t adc_buf[];
extern volatile LightState_t state_now;
extern void ftoa_2dp(char *buf, float val);
extern uint8_t uart_rx_buf[128];
extern void OTA_Process(void);
void LightState_Update(uint16_t adc_value);  //函数声明
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
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for Task_Light */
osThreadId_t Task_LightHandle;
const osThreadAttr_t Task_Light_attributes = {
  .name = "Task_Light",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal4,
};
/* Definitions for Task_MPU */
osThreadId_t Task_MPUHandle;
const osThreadAttr_t Task_MPU_attributes = {
  .name = "Task_MPU",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal4,
};
/* Definitions for Task_OLED */
osThreadId_t Task_OLEDHandle;
const osThreadAttr_t Task_OLED_attributes = {
  .name = "Task_OLED",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal4,
};
/* Definitions for Task_LED */
osThreadId_t Task_LEDHandle;
const osThreadAttr_t Task_LED_attributes = {
  .name = "Task_LED",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow4,
};
/* Definitions for Task_SPIFlash */
osThreadId_t Task_SPIFlashHandle;
const osThreadAttr_t Task_SPIFlash_attributes = {
  .name = "Task_SPIFlash",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow3,
};
/* Definitions for Task_UART */
osThreadId_t Task_UARTHandle;
const osThreadAttr_t Task_UART_attributes = {
  .name = "Task_UART",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityHigh4,
};
/* Definitions for queue_light */
osMessageQueueId_t queue_lightHandle;
const osMessageQueueAttr_t queue_light_attributes = {
  .name = "queue_light"
};
/* Definitions for queue_mpu */
osMessageQueueId_t queue_mpuHandle;
const osMessageQueueAttr_t queue_mpu_attributes = {
  .name = "queue_mpu"
};
/* Definitions for queue_flash */
osMessageQueueId_t queue_flashHandle;
const osMessageQueueAttr_t queue_flash_attributes = {
  .name = "queue_flash"
};
/* Definitions for sem_adc_ready */
osSemaphoreId_t sem_adc_readyHandle;
const osSemaphoreAttr_t sem_adc_ready_attributes = {
  .name = "sem_adc_ready"
};
/* Definitions for sem_uart_rx */
osSemaphoreId_t sem_uart_rxHandle;
const osSemaphoreAttr_t sem_uart_rx_attributes = {
  .name = "sem_uart_rx"
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartTaskLight(void *argument);
void StartTaskMPU(void *argument);
void StartTaskOLED(void *argument);
void StartTaskLED(void *argument);
void StartTaskSPIFlash(void *argument);
void StartTaskUART(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* Create the semaphores(s) */
  /* creation of sem_adc_ready */
  sem_adc_readyHandle = osSemaphoreNew(1, 0, &sem_adc_ready_attributes);

  /* creation of sem_uart_rx */
  sem_uart_rxHandle = osSemaphoreNew(1, 0, &sem_uart_rx_attributes);

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of queue_light */
  queue_lightHandle = osMessageQueueNew (5, sizeof(uint32_t), &queue_light_attributes);

  /* creation of queue_mpu */
  queue_mpuHandle = osMessageQueueNew (8, sizeof(MpuData_t), &queue_mpu_attributes);

  /* creation of queue_flash */
  queue_flashHandle = osMessageQueueNew (8, sizeof(FlashCmd_t), &queue_flash_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of Task_Light */
  Task_LightHandle = osThreadNew(StartTaskLight, NULL, &Task_Light_attributes);

  /* creation of Task_MPU */
  Task_MPUHandle = osThreadNew(StartTaskMPU, NULL, &Task_MPU_attributes);

  /* creation of Task_OLED */
  Task_OLEDHandle = osThreadNew(StartTaskOLED, NULL, &Task_OLED_attributes);

  /* creation of Task_LED */
  Task_LEDHandle = osThreadNew(StartTaskLED, NULL, &Task_LED_attributes);

  /* creation of Task_SPIFlash */
  Task_SPIFlashHandle = osThreadNew(StartTaskSPIFlash, NULL, &Task_SPIFlash_attributes);

  /* creation of Task_UART */
  Task_UARTHandle = osThreadNew(StartTaskUART, NULL, &Task_UART_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartTaskLight */
/**
  * @brief  Function implementing the Task_Light thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartTaskLight */
void StartTaskLight(void *argument)
{
  /* USER CODE BEGIN StartTaskLight */
  /* Infinite loop */
  for(;;)
  {
    osSemaphoreAcquire(sem_adc_readyHandle,osWaitForever);
    uint16_t cur = adc_buf[0];  //读取ADC_DMA
    LightState_Update(cur);
    osMessageQueuePut(queue_lightHandle,&state_now,0,0);
    osDelay(10);
  }
  /* USER CODE END StartTaskLight */
}

/* USER CODE BEGIN Header_StartTaskMPU */
/**
* @brief Function implementing the Task_MPU thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskMPU */
void StartTaskMPU(void *argument)
{
  /* USER CODE BEGIN StartTaskMPU */
  MpuData_t data;
  /* Infinite loop */
  for(;;)
  {
    MPU6050_Read_Accel(&data.ax, &data.ay, &data.az);
    MPU6050_Cacul_Tangle(&data.Pitch, &data.Roll, &data.ax, &data.ay, &data.az);
    osMessageQueuePut(queue_mpuHandle,&data,0,0);
    osDelay(200);
  }
  /* USER CODE END StartTaskMPU */
}

/* USER CODE BEGIN Header_StartTaskOLED */
/**
* @brief Function implementing the Task_OLED thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskOLED */
void StartTaskOLED(void *argument)
{
  /* USER CODE BEGIN StartTaskOLED */
  MpuData_t received;
  char state_str[6];
  char line[22];
  char num[8];
  int time;
  /* Infinite loop */
  for(;;)
  {
    osMessageQueueGet(queue_mpuHandle, &received, 0, osWaitForever);  // 从队列拷贝出来
    switch (state_now)
      {
      case STATE_DARK:  strcpy(state_str,"Dark "); break;
      case STATE_DIM:   strcpy(state_str,"Dim  "); break;
      case STATE_IDEAL: strcpy(state_str,"Ideal"); break;
      case STATE_GLARE: strcpy(state_str,"Glare"); break;
      default:          strcpy(state_str,"ERROR"); break;
      }
    
    OLED_ShowString(0, 0, state_str);

    strcpy(line, "ax:");
    ftoa_2dp(num, received.ax);
    strcat(line, num);
    strcat(line, " P:");
    ftoa_2dp(num, received.Pitch);
    strcat(line, num);
    OLED_ShowString(0, 1, line);

    strcpy(line, "ay:");
    ftoa_2dp(num, received.ay);
    strcat(line, num);
    strcat(line, " R:");
    ftoa_2dp(num, received.Roll);
    strcat(line, num);
    OLED_ShowString(0, 2, line);

    strcpy(line, "az:");
    ftoa_2dp(num, received.az);
    strcat(line, num);
    OLED_ShowString(0, 3, line);
    osDelay(200);
  }

  /* USER CODE END StartTaskOLED */
}

/* USER CODE BEGIN Header_StartTaskLED */
/**
* @brief Function implementing the Task_LED thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskLED */
void StartTaskLED(void *argument)
{
  /* USER CODE BEGIN StartTaskLED */
  LightState_t light;
  /* Infinite loop */
  for(;;)
  {
    osMessageQueueGet(queue_lightHandle,&light,0,osWaitForever);
    switch(state_now)
    {
    case STATE_DARK:  osDelay(100); HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin); break;   // 快闪
    case STATE_DIM:   osDelay(500); HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin); break;
    case STATE_IDEAL: osDelay(1000); HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin); break;   // 慢闪
    case STATE_GLARE: osDelay(200); HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin); break;
    }
  }
  /* USER CODE END StartTaskLED */
}

/* USER CODE BEGIN Header_StartTaskSPIFlash */
/**
* @brief Function implementing the Task_SPIFlash thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskSPIFlash */
void StartTaskSPIFlash(void *argument)
{
  /* USER CODE BEGIN StartTaskSPIFlash */
  /* Infinite loop */
  FlashCmd_t cmd;
  for(;;)
  {
    if(osMessageQueueGet(queue_flashHandle,&cmd,0,osWaitForever) == osOK){
      switch(cmd.cmd){
        case CMD_SAVE_LIGHT_STATS:
          W25_EraseSector(cmd.addr);
          W25_WritePage(cmd.addr,cmd.data,cmd.len);
          break;
        case CMD_SAVE_CONFIG:
          W25_EraseSector(cmd.addr);
          W25_WritePage(cmd.addr,cmd.data,cmd.len);
          break;
        case CMD_SAVE_LOG:
          W25_WritePage(cmd.addr,cmd.data,cmd.len);
          break;                  
      }
    }
  }
  /* USER CODE END StartTaskSPIFlash */
}

/* USER CODE BEGIN Header_StartTaskUART */
/**
* @brief Function implementing the Task_UART thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskUART */
void StartTaskUART(void *argument)
{
  /* USER CODE BEGIN StartTaskUART */
  /* Infinite loop */
  for(;;)
  {
    // 先获取信号量，然后处理uart_buf
    osSemaphoreAcquire(sem_uart_rxHandle,osWaitForever);

    OTA_Process();
    memset(uart_rx_buf,0,sizeof(uart_rx_buf));  // 清空缓冲区
  }
  /* USER CODE END StartTaskUART */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

