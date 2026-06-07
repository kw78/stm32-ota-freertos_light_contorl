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
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityHigh4,
};
/* Definitions for queue_light */
osMessageQueueId_t queue_lightHandle;
const osMessageQueueAttr_t queue_light_attributes = {
  .name = "queue_light"
};
/* Definitions for myQueue02 */
osMessageQueueId_t myQueue02Handle;
const osMessageQueueAttr_t myQueue02_attributes = {
  .name = "myQueue02"
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
void StartOLEDTask(void *argument);
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

  /* creation of myQueue02 */
  myQueue02Handle = osMessageQueueNew (16, 24, &myQueue02_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of Task_Light */
  Task_LightHandle = osThreadNew(StartTaskLight, NULL, &Task_Light_attributes);

  /* creation of Task_MPU */
  Task_MPUHandle = osThreadNew(StartTaskMPU, NULL, &Task_MPU_attributes);

  /* creation of Task_OLED */
  Task_OLEDHandle = osThreadNew(StartOLEDTask, NULL, &Task_OLED_attributes);

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
    osDelay(1);
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
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartTaskMPU */
}

/* USER CODE BEGIN Header_StartOLEDTask */
/**
* @brief Function implementing the Task_OLED thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartOLEDTask */
void StartOLEDTask(void *argument)
{
  /* USER CODE BEGIN StartOLEDTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartOLEDTask */
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
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
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
  for(;;)
  {
    osDelay(1);
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
    osDelay(1);
  }
  /* USER CODE END StartTaskUART */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

