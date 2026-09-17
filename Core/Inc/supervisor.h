#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include "ota.h"

/*
 * Supervisor：OTA v2 的"确认启动"执行者 + 系统健康监测
 *
 * 职责：
 *   1. IWDG 喂狗（只在健康检查通过时喂，任一项失效 → 看门狗复位 → Bootloader 计数回滚）
 *   2. 启动时加载/迁移 OTA 控制块（v1 → v2 一次性迁移，或初始化全新标志）
 *   3. TESTING 状态下健康运行满 CONFIRM_DELAY_MS 后：
 *      备份槽 A → 槽 B 金固件，控制块回到 IDLE —— 完成"两阶段提交"
 */

void StartTaskSupervisor(void *argument);

/* IWDG（main.c 早期启动 / 本模块运行期共用） */
void Supervisor_IWDGEarlyArm(void);
void Supervisor_IWDGFeed(void);

/* PVD 欠压探测（EXTI16，阈值 2.9V）：main.c 早期启动一次 */
void Supervisor_PVDInit(void);

#endif
