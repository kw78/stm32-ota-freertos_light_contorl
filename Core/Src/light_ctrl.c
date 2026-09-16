#include "light_ctrl.h"
#include "main.h"
#include "cmsis_os.h"
#include <stdio.h>

extern TIM_HandleTypeDef htim3;
extern uint16_t adc_buf[];                 /* main.c：ADC DMA 最新采样（环形刷新） */
extern UART_HandleTypeDef huart1;

/* ---- 控制参数（上板后按实际光路整定） ---- */
#define CTRL_PERIOD_MS    100              /* 控制周期 10Hz */
#define BAND_UP           1200             /* ADC 高于此值 = 偏暗 → 加亮 */
#define BAND_DOWN         1000             /* ADC 低于此值 = 偏亮 → 减亮 */
#define KP_SHIFT          3                /* duty = (e >> 3) + (integ >> 5) */
#define KI_SHIFT          5
#define INTEG_MAX         (999 << KI_SHIFT)   /* 积分限幅 = 占空比满量程 */

/* 目标频带（target_adc 传入时以 target 为中心 ±100 重建频带） */
static uint16_t band_up = BAND_UP;
static uint16_t band_down = BAND_DOWN;

static uint8_t  ctrl_enabled = 0;
static int32_t  integ = 0;
static uint16_t duty = 0;
static osTimerId_t ctrl_timer = NULL;

static void ctrl_tick(void);

static void ctrl_timer_cb(void *argument)
{
    (void)argument;
    ctrl_tick();
}

void LightCtrl_Init(void)
{
    /* TIM3 从 1Hz 时基重构为 1kHz PWM 载波：
     * 原配置 64MHz/6400/10000 = 1Hz（统计中断）
     * 新配置 64MHz/64/1000  = 1kHz（PWM 载波 + 千分频统计，见 main.c 回调） */
    HAL_TIM_Base_Stop(&htim3);
    __HAL_TIM_SET_PRESCALER(&htim3, 64 - 1);
    __HAL_TIM_SET_AUTORELOAD(&htim3, 1000 - 1);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 0);
    TIM3->EGR = TIM_EGR_UG;                /* 立即装载 PSC/ARR */
    HAL_TIM_Base_Start_IT(&htim3);
}

void LightCtrl_Start(void)
{
    /* 控制节拍用 osTimer（回调跑在定时器服务任务里），默认不启动 */
    ctrl_timer = osTimerNew(ctrl_timer_cb, osTimerPeriodic, NULL, NULL);
}

static void ctrl_tick(void)
{
    uint16_t adc = adc_buf[0];

    /* 带死区的误差：频带内输出保持（避免传感器噪声驱动极限环） */
    int32_t e = 0;
    if (adc > band_up)       e = (int32_t)adc - band_up;       /* 偏暗，需要更亮 */
    else if (adc < band_down) e = (int32_t)adc - band_down;    /* 偏亮，需要更暗 */

    integ += e;
    if (integ > INTEG_MAX)  integ = INTEG_MAX;
    if (integ < -INTEG_MAX) integ = -INTEG_MAX;

    int32_t out = (e >> KP_SHIFT) + (integ >> KI_SHIFT);
    if (out > 999)  { out = 999; }
    if (out < 0)    { out = 0; }            /* LED 单向：只能加光不能减环境光 */
    duty = (uint16_t)out;

    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, duty);
}

void LightCtrl_SetEnable(uint8_t enable, uint16_t target_adc)
{
    if (target_adc >= 200 && target_adc <= 4000) {
        band_up   = target_adc + 100;
        band_down = (target_adc > 100 + 200) ? target_adc - 100 : 200;
    } else {
        band_up   = BAND_UP;                /* 0/越界 = 用默认 IDEAL 频带 */
        band_down = BAND_DOWN;
    }

    if (enable && !ctrl_enabled) {
        integ = 0;
        duty = 0;
        /* PA7 切复用推挽 → ODR 脱开，Task_LED 的 Toggle 自然失效 */
        GPIO_InitTypeDef g = {0};
        g.Pin = LED_Pin;
        g.Mode = GPIO_MODE_AF_PP;
        g.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(LED_GPIO_Port, &g);

        TIM_OC_InitTypeDef oc = {0};
        oc.OCMode = TIM_OCMODE_PWM1;
        oc.Pulse = 0;
        oc.OCPolarity = TIM_OCPOLARITY_HIGH;
        HAL_TIM_PWM_ConfigChannel(&htim3, &oc, TIM_CHANNEL_2);
        HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2);
        if (ctrl_timer) osTimerStart(ctrl_timer, CTRL_PERIOD_MS);
        ctrl_enabled = 1;
    } else if (!enable && ctrl_enabled) {
        if (ctrl_timer) osTimerStop(ctrl_timer);
        HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_2);
        /* PA7 回普通推挽输出，Task_LED 恢复状态闪灯 */
        GPIO_InitTypeDef g = {0};
        g.Pin = LED_Pin;
        g.Mode = GPIO_MODE_OUTPUT_PP;
        g.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(LED_GPIO_Port, &g);
        HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
        ctrl_enabled = 0;
    }
}

uint8_t LightCtrl_Enabled(void)
{
    return ctrl_enabled;
}
