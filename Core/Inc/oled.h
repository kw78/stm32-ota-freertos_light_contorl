#ifndef OLED_H
#define OLED_H

#include "main.h"  // 我们依赖 HAL 库

void OLED_Init(void);
void OLED_ShowString(uint8_t x, uint8_t y, const char *str);

#endif