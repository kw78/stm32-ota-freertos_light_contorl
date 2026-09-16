#ifndef LIGHT_CTRL_H
#define LIGHT_CTRL_H

#include <stdint.h>

/*
 * 光照闭环调光（P3）：LED(PA7 = TIM3_CH2) 从"状态指示灯"升级为"执行器"
 *
 * 控制回路: TIM3_CH2 PWM 驱动 LED → 光照传感器(ADC) → PI 控制器(10Hz)
 *           → 调节占空比，把 ADC 读数稳定在 IDEAL 频带 [1000, 1200]
 *
 * 与 Task_LED 状态指示灯的共存：
 *   调光关闭时 PA7 是普通推挽输出，Task_LED 的 TogglePin 正常闪灯；
 *   调光开启时 PA7 切到复用推挽(AF)，ODR 与引脚脱开——Task_LED 继续
 *   Toggle 也只是写一个无人消费的寄存器，天然零冲突。
 *
 * 传感器方向：本项目 ADC 读数越高 = 环境越暗（DARK_EXIT=2800 > GLARE<800），
 * LED 变亮 → ADC 读数下降，控制器方向按此约定。
 */

void     LightCtrl_Init(void);                            /* 早期：TIM3 重定时到 1kHz */
void     LightCtrl_Start(void);                           /* osKernelInitialize 之后：建控制定时器 */
void     LightCtrl_SetEnable(uint8_t enable, uint16_t target_adc);
uint8_t  LightCtrl_Enabled(void);

#endif
