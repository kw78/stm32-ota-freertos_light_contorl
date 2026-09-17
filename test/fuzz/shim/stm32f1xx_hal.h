#pragma once
/*
 * host fuzz shim：顶替 Drivers/... 的 stm32f1xx_hal.h。
 * real Core/Inc/main.h 里 #include "stm32f1xx_hal.h" 是同目录找不到的
 * quoted include，会落到 -I 路径——本目录排在最前，于是命中这里。
 * 只提供 ota.c 编译传递所需的最小类型与原型。
 */
#include <stdint.h>
#include <stddef.h>

typedef struct { int dummy; } UART_HandleTypeDef;
typedef enum { HAL_OK = 0 } HAL_StatusTypeDef;

uint32_t HAL_GetTick(void);
void HAL_UART_Transmit(UART_HandleTypeDef *h, const uint8_t *data,
                       uint16_t len, int timeout);
