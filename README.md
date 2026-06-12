# STM32 智能光照系统 — 基于 FreeRTOS 的 OTA 远程升级平台

## 项目概述

这是一个基于 **STM32F103C8T6 + FreeRTOS** 的多任务嵌入式系统，集成了光照检测、姿态传感、OLED 显示、SPI Flash 持久化存储，以及完整的 **OTA（Over-The-Air）远程固件升级**链路——从上位机通过 UART 发送固件，经 SPI Flash 暂存，由 Bootloader 搬运到内部 Flash 并校验跳转。

**本项目的核心技术价值不在功能本身，而在于完整的工程实践能力展示：** RTOS 多任务架构设计、SPI Flash 驱动开发、Bootloader 跳转机制、OTA 协议栈设计、端到端调试方法论。

---

## 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                    上位机 (Python)                               │
│  ota_upload.py ── 读取 .bin → CRC32 计算 → 分包发送 → ACK 等待  │
└───────────────────────┬─────────────────────────────────────────┘
                        │ UART 115200
┌───────────────────────▼─────────────────────────────────────────┐
│               STM32F103C8T6 (App, 0x0800_2000)                  │
│                                                                 │
│  ┌──────────┐  sem    ┌──────────┐  queue   ┌──────────┐       │
│  │Task_Light│────────→│Task_OLED │←────────│Task_MPU  │       │
│  │ADC+状态机│         │显示更新   │         │MPU6050   │       │
│  └──────────┘         └──────────┘         └──────────┘       │
│                                                                 │
│  ┌──────────┐  queue   ┌──────────┐  ringbuf ┌──────────┐     │
│  │Task_UART │←────────│ISR→RingBuf│←────────│UART RX   │     │
│  │OTA协议栈 │          │环形缓冲区 │          │中断      │     │
│  └──────────┘          └──────────┘          └──────────┘     │
│                                                                 │
│  ┌──────────┐  queue   ┌──────────┐                           │
│  │Task_SPI  │←────────│OTA写请求 │  → W25D64 SPI Flash      │
│  │Flash持久化│         │          │                           │
│  └──────────┘          └──────────┘                           │
│                                                                 │
│  Task_LED ── LED 指示灯                                        │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │ FreeRTOS 内核：抢占式调度 | SysTick 驱动 | 6 任务         │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│          Bootloader (0x0800_0000, 8KB)                          │
│  读 OTA 标志 → SPI Flash CRC32 校验 → 搬运固件 → 跳转 App      │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│          W25D64 SPI Flash (8MB)                                 │
│  OTA 标志区 | 固件暂存区 | 配置参数区 | 日志区                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 技术栈

| 层次 | 技术 |
|------|------|
| MCU | STM32F103C8T6 (Cortex-M3, 64MHz) |
| RTOS | FreeRTOS (CMSIS-RTOS V2, 6 任务, 抢占式调度) |
| 外设驱动 | ADC (光照) + I2C (OLED/MPU6050) + SPI (W25D64) + UART |
| 存储 | W25D64 SPI Flash (8MB) 持久化 + OTA 固件暂存 |
| OTA 协议 | 自定义协议 (包头+命令+数据+CRC16) + CRC32 固件校验 |
| Bootloader | Cortex-M naked 跳转 + SPI Flash 搬运 + 回读校验 |
| 工具链 | GCC + CMake + OpenOCD + GDB (WSL2 环境) |
| 上位机 | Python (pyserial) OTA 上传工具 |

---

## FreeRTOS 任务设计

| 任务 | 优先级 | 功能 | 通信方式 |
|------|--------|------|---------|
| Task_UART | High | UART 协议解析 + OTA 接收 | sem_uart_rx (信号量) |
| Task_Light | AboveNormal | ADC 光照状态机 (DARK/DIM/IDEAL/GLARE) | sem_adc_ready (信号量) → queue_light |
| Task_MPU | Normal | MPU6050 姿态读取 | queue_mpu |
| Task_OLED | BelowNormal | OLED 显示更新 | queue_light + queue_mpu |
| Task_LED | Low | LED 状态指示 | queue_light |
| Task_SPIFlash | Low | SPI Flash 写入队列 | queue_flash |

**任务间通信架构：**
- **信号量**：ISR → Task 的事件通知（ADC 转换完成、UART 数据到达）
- **队列**：Task → Task 的数据传递（光照状态、MPU 数据、Flash 写请求）
- **环形缓冲区**：ISR → Task 的字节流传递（UART 数据，解决 ISR 上下文无法调用 FreeRTOS API 的问题）

---

## OTA 升级流程

```
┌─────────┐    ┌──────────┐    ┌────────────┐    ┌──────────┐
│ Python   │───→│  UART    │───→│ SPI Flash  │───→│内部 Flash│
│ 上位机   │    │  传输    │    │  暂存      │    │  搬运    │
│          │    │          │    │            │    │          │
│ START ───┤    │ 状态机   │    │ 擦除扇区   │    │ 校验CRC  │
│ DATA ×N ─┤    │ +CRC16   │    │ 页编程     │    │ 跳转App  │
│ END ─────┤    │ 环形缓冲 │    │ CRC32校验  │    │          │
│ ACK ←────┤    │          │    │ 写OTA标志  │    │          │
└─────────┘    └──────────┘    └────────────┘    └──────────┘
```

**协议格式：** `| 0xAA (包头) | CMD (命令) | LEN (长度) | DATA | CRC16 (校验) |`

- **CMD_OTA_START (0x01)**：携带固件大小 + CRC32，设备擦除 SPI Flash 扇区
- **CMD_OTA_DATA (0x02)**：64 字节固件数据，设备逐包写入 SPI Flash 并回读验证
- **CMD_OTA_END (0x03)**：设备回读整个固件计算 CRC32，与 START 中的 CRC32 对比

---

## Bootloader 设计

```
0x0800_0000 ┌────────────────────┐
            │ Bootloader (8KB)   │
            │ 读OTA标志          │
            │ SPI Flash CRC校验  │
            │ 搬运固件           │
            │ 跳转App (naked)    │
0x0800_2000 ├────────────────────┤
            │ App (54KB)         │
            │ FreeRTOS + 业务逻辑 │
0x0800_FBFF └────────────────────┘
```

**关键技术点：**
- `__attribute__((naked))` + 内联汇编实现跳转，避免编译器生成 push/pop 导致 HardFault
- 搬运固件后带**回读校验**（读内部 Flash 和 SPI Flash 对比），防止数据损坏
- SPI Flash ID 预检，通信异常时安全跳转到 App

---

## 调试方法论

本项目遇到了 **8 个高难度 Bug**，完整记录在 [BUGS.md](BUGS.md) 中。核心调试手段：

| 工具 | 用途 |
|------|------|
| `openocd -c "reg pc"` | 快速确认 MCU 状态（HardFault/正常） |
| `openocd -c "mdw <addr>"` | 读 Flash/RAM 验证数据 |
| Cortex-M3 异常帧分析 | 从 MSP 还原崩溃现场（LR/PC/xPSR） |
| `arm-none-eabi-addr2line` | PC 地址反查源码行号 |
| `arm-none-eabi-objdump -d` | 反汇编确认编译器生成的指令序列 |
| GDB 远程断点 | 单步跟踪 Bootloader 跳转过程 |
| 排除法 | GDB 手动设置寄存器隔离问题范围 |

---

## 构建与烧录

```bash
# 编译
cmake --preset Debug && cmake --build build/Debug

# 编译 Bootloader
cd bootloader && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../cmake/gcc-arm-none-eabi.cmake && cmake --build build

# 烧录 (Bootloader + App)
arm-none-eabi-objcopy -O binary bootloader/build/bootloader.elf /tmp/boot.bin
arm-none-eabi-objcopy -O binary build/Debug/gcctest.elf /tmp/app.bin
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "init; reset halt" \
  -c "flash erase_address 0x08000000 0x10000" \
  -c "flash write_image /tmp/boot.bin 0x08000000 bin" \
  -c "flash write_image /tmp/app.bin 0x08002000 bin" \
  -c "reset run; shutdown"

# OTA 上传
python3 ota_upload.py /dev/ttyUSB0 build/Debug/gcctest.bin 115200
```

---

## Flash 资源占用

| 区域 | 大小 | 占用 |
|------|------|------|
| Flash | 64KB | App 56.6% + Bootloader 81.9% |
| RAM | 20KB | 74.2%（FreeRTOS 堆 + 6 任务栈 + 全局变量） |
