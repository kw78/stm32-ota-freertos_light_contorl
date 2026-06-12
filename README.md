# STM32 智能光照系统 — FreeRTOS + OTA 远程升级

> 在 64KB Flash / 20KB RAM 的 STM32F103C8T6 上，实现了一个包含 6 个 FreeRTOS 任务、自定义 OTA 协议栈、SPI Flash 驱动、Bootloader 跳转机制的完整嵌入式系统。项目完整经历了"架构设计 → 编码实现 → 硬件调试 → Bug 修复"的全链路工程实践，解决了 8 个高难度技术问题。

---

## 项目亮点

### 1. 完整的 OTA 远程升级链路

```text
上位机 (Python) ──UART──→ App (FreeRTOS) ──SPI──→ W25D64 ──搬运──→ 内部 Flash
                                                         ↑
                                                    Bootloader
                                                    (naked ASM 跳转)
```

- **自定义协议**：`| 0xAA | CMD | LEN | DATA | CRC16 |`，支持 START/DATA/END 三种命令
- **双重 CRC 校验**：CRC16 包级校验（UART 传输）+ CRC32 固件级校验（SPI Flash 存储）
- **断电安全**：OTA 标志持久化到 SPI Flash，断电后 Bootloader 自动恢复
- **回读验证**：搬运固件后从内部 Flash 回读并 CRC32 校验，防止数据损坏

---

### 2. Cortex-M3 Bootloader 设计

```c
// 为什么必须用 __attribute__((naked))？
// 普通 C 函数编译器会自动插入 push/pop：
//   push {r4}     ← 压入 Bootloader 栈
//   msr MSP, r4   ← 栈指针改到 App 区域
//   pop  {r4}     ← 从 App 栈弹出 → 越界 → HardFault！
//
// naked 版本零 push/pop，MSM 修改后不会从栈上读写
static void __attribute__((naked)) jump_to_app(void){
    __asm volatile(
        "cpsid i              \n"
        "ldr r0, [%0]        \n"  // App SP
        "ldr r1, [%0, #4]    \n"  // App Reset_Handler
        "msr MSP, r0         \n"  // 设栈指针
        "movw r2, #0xED08     \n"
        "movt r2, #0xE000     \n"
        "str %0, [r2]        \n"  // VTOR = App 向量表
        "cpsie i              \n"
        "bx r1               \n"  // 跳转
        : : "r" (APP_START_ADDR) : "r0","r1","r2"
    );
}
```

这个 Bug 的调试过程极其复杂：**GDB 设断点时能跑、不设断点时崩溃**（经典的海森堡效应），最终通过反汇编分析编译器生成的 `push/pop` 指令序列定位根因。详见 [BUGS.md](BUGS.md) Bug #6。

### 3. FreeRTOS 多任务架构

6 个任务，4 种 IPC 机制，完整的生产者-消费者模型：

```text
ISR 上下文（不能调 FreeRTOS API）
  │
  ├─ ADC DMA 中断 ──binary_semaphore──→ Task_Light（光照状态机）
  │                                          ├─queue──→ Task_OLED（显示）
  │                                          └─queue──→ Task_LED（指示灯）
  │
  └─ UART RX 中断 ──ring_buffer──→ Task_UART（OTA 协议栈）
                                      └─queue──→ Task_SPIFlash（持久化）

任务上下文
  └─ Task_MPU（MPU6050 轮询）──queue──→ Task_OLED
```

**关键设计决策：**

- ADC 中断优先级设为 6（> `configMAX_SYSCALL_INTERRUPT_PRIORITY=5`），才能安全调用 `osSemaphoreRelease()`
- UART 回调中**禁止任何阻塞操作**（`HAL_UART_Transmit`、`snprintf`），否则导致 OTA 数据丢失
- 高优先级任务（Task_Light）必须有 `osDelay()` 让出 CPU，否则低优先级任务被饿死

### 4. SPI Flash 驱动 + 信号完整性调试

W25D64 驱动从零实现：CS 控制 → 命令发送 → 地址处理 → 数据读写 → BUSY 等待（500ms 超时）。

开发过程中发现**面包板 SPI 信号完整性问题**：DMA/ADC/I2C 全部运行时，GPIO 翻转产生的电磁噪声通过面包板寄生电容耦合到 MISO 线，导致 bit 翻转（每次读回值不同，只有 1 个 bit 差异）。改为杜邦线直连 + 100nF 去耦电容后解决。

### 5. 工程实践：从 Keil 到现代工具链

| 维度 | 传统 Keil | 本项目 |
|------|----------|--------|
| 构建系统 | .uvprojx（二进制） | CMake + Ninja（声明式，可 diff） |
| 编译器 | ARMCC | GCC ARM（`-Os -flto --gc-sections`） |
| 调试 | Keil 内置调试器 | OpenOCD + GDB（命令行，可脚本化） |
| 版本管理 | 无法有效 git diff | 全文件文本化，完整 git 历史 |
| 平台 | Windows only | WSL2 Linux（VS Code 远程开发） |

---

## 系统架构

```text
┌─────────────────────────────────────────────────────────────────┐
│                    上位机 (Python)                               │
│  ota_upload.py ── 读 .bin → CRC32 → 分包(64B) → 等 ACK         │
└───────────────────────┬─────────────────────────────────────────┘
                        │ UART 115200
┌───────────────────────▼─────────────────────────────────────────┐
│               STM32F103C8T6 (App, 0x0800_2000)                  │
│  ┌──────────┐  sem    ┌──────────┐  queue   ┌──────────┐       │
│  │Task_Light│────────→│Task_OLED │←────────│Task_MPU  │       │
│  │ADC+状态机│         │显示更新   │         │MPU6050   │       │
│  └──────────┘         └──────────┘         └──────────┘       │
│  ┌──────────┐  ringbuf ┌──────────┐                           │
│  │Task_UART │←────────│UART RX   │  sem_uart_rx               │
│  │OTA协议栈 │          │ISR→RingBuf│                           │
│  └──────────┘          └──────────┘                           │
│  ┌──────────┐  queue   ┌──────────┐                           │
│  │Task_SPI  │←────────│Flash请求 │  → W25D64                 │
│  └──────────┘          └──────────┘                           │
│  Task_LED ── LED 指示灯                                        │
│  FreeRTOS 内核：抢占式 | SysTick | 6 任务 | heap_4             │
└─────────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────────┐
│          Bootloader (0x0800_0000, 8KB)                          │
│  SPI Flash ID 预检 → OTA 标志读取 → CRC32 校验 → 回读验证      │
│  → naked ASM 跳转（无 push/pop，安全切换栈指针）                │
└─────────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────────┐
│          W25D64 SPI Flash (8MB)                                 │
│  OTA 标志(4KB) | 固件暂存(56KB) | 配置参数 | 日志              │
└─────────────────────────────────────────────────────────────────┘
```

---

## 技术栈

| 层次 | 技术 | 关键约束 |
|------|------|---------|
| MCU | STM32F103C8T6 (Cortex-M3, 64MHz) | 64KB Flash, 20KB RAM |
| RTOS | FreeRTOS CMSIS-RTOS V2 | 6 任务, 抢占式, SysTick 驱动 |
| 外设 | ADC + I2C + SPI + UART + DMA | 全部中断驱动, DMA 循环模式 |
| 存储 | W25D64 SPI Flash 8MB | 16MHz SPI, 4KB 扇区擦除 |
| OTA | 自定义协议 + CRC16/CRC32 | 环形缓冲 + 状态机 |
| Bootloader | 裸机 C + naked ASM | 8KB 约束, 跳转 App |
| 工具链 | GCC + CMake + OpenOCD + GDB | WSL2, 交叉编译 |
| 上位机 | Python (pyserial) | OTA 分包上传 |

---

## FreeRTOS 任务设计

| 任务 | 优先级 | 功能 | IPC |
|------|--------|------|-----|
| Task_UART | High | OTA 协议解析 + UART 收发 | sem_uart_rx → queue_flash |
| Task_Light | AboveNormal | ADC 光照状态机 (DARK/DIM/IDEAL/GLARE) | sem_adc_ready → queue_light |
| Task_MPU | Normal | MPU6050 三轴加速度 + 姿态角 | 定时轮询 → queue_mpu |
| Task_OLED | BelowNormal | OLED 显示 | queue_light + queue_mpu |
| Task_LED | Low | LED 状态指示 | queue_light |
| Task_SPIFlash | Low | SPI Flash 写入队列 | queue_flash |

**IPC 选型理由：**

- **信号量**：ISR → Task 事件通知（ADC 转换完成、UART 数据到达），二进制信号量天然去重
- **队列**：Task → Task 数据传递，内核拷贝，无竞争
- **环形缓冲区**：ISR → Task 字节流（UART 数据），绕过 ISR 中不能调 FreeRTOS API 的限制

---

## OTA 协议

```text
包格式: | 0xAA | CMD | LEN | DATA(0-64B) | CRC16 |

CMD_OTA_START (0x01): 固件大小(4B) + CRC32(4B) → 设备擦除 SPI Flash
CMD_OTA_DATA  (0x02): 固件数据(≤64B)          → 设备写入 SPI Flash
CMD_OTA_END   (0x03): 无数据                   → 设备校验 + 设置标志 + 复位
```

**双重校验：**

- CRC16（每包）：保证 UART 传输无损
- CRC32（整体）：保证 SPI Flash 存储正确

---

## Bootloader 流程

```text
上电 → SPI Flash ID 预检（失败直接跳 App）
  → 读 OTA 标志
  → CRC32 校验固件
  → SP 合法性检查（0x20000000~0x20005000）
  → 搬运固件（带回读校验）
  → 清除标志
  → naked ASM 跳转（设置 MSP + VTOR + bx）
```

---

## 资源占用

| 区域 | 大小 | 已用 | 占比 |
|------|------|------|------|
| Flash (App) | 54KB | 37KB | 67.8% |
| Flash (Bootloader) | 8KB | 6.2KB | 75.7% |
| RAM | 20KB | 15.2KB | 74.2% |

---

## 构建与烧录

```bash
# 编译
cmake --preset Debug && cmake --build build/Debug
cd bootloader && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../cmake/gcc-arm-none-eabi.cmake && cmake --build build && cd ..

# 烧录（必须用 objcopy + write_image，不能用 openocd program）
arm-none-eabi-objcopy -O binary bootloader/build/bootloader.elf /tmp/boot.bin
arm-none-eabi-objcopy -O binary build/Debug/gcctest.elf /tmp/app.bin
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "init; reset halt" \
  -c "flash erase_address 0x08000000 0x10000" \
  -c "flash write_image /tmp/boot.bin 0x08000000 bin" \
  -c "flash write_image /tmp/app.bin 0x08002000 bin" \
  -c "reset run; shutdown"

# OTA 上传
sudo python3 tools/ota_upload.py /dev/ttyUSB0 build/Debug/gcctest.bin 115200
```

---

## 遇到的技术难题

共 8 个高难度 Bug，完整调试过程记录在 [BUGS.md](BUGS.md)：

| # | 问题 | 根因 | 修复 |
|---|------|------|------|
| 1 | SPI Flash 读 ID 返回 0 | uint32_t 小端序导致字节顺序反 | 用 uint8_t 数组 |
| 2 | HAL_Delay 卡死 | FreeRTOS 和 HAL 抢 SysTick | HAL 改用 TIM4 |
| 3 | DMA 回调调 FreeRTOS API 后 HardFault | 中断优先级 ≤ 5 禁调 FreeRTOS API | DMA 优先级改 6 |
| 4 | OTA 32KB 传输 CRC 不匹配 | UART 回调中 HAL_UART_Transmit 阻塞导致字节丢失 | ISR 中移除阻塞操作 |
| 5 | ELF 烧录覆盖 Bootloader | LOAD 段 PhysAddr 对齐导致数据错位 | 用 objcopy + write_image |
| 6 | Bootloader 跳转 App 后 HardFault | C 函数 push/pop 在 MSP 修改后导致栈越界 | naked + 内联汇编 |
| 7 | SPI 读回值每次不同（bit 翻转） | 面包板信号完整性（寄生电容串扰） | 杜邦线直连 + 去耦电容 |
| 8 | Task_OLED/Task_LED 不执行 | 高优先级 Task_Light 缺 osDelay 饿死低优先级任务 | 加 osDelay(10) |
