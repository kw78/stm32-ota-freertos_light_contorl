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
│  SPI Flash ID 预检 → 读 OTA 标志 → CRC32 校验                  │
│  → 搬运固件（带回读验证）→ 跳转 App (naked ASM)                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│          W25D64 SPI Flash (8MB)                                 │
│  OTA 标志区 (4KB) | 固件暂存区 (56KB) | 配置参数区 | 日志区      │
└─────────────────────────────────────────────────────────────────┘
```

---

## 技术栈

| 层次 | 技术 | 说明 |
|------|------|------|
| MCU | STM32F103C8T6 | Cortex-M3, 64MHz, 64KB Flash, 20KB RAM |
| RTOS | FreeRTOS CMSIS-RTOS V2 | 6 任务, 抢占式调度, SysTick 驱动 |
| 外设 | ADC + I2C + SPI + UART + DMA | 全部中断驱动 |
| 传感器 | BH1750 光照 + MPU6050 姿态 | I2C 总线 |
| 显示 | SSD1306 OLED 128x64 | I2C 驱动 |
| 存储 | W25D64 SPI Flash 8MB | 持久化 + OTA 固件暂存 |
| OTA | 自定义协议 + CRC16/CRC32 | 端到端校验 |
| Bootloader | 裸机 C + naked ASM | Cortex-M 跳转 + 回读验证 |
| 工具链 | GCC + CMake + OpenOCD + GDB | WSL2 + VS Code |
| 上位机 | Python pyserial | OTA 上传脚本 |

---

## FreeRTOS 任务设计

### 任务优先级与功能

| 任务 | 优先级 | 功能 | 数据来源 | 数据去向 |
|------|--------|------|---------|---------|
| Task_UART | High | OTA 协议解析 + UART 收发 | sem_uart_rx | queue_flash |
| Task_Light | AboveNormal | ADC 光照状态机 (DARK/DIM/IDEAL/GLARE) | sem_adc_ready | queue_light |
| Task_MPU | Normal | MPU6050 三轴加速度 + 姿态角 | 定时轮询 | queue_mpu |
| Task_OLED | BelowNormal | OLED 显示（光照状态 + MPU 数据） | queue_light + queue_mpu | OLED 硬件 |
| Task_LED | Low | LED 状态指示灯 | queue_light | GPIO |
| Task_SPIFlash | Low | SPI Flash 写入队列 | queue_flash | W25D64 |

### 任务间通信架构

```
ISR 上下文                    任务上下文
──────────                    ──────────
ADC DMA 中断 ──semaphore──→ Task_Light
                              │
                              ├─queue_light──→ Task_OLED
                              │              └─→ Task_LED
                              │
UART RX 中断 ──ringbuf──→ Task_UART
                              ├─queue_flash──→ Task_SPIFlash
                              │
MPU6050 轮询 ──queue_mpu──→ Task_OLED
```

**通信机制选型理由：**
- **信号量**（`sem_adc_ready`, `sem_uart_rx`）：ISR → Task 的事件通知，只传递"有数据了"的信号，不携带数据。二进制信号量天然去重——如果 ISR 连续释放多次，只有一次有效，避免任务被重复唤醒。
- **队列**（`queue_light`, `queue_mpu`, `queue_flash`）：Task → Task 的数据传递，内核拷贝数据到队列内部缓冲区，生产者和消费者各操作独立副本，天然无竞争。
- **环形缓冲区**：ISR → Task 的字节流传递。ISR 上下文中不能调用 FreeRTOS API（中断优先级 ≤ `configMAX_SYSCALL_INTERRUPT_PRIORITY` 时），所以用无锁环形缓冲区在 ISR 中暂存字节，任务中批量读取。

---

## W25D64 SPI Flash 驱动

### 驱动架构

```c
// 层级 1：硬件操作（SPI 收发 + CS 控制）
CS_Low() / CS_High()
HAL_SPI_Transmit() / HAL_SPI_Receive()

// 层级 2：Flash 协议（命令 + 地址 + 数据）
W25_ReadID()          // 读 JEDEC ID（验证芯片连接）
W25_Read()            // 读数据（任意地址，任意长度）
W25_WritePage()       // 页编程（≤256字节，自动拆页）
W25_EraseSector()     // 扇区擦除（4KB，地址自动对齐）

// 层级 3：内部辅助（带超时保护）
W25_WaitBusy()        // 轮询 BUSY 位，500ms 超时
W25_CMD_WRITE_ENABLE  // 写使能（每次写/擦前必须调用）
```

### 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| SPI 时序 | Mode 0 (CPOL=LOW, CPHA=1EDGE) | W25D64 兼容 |
| 波特率 | 16MHz (分频 4) | 兼顾速度和稳定性 |
| BUSY 等待 | 轮询 + 500ms 超时 | 防止 SPI 通信异常时死循环 |
| CS 控制 | 软件 GPIO | 不依赖 SPI NSS 硬件 |
| 写使能 | 每次写/擦前发送 0x06 | Flash 协议要求 |

---

## OTA 升级流程

### 协议格式

```
| 0xAA (包头, 1B) | CMD (命令, 1B) | LEN (数据长度, 1B) | DATA (0-64B) | CRC16 (校验, 2B) |
```

### 升级时序

```
上位机                    设备                         SPI Flash
  │                        │                              │
  │── START (fw_size,     │                              │
  │    fw_crc32) ────────→│                              │
  │                        │── 擦除扇区 ────────────────→│
  │←── ACK ───────────────│                              │
  │                        │                              │
  │── DATA (64B) ────────→│── 页编程 ──────────────────→│
  │←── ACK ───────────────│                              │
  │── DATA (64B) ────────→│── 页编程 ──────────────────→│
  │←── ACK ───────────────│                              │
  │   ...重复 508 次...     │                              │
  │                        │                              │
  │── END ────────────────→│                              │
  │                        │── 回读固件 ────────────────→│
  │                        │── 计算 CRC32               │
  │                        │── 对比 START 中的 CRC32     │
  │                        │── 写 OTA 标志 (PENDING) ──→│
  │←── ACK ───────────────│                              │
  │                        │── 复位 (NVIC_SystemReset)   │
  │                        │                              │
  │                        ├── Bootloader 启动            │
  │                        │── 读 OTA 标志 → PENDING      │
  │                        │── 搬运固件到内部 Flash ─────→│
  │                        │── 清除 OTA 标志              │
  │                        │── 跳转 App                  │
```

### 双重 CRC 校验策略

| 校验点 | 算法 | 范围 | 时机 |
|--------|------|------|------|
| 包级校验 | CRC16 | 每个 OTA 包 (CMD+LEN+DATA) | 每包传输时 |
| 固件级校验 | CRC32 | 整个固件二进制数据 | START 包中发送，END 时回读验证 |

**为什么需要两层校验？**
- CRC16 保证每个包在传输过程中没有损坏（UART 噪声、位翻转）
- CRC32 保证整个固件在 SPI Flash 中存储正确（SPI 通信异常、写入失败）
- 两层校验的碰撞概率：CRC16 约 1/65536 × 508包 = 极低；CRC32 约 1/42亿 = 可忽略

---

## Bootloader 设计

### 内存布局

```
Internal Flash (64KB):
┌─────────────────────┐ 0x0800_0000
│  Bootloader (8KB)   │
│  - SPI Flash 驱动   │
│  - OTA 标志读取      │
│  - 固件搬运+回读校验 │
│  - naked 跳转        │
├─────────────────────┤ 0x0800_2000
│  App (54KB)          │
│  - FreeRTOS + 任务   │
│  - OTA 接收协议栈    │
│  - 业务逻辑          │
├─────────────────────┤ 0x0800_FC00
│  保留 (1KB)          │
└─────────────────────┘ 0x0800_FFFF

SPI Flash (8MB):
┌─────────────────────┐ 0x0000_0000
│  OTA 标志 (4KB)      │  magic + state + fw_size + fw_crc32
├─────────────────────┤ 0x0000_1000
│  固件暂存 (56KB)     │  OTA 接收时写入，Bootloader 读取搬运
├─────────────────────┤ 0x0000_F000
│  配置参数 (4KB)      │
├─────────────────────┤ 0x0001_0000
│  日志 + 扩展 (7.5MB) │
└─────────────────────┘ 0x0080_0000
```

### Bootloader 启动流程

```
上电 → Reset_Handler (0x0800_0000)
  │
  ├─ HAL_Init() → SysTick 配置
  ├─ SystemClock_Config() → 64MHz HSI+PLL
  ├─ GPIO 初始化 → CS 引脚
  ├─ SPI2 初始化 → 16MHz
  │
  ├─ W25_ReadID() → 验证 SPI Flash 连接
  │   └─ 失败 → 直接跳转 App
  │
  ├─ W25_Read(OTA_FLAG_ADDR) → 读 OTA 标志
  │   ├─ flag.magic != OTA_MAGIC → 跳转 App
  │   ├─ flag.state != PENDING → 跳转 App
  │   └─ PENDING → 继续
  │
  ├─ CRC32 校验 → 从 SPI Flash 读取固件计算 CRC32
  │   └─ 不匹配 → 清除标志，跳转 App
  │
  ├─ SP 合法性检查 → 栈顶地址在 0x20000000~0x20005000 范围内
  │   └─ 不合法 → 清除标志，跳转 App
  │
  ├─ 搬运固件 → SPI Flash → 内部 Flash（带回读校验）
  │   └─ 校验失败 → 标志不清除，下次重启重试
  │
  ├─ 清除 OTA 标志 → W25_EraseSector + W25_WritePage
  │
  └─ jump_to_app() → naked ASM 跳转
      ├─ 关中断
      ├─ 设置 MSP（App 栈顶）
      ├─ 设置 VTOR（App 向量表）
      ├─ 开中断
      └─ bx r1 → 跳转到 App Reset_Handler
```

### jump_to_app 的关键技术

```c
// 为什么必须用 naked？普通 C 函数会生成 push/pop：
//   push {r4}        → 压入 Bootloader 的栈
//   msr MSP, r4      → 栈指针改为 App 栈
//   pop  {r4}        → 从 App 栈弹出 → 越界 → HardFault！
//
// naked 版本没有任何 push/pop，MSM 修改后不会从栈上读写：
static void __attribute__((naked)) jump_to_app(void){
    __asm volatile(
        "cpsid i              \n"  // 关中断
        "ldr r0, [%0]        \n"  // r0 = App SP
        "ldr r1, [%0, #4]    \n"  // r1 = App Reset_Handler
        "msr MSP, r0         \n"  // 设栈指针
        "movw r2, #0xED08     \n"
        "movt r2, #0xE000     \n"
        "str %0, [r2]        \n"  // VTOR = App
        "cpsie i              \n"  // 开中断
        "bx r1               \n"  // 跳转
        : : "r" (APP_START_ADDR) : "r0","r1","r2"
    );
}
```

---

## 调试方法论

本项目在开发过程中遇到了 **8 个高难度 Bug**，完整调试记录在 [BUGS.md](BUGS.md) 中。以下是调试方法的系统化总结：

### 调试工具链

| 工具 | 命令/操作 | 解决的问题 |
|------|----------|-----------|
| OpenOCD 寄存器读取 | `openocd -c "reg pc"` | 5 秒内确认 MCU 状态（HardFault/正常运行/卡死） |
| OpenOCD 内存读取 | `openocd -c "mdw 0x08002000 4"` | 验证 Flash 内容、向量表、异常帧 |
| 异常帧分析 | `openocd -c "mdw <MSP> 8"` | 从栈上读取 {R0-R3,R12,LR,PC,xPSR} 还原崩溃现场 |
| addr2line | `arm-none-eabi-addr2line -e elf 0x08003463` | 把 PC/LR 地址翻译为源码文件+行号 |
| objdump | `arm-none-eabi-objdump -d elf \| grep jump_to_app` | 反汇编关键函数，确认编译器生成的指令序列 |
| readelf | `arm-none-eabi-readelf -l elf` | 检查 LOAD 段地址和对齐 |
| GDB 远程调试 | `gdb-multiarch elf -ex "target remote :3333"` | 单步跟踪、断点、寄存器检查 |
| 排除法 | OpenOCD 手动设置 VTOR/SP/PC | 隔离问题：Bootloader 跳转 vs App 代码 |

### 调试思维模型

```
1. 缩小范围 → 问题在 Bootloader 还是 App？
   └── 用 GDB 手动跳转 App → App 正常 → 问题在 Bootloader

2. 定位崩溃点 → 异常帧分析
   └── 读 MSP 处的 8 个 word → LR 指向 __libc_init_array
   └── addr2line 确认源码位置

3. 排除数据损坏 → 读 Flash 内容对比 ELF
   └── 数据正确 → 问题在运行时行为，不是存储

4. 分析运行时行为 → 反汇编关键函数
   └── 发现 push/pop 在 MSP 修改后导致栈越界

5. 验证修复 → 编译后反汇编确认指令序列
   └── naked 版本无 push/pop → 修复确认
```

---

## Flash 资源占用

| 区域 | 大小 | 已用 | 占比 | 说明 |
|------|------|------|------|------|
| Flash (App) | 54KB | 37KB | 67.8% | FreeRTOS + 业务逻辑 + OTA 协议栈 + SPI Flash 驱动 |
| Flash (Bootloader) | 8KB | 6.2KB | 75.7% | SPI 驱动 + OTA 校验 + 搬运 + naked 跳转 |
| RAM | 20KB | 15.2KB | 74.2% | FreeRTOS 堆(8KB) + 6 任务栈(~3KB) + 全局变量(~4KB) |

---

## 构建与烧录

```bash
# === 编译 App ===
cmake --preset Debug
cmake --build build/Debug

# === 编译 Bootloader ===
cd bootloader
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../cmake/gcc-arm-none-eabi.cmake
cmake --build build
cd ..

# === 烧录（Bootloader + App）===
arm-none-eabi-objcopy -O binary bootloader/build/bootloader.elf /tmp/boot.bin
arm-none-eabi-objcopy -O binary build/Debug/gcctest.elf /tmp/app.bin

openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "init; reset halt" \
  -c "flash erase_address 0x08000000 0x10000" \
  -c "flash write_image /tmp/boot.bin 0x08000000 bin" \
  -c "flash write_image /tmp/app.bin 0x08002000 bin" \
  -c "reset run; shutdown"

# === OTA 上传 ===
sudo python3 tools/ota_upload.py /dev/ttyUSB0 build/Debug/gcctest.bin 115200
```

**注意：** 必须用 `objcopy -O binary` + `flash write_image` 烧录，不能用 `openocd program`（ELF 的 LOAD 段 PhysAddr 对齐问题会覆盖 Bootloader）。
