# OTA 系统开发中的高难度 Bug 与调试记录

> 本文记录了在 STM32F103 + FreeRTOS + W25D64 SPI Flash 实现 OTA 升级系统过程中遇到的核心技术难点。每个 Bug 都完整记录了 **现象 → 排查思路 → 尝试手段 → 根因分析 → 最终修复** 的全过程，体现了从"不知道问题在哪"到"精确定位并修复"的完整调试方法论。

---

## Bug #1：SPI 字节序陷阱 — 串行通信的隐性契约

### 现象

W25D64 SPI Flash 驱动写完后，读 JEDEC ID 返回 `0x000000`。SPI 线路物理连接正确，示波器能看到时钟和 MOSI 波形，但 Flash 完全无响应。

### 调试思路

1. **排除硬件问题**：用 ST-Link SWD 读取 SPI2 外设寄存器 → CR1 正确（MSTR=1, SPE=1），外设已使能
2. **最小化测试**：写一个不依赖 HAL 库的裸寄存器 SPI 发送，发送 `0x9F`（Read JEDEC ID）→ 仍然返回 0
3. **对比参考实现**：查阅 W25D64 数据手册的时序图，发现协议要求**命令字节在先，地址字节在后**，而我的代码把命令和地址拼成了一个 `uint32_t` 后直接发送

### 根因

```c
// 我的写法
uint32_t cmd = 0x03001234;  // 命令 0x03 + 地址 0x001234
HAL_SPI_Transmit(&hspi2, &cmd, 4, 100);
```

ARM Cortex-M3 是**小端序**（Little-Endian），`uint32_t` 在内存中低字节在前。CPU 从 `&cmd` 开始按字节发送，实际发出的顺序是 `0x34, 0x12, 0x00, 0x03` —— 命令字节 `0x03` 跑到了最后一位，Flash 收到的第一个字节是 `0x34`（不是任何有效命令），所以完全无法通信。

### 修复

```c
uint8_t cmd[4] = {
    0x03,                  // 命令（字节数组天然按顺序存储）
    (addr >> 16) & 0xFF,  // 地址高字节
    (addr >>  8) & 0xFF,  // 地址中字节
    (addr      ) & 0xFF   // 地址低字节
};
HAL_SPI_Transmit(&hspi2, cmd, 4, 100);
```

### 教训

> SPI/I2C/UART 等串行协议都是按字节流发送的。在内存中，`uint32_t` 的字节排列由 CPU 的端序决定，而串行协议要求的是**逻辑顺序**。两者不一定一致。永远用 `uint8_t` 数组来控制字节发送顺序。

---

## Bug #2：FreeRTOS 与 HAL 抢占 SysTick

### 现象

集成 FreeRTOS 后，`HAL_Delay()` 卡死，LED 不闪，系统完全无响应。但 FreeRTOS 的任务调度似乎在运行（idle task 在跑）。

### 调试思路

1. **GDB 断点**：在 `HAL_Delay` 内部打断点 → 发现 `HAL_GetTick()` 永远返回 0，超时永远不触发
2. **查 HAL 时基配置**：`stm32f1xx_hal_conf.h` 中 `TICK_INT_PRIORITY = 15`，HAL 使用 SysTick
3. **查 FreeRTOS port**：`port.c` 中 `xPortSysTickHandler` 也绑定了 SysTick
4. **确认冲突**：两个系统都试图配置 SysTick，互相覆盖

### 根因

SysTick 是 ARM Cortex-M3 内置的 24 位向下计数器，只有一个。FreeRTOS 的 `vPortSetupTimerInterrupt()` 用它产生 1ms 的 tick 中断来驱动任务调度。HAL 的 `HAL_InitTick()` 也用它来提供 `HAL_GetTick()` 计时。两者同时配置同一个硬件资源，互相覆盖配置。

### 修复

在 CubeMX 中将 **SYS → Timebase Source** 从 SysTick 改为 **TIM4**。这样：
- SysTick 完全交给 FreeRTOS
- HAL 用 TIM4 作为时基，`HAL_GetTick()` 和 `HAL_Delay()` 正常工作
- 两者各用独立定时器，互不干扰

### 教训

> STM32 + FreeRTOS + HAL 三件套的项目，**第一步**永远是改 HAL Timebase。这必须在添加 FreeRTOS 之前完成，否则 CubeMX 生成代码时会产生配置冲突。Cortex-M3 只有一个 SysTick，不能被两个系统共享。

---

## Bug #3：DMA 中断优先级边界违规 — FreeRTOS 的隐性约束

### 现象

在 ADC DMA 完成回调中调用 `osSemaphoreRelease()` 后，系统偶发 HardFault 或任务不再被唤醒。

### 调试思路

1. **GDB 检查**：HardFault 时 PC 指向 `HardFault_Handler`，调用栈中出现 `pxQueueSend` 内部的 assert
2. **查阅 FreeRTOS 文档**：发现 Cortex-M3 port 有一个 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 参数（默认 5）
3. **理解规则**：优先级数值 **≤** 5 的中断中，**禁止**调用任何 FreeRTOS API（包括 `FromISR` 版本）
4. **检查当前配置**：DMA1_Channel1 中断优先级 = 5（CubeMX 默认值），恰好踩在边界上

### 根因

FreeRTOS 的 Cortex-M3 port 内部有临界区保护机制。当优先级 ≤ `configMAX_SYSCALL_INTERRUPT_PRIORITY` 的中断调用 `osXxxFromISR()` 时，可能打断 FreeRTOS 的内部数据结构操作，导致队列/信号量状态不一致。这不是 bug，是 FreeRTOS 的**设计约束**。

### 修复

在 CubeMX → NVIC 中将 DMA 中断优先级从 5 改为 **6**。数值越大优先级越低，6 > 5，满足 FreeRTOS 的要求。

### 教训

> FreeRTOS 在 Cortex-M 上的中断优先级规则：**只有优先级数值 > `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` 的中断，才能安全调用 FreeRTOS API**。这个阈值是 `FreeRTOSConfig.h` 中的编译时常量，默认值 5。CubeMX 默认给 DMA 分配优先级 5，刚好踩线。

---

## Bug #4：UART 回调中的阻塞操作导致 OTA 数据丢失

### 现象

OTA 上传 100 字节测试固件成功，但上传 32KB 真实固件时，END 包 CRC32 校验失败。设备计算的 CRC (`0x2CFC43E4`) 与上位机发送的 CRC (`0xD866307C`) 完全不匹配。

### 调试思路

1. **验证上位机正确性**：用 Python 直接计算 `binascii.crc32(fw_data)` → 与脚本输出一致
2. **缩小范围**：在设备端 `OTA_HandlePacket` 的 `CMD_OTA_DATA` 处加回读验证（写入后立即读回并 `memcmp`）→ 第 0 包就失败（`w=005000 r=000000`）
3. **排除 Flash 问题**：SPI Flash 写入验证用的是同一个 `W25_WritePage` + `W25_Read`，如果 Flash 有硬件问题，Bootloader 阶段的 OTA 标志读取也会失败。但 Bootloader 能正常读取 OTA 标志 → Flash 正常
4. **锁定 UART 传输**：在 UART 回调中逐字节测试 → 发现 `HAL_UART_Transmit` 发送调试字符串（~0.5ms）期间，新的 UART 字节到达但无法被接收（回调中 RXNE 中断被屏蔽），字节丢失

### 根因

`HAL_UARTEx_RxEventCallback` 中有一行调试输出：
```c
HAL_UART_Transmit(&huart1, (uint8_t *)dbg, n, 100);  // 阻塞 ~0.5ms
```

在 ISR 中执行阻塞的 `HAL_UART_Transmit` 期间，UART 的 RXNE 中断被屏蔽（因为已经在 ISR 上下文中）。此时到达的字节无法被读取，UART 硬件只有 1 字节接收寄存器，后续字节覆盖前一个字节 → 数据丢失。

100 字节测试固件只发 2 个 DATA 包，间隔较长，回调有足够时间完成。32KB 固件发 508 个 DATA 包，包间间隔极短，回调中的阻塞导致大量字节丢失。

### 修复

移除 UART 回调中所有阻塞操作（`HAL_UART_Transmit`、`snprintf`）。ISR 中只做最小操作：写入环形缓冲区 + 释放信号量。

### 教训

> **ISR 黄金法则：ISR 中不做任何阻塞操作。** `HAL_UART_Transmit` 是阻塞的（轮询等待 TXE），在 ISR 中调用会阻塞 CPU 直到发送完成。这不仅导致自身延迟，还会屏蔽同优先级和低优先级的中断，造成字节丢失。正确做法：ISR 只做"放数据到缓冲区 + 通知任务"，耗时操作交给任务上下文。

---

## Bug #5：ELF LOAD 段地址对齐导致固件烧录错位

### 现象

OpenOCD 的 `program` 命令烧录 App ELF 后，Bootloader 读到的 App 向量表是 `0x08000000` 区域的数据（ELF 文件头），而不是 App 代码。

### 调试思路

1. **读取 Flash 内容**：`openocd -c "mdw 0x08002000 2"` → 显示 `464c457f 00010101`（ELF 魔数 `\x7FELF`）
2. **分析 ELF 段布局**：`arm-none-eabi-readelf -l gcctest.elf` → LOAD 段 `PhysAddr=0x08000000`，但代码段 `.isr_vector` 的 VMA 是 `0x08002000`
3. **根因定位**：OpenOCD 的 `program` 命令使用 LOAD 段的 `PhysAddr`（0x08000000）作为烧录目标地址，把 ELF 文件头数据写到了 0x08000000，覆盖了 Bootloader

### 根因

GCC 链接器生成的 ELF 文件中，LOAD 段的 `PhysAddr` 由于地址对齐要求（`Align=0x10000`）被向下取整到 0x08000000，而实际代码从 0x08002000 开始。OpenOCD 的 `program` 命令信任这个 `PhysAddr`，将整个 LOAD 段（包括 ELF 头部填充）写入错误的地址。

### 修复

用 `objcopy` 先将 ELF 转为原始 bin 文件（按实际 section 地址排列），再用 `flash write_image` 指定正确的烧录地址：

```bash
arm-none-eabi-objcopy -O binary bootloader.elf /tmp/boot.bin
arm-none-eabi-objcopy -O binary gcctest.elf    /tmp/app.bin
openocd -c "flash write_image /tmp/boot.bin 0x08000000 bin" \
         -c "flash write_image /tmp/app.bin  0x08002000 bin"
```

`objcopy -O binary` 按 section 的 VMA 地址排列数据，`flash write_image` 的 `bin` 格式直接从指定地址开始写入，绕过了 ELF LOAD 段的 PhysAddr 问题。

### 教训

> `arm-none-eabi-objcopy -O binary` 生成的 bin 文件是按**段地址（VMA）**排列的，而 `openocd program` 使用的是 LOAD 段的 **PhysAddr**。当两者不一致时（通常因为链接器的地址对齐），必须先 `objcopy` 再 `flash write_image`，这是 STM32 裸机开发中烧录多分区固件的标准做法。

---

## Bug #6：Bootloader 的 `jump_to_app` 中编译器生成的 push/pop 导致 HardFault

### 现象

OTA 上传成功，设备复位，Bootloader 搬运固件成功（Flash 内容验证正确），但跳转到 App 后崩溃。GDB 显示 `HardFault_Handler`，调用栈中 `__libc_init_array` → `blx r3` 跳转到垃圾地址 `0xd58aebde`。

### 调试思路（最复杂的调试过程）

**第一轮：排除法**

1. **单独烧录 App 到 0x08002000**（不用 Bootloader）→ 也不跑，排除了 Bootloader 搬运问题
2. **但 GDB 手动设置 VTOR/SP/PC 后 App 正常运行** → App 代码本身没问题
3. **结论：App 在 0x08002000 能跑，但 CPU 从 reset 启动时无法自动到达 App**

**第二轮：异常帧分析**

Cortex-M3 发生异常时硬件自动压栈 `{R0,R1,R2,R3,R12,LR,PC,xPSR}` 到 MSP。读 MSP 指向的内存：

```
0x20004fe4: 20000064 00000000 2000355c 00000000
            R0       R1       R2       R3
0x20004fec: 40021000 08003463 d58aebde d5aaebdf
            R12      LR        PC        xPSR
```

- **LR = `0x08003463`** → `addr2line` 定位到 `__libc_init_array` 内部
- **PC = `0xd58aebde`** → 垃圾地址（不在任何合法内存范围内）
- 崩溃点确认：`__libc_init_array` 调用 `.init_array` 中的函数指针时，函数指针地址是垃圾值

**第三轮：验证 Flash 内容**

```
openocd -c "mdw 0x0800b1f8 2"  →  08002179 08002151   ← Flash 中的数据
arm-none-eabi-objdump -s -j .init_array gcctest.elf     ← ELF 中的数据
```

Flash 内容完全正确。**问题不在 Flash 数据，而在运行时行为。**

**第四轮：排除 App 自身问题**

用 OpenOCD 手动跳转（`mww VTOR` + `reg sp` + `reg pc` + `resume`）→ App 正常运行到 `HAL_Delay`。**确认 App 代码无误，问题出在 Bootloader 的跳转过程。**

**第五轮：GDB 逐步跟踪跳转过程**

在 Bootloader 的 `jump_to_app` 末尾（`bx r0`）设断点，检查跳转前的寄存器状态：
- `r0 = 0x0800a19d`（App Reset_Handler，正确）
- `r3 = 0x08002000`（APP_START_ADDR，正确）
- `VTOR = 0x08002000`（正确）

一切正确，但 `continue` 后崩溃。

**第六轮：反汇编发现根因**

```bash
arm-none-eabi-objdump -d bootloader.elf | grep -A 20 "jump_to_app"
```

```asm
push   {r4}            ; ← 保存 r4 到 Bootloader 的栈（MSP 指向旧栈）
...
msr    MSP, r4         ; ← MSP 改为 0x20005000（App 栈顶）
...
pop    {r4}            ; ← 从新 MSP（0x20005000）弹出！
bx     r0              ; ← 跳转
```

**根因：** `push {r4}` 把 r4 压入 Bootloader 的栈（MSP = 0x20004xxx）。然后 `msr MSP` 把栈指针改成 0x20005000。`pop {r4}` 从 0x20005000 弹出——这个地址恰好在 STM32F103C8 的 20KB RAM 边界上（RAM = 0x20000000~0x20004FFF），读取无效地址触发 BusFault → HardFault。

**为什么 GDB 设断点时能跑过去？** 硬件断点会暂停 CPU、恢复执行，这个过程改变了 Flash/SPI 的访问时序和中断 pending 状态，恰好避开了崩溃路径。经典的**海森堡效应（Heisenbug）**——观测行为改变了被观测系统。

### 修复

用 `__attribute__((naked))` 告诉编译器不要生成任何函数序言/尾声（push/pop），然后用纯内联汇编完成跳转：

```c
static void __attribute__((naked)) jump_to_app(void){
    __asm volatile(
        "cpsid i                    \n"  // 关中断
        "ldr r0, [%0]              \n"  // r0 = App SP
        "ldr r1, [%0, #4]          \n"  // r1 = App 入口
        "msr MSP, r0               \n"  // 设栈指针
        "movw r2, #0xED08           \n"
        "movt r2, #0xE000           \n"
        "str %0, [r2]              \n"  // VTOR = App 地址
        "cpsie i                    \n"  // 开中断
        "bx r1                     \n"  // 跳转
        :
        : "r" (APP_START_ADDR)
        : "r0", "r1", "r2"
    );
}
```

编译后反汇编验证：**无 push/pop**，MSM 修改后没有指令从旧栈或新栈读取数据。

### 教训

> **Cortex-M Bootloader 跳转是嵌入式领域最经典的 Bug 之一。** 核心问题：编译器为 C 函数生成的 prologue（push）和 epilogue（pop）假定整个函数执行期间栈指针不变。但 `jump_to_app` 需要在函数执行中途修改 MSP——这违反了编译器的假设。`__attribute__((naked))` 是 C 语言提供的唯一解决方案，它告诉编译器"不要帮我生成任何序言和尾声"，让开发者完全控制指令序列。这在 ARM Cortex-M 的 Bootloader、RTOS 上下文切换、中断返回等场景中是必须掌握的技巧。

---

## Bug #7：面包板 SPI 信号完整性 — 硬件对软件的隐性影响

### 现象

OTA 上传 32KB 固件时，SPI Flash 写入验证失败。每次读回的数据不同（`0x000000`、`0x004000`、`0x002000`、`0x000080`），但写入数据固定（`0x005000`）。只有 1 个 bit 不同。

### 调试思路

1. **观察规律**：每次读回值只有一个 bit 不同（0x50→0x40→0x20→0x80），说明不是完全随机噪声，而是特定信号线上的 bit 翻转
2. **排查软件**：SPI 配置（CPOL/CPHA/时钟/波特率）全部正确，Bootloader 和 App 用相同配置
3. **对比环境**：Bootloader 阶段（无 DMA、无 ADC、无 I2C）SPI 通信正常 → App 阶段（所有外设运行）SPI 通信异常
4. **关键发现**：App 启动后 DMA、ADC、I2C 等外设全部运行，GPIO 引脚频繁翻转产生电磁噪声，通过面包板的长金属条和杜邦线耦合到 SPI MISO 线上

### 根因

面包板的物理特性：金属簧片之间存在 **2-5pF 寄生电容**，相邻走线间距仅 **2.54mm**。SPI 时钟（16MHz）的边沿通过寄生电容耦合到相邻的 MISO 线，在信号上升/下降沿产生 **crosstalk**。当多个外设同时运行（DMA 控制 SPI2、ADC DMA、I2C DMA），GPIO 翻转频率增加，噪声加剧，导致 MISO 线上的 bit 被干扰。

### 修复

将 SPI Flash 从面包板改为**杜邦线直连芯片**（缩短信号路径），并在 VCC/GND 之间加 **100nF 陶瓷去耦电容**（吸收高频噪声）。修复后 SPI 通信完全稳定。

### 教训

> 嵌入式系统中，**软件的正确性不能脱离硬件环境来保证**。相同的代码在面包板上失败、在 PCB 上成功，是完全正常的。调试时如果排除了所有软件可能，必须回到硬件层面检查信号完整性。特别是 SPI 这类高速同步协议，对信号质量的要求远高于 I2C/UART。

---

## Bug #8：FreeRTOS 任务饥饿 — 高优先级任务独占 CPU

### 现象

FreeRTOS 启动后，LED 不闪、OLED 不更新、UART 无响应。GDB 显示 CPU 大部分时间在 idle task，偶尔在 `StartTaskLight` 中。

### 调试思路

1. **GDB 确认**：`bt` 显示 CPU 在 `prvIdleTask`，偶尔在 `HAL_ADC_ConvCpltCallback` → `osSemaphoreRelease` → `StartTaskLight`
2. **分析任务流**：`StartTaskLight` 优先级最高（AboveNormal），每次 ADC DMA 完成就唤醒。ADC 是连续模式 + DMA 循环，每秒产生上千次中断
3. **发现饥饿**：`StartTaskLight` 没有 `osDelay`，获取信号量后立即处理再等下一个。由于优先级最高，每次中断都抢占其他任务。`Task_OLED`、`Task_UART`、`Task_LED` 永远没机会执行

### 根因

`StartTaskLight` 的循环中缺少 `osDelay`。ADC 连续转换模式下，每次 DMA 传输完成都释放信号量，高优先级的 `StartTaskLight` 立刻被唤醒并抢占所有低优先级任务。由于处理时间（读 ADC + 更新状态机 + 发队列）远小于 ADC 采样间隔，`StartTaskLight` 反复就绪 → 抢占 → 处理 → 再等待，形成"饥饿链"，低优先级任务完全被压制。

### 修复

在 `StartTaskLight` 的循环末尾加 `osDelay(10)`，每次处理后主动让出 CPU 10ms，给低优先级任务执行窗口。

### 教训

> **RTOS 中，"正确"不等于"好用"。** 即使任务逻辑正确、优先级合理，如果高优先级任务的执行频率太高且没有主动让出 CPU，低优先级任务会被饿死。`osDelay` / `vTaskDelay` 不仅是"等一等"，更是在**主动释放 CPU 时间片**给其他任务。这是 RTOS 调度的核心概念：**协作式让出 + 抢占式响应**。

---

## Debug 方法论总结

| 方法 | 工具 | 适用场景 |
|------|------|----------|
| 快速状态检测 | `openocd -c "reg pc"` | 不确定 MCU 卡在哪 |
| 内存内容验证 | `openocd -c "mdw <addr> <len>"` | 验证 Flash 数据、向量表、异常帧 |
| 源码定位 | `arm-none-eabi-addr2line` | 从 PC/LR 地址反查源码行号 |
| 指令级分析 | `arm-none-eabi-objdump -d` | 确认编译器生成的指令序列 |
| ELF 结构检查 | `arm-none-eabi-readelf -l/-S` | 检查段地址、LOAD 映射 |
| 远程断点调试 | `gdb-multiarch` + OpenOCD | 需要单步跟踪时 |
| 排除法 | GDB 手动设置寄存器 | 隔离问题范围（Bootloader vs App） |
| 异常帧分析 | `mdw <MSP> 8` | HardFault 后还原崩溃现场 |
