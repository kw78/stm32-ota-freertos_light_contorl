# OTA 系统开发中的高难度 Bug 与调试记录

> 本文记录了在 STM32F103 + FreeRTOS + W25D64 SPI Flash 实现 OTA 升级系统过程中遇到的核心技术难点。每个 Bug 都完整记录了 **现象 → 排查思路 → 尝试手段 → 根因分析 → 最终修复** 的全过程，体现了从"不知道问题在哪"到"精确定位并修复"的完整调试方法论。

---

## Bug #1：SPI 字节序陷阱 — 串行协议的隐性契约

### 现象

W25D64 SPI Flash 驱动写完后，读 JEDEC ID 返回 `0x000000`。SPI 线路物理连接正确，用示波器抓取 SPI 时钟（PB13）和 MOSI（PB15）波形，能看到完整的 8 个时钟周期和对应的 bit 变化。但 Flash 完全无响应——读到的数据全是 0。

### 调试思路

1. **排除硬件问题**：通过 ST-Link SWD 读取 SPI2 外设寄存器
   ```
   SPI2->CR1 = 0x034C  →  MSTR=1(SPI主机), BR=001(分频4), SPE=1(SPI已使能)
   SPI2->CR2 = 0x0000  →  8bit数据, 无NSS软件管理
   ```
   外设寄存器配置完全正确。

2. **最小化测试**：不依赖 HAL 库，直接操作寄存器发送 `0x9F`（Read JEDEC ID 命令），仍然返回 0。这排除了 HAL 库配置问题。

3. **对比参考实现**：查阅 W25D64 数据手册第 5.1 节"Read JEDEC ID (9Fh)"的时序图。数据手册明确要求：CS 拉低后，主机按字节顺序发送命令字节，Flash 在时钟驱动下逐字节响应。关键约束：**命令字节必须在数据流的第一个位置**。

4. **逐字节追踪**：用 `HAL_UART_Transmit` 在 SPI 发送前后打印 MOSI 线上的实际数据，发现发出的第一个字节是 `0x34` 而不是 `0x03`。

### 根因

```c
// 我的写法
uint32_t cmd = 0x03001234;  // 命令 0x03 + 地址 0x001234
HAL_SPI_Transmit(&hspi2, &cmd, 4, 100);
```

ARM Cortex-M3 是**小端序**（Little-Endian），`uint32_t` 在内存中的布局是低字节在低地址。SPI 控制器从低地址开始按字节发送：

```
内存地址:  &cmd+0  &cmd+1  &cmd+2  &cmd+3
数据:      0x34    0x12    0x00    0x03
期望发出:  0x03    0x00    0x12    0x34  (命令在前)
实际发出:  0x34    0x12    0x00    0x03  (低字节在前)
```

Flash 收到的第一个字节是 `0x34`，不是任何有效 SPI Flash 命令。Flash 芯片直接忽略这个字节，后续通信全部无效。

### 修复

```c
uint8_t cmd[4] = {
    0x03,                  // 命令（字节数组天然按声明顺序存储在连续内存）
    (addr >> 16) & 0xFF,  // 地址高字节
    (addr >>  8) & 0xFF,  // 地址中字节
    (addr      ) & 0xFF   // 地址低字节
};
HAL_SPI_Transmit(&hspi2, cmd, 4, 100);
```

`uint8_t` 数组在内存中的排列就是声明顺序，CPU 按低地址到高地址逐字节发送，和串行协议的"命令在前"要求完全一致。

### 教训

> SPI/I2C/UART 等串行协议都是按字节流发送的。在内存中，`uint32_t` 的字节排列由 CPU 的**端序**（Endianness）决定，而串行协议要求的是**逻辑顺序**。两者不一定一致。**永远用 `uint8_t` 数组来控制字节发送顺序**——这是嵌入式串行通信的第一准则。

---

## Bug #2：FreeRTOS 与 HAL 抢占 SysTick — 单一时钟源的资源冲突

### 现象

集成 FreeRTOS 后，`HAL_Delay()` 永远不返回（卡死在 while 循环中），LED 不闪，系统完全无响应。但用 GDB 打断点发现 FreeRTOS 的 idle task 确实在运行——说明 FreeRTOS 的调度器已启动，但 HAL 计时完全失效。

### 调试思路

1. **GDB 定位**：在 `HAL_Delay` 函数入口设断点 → 进入后检查 `HAL_GetTick()` 返回值 → 永远返回 0。`HAL_Delay(1000)` 的循环条件 `HAL_GetTick() - tickstart < 1000` 永远为真 → 死循环。

2. **追查 HAL 时基配置**：阅读 `stm32f1xx_hal_conf.h` → `TICK_INT_PRIORITY = 15`，确认 HAL 默认使用 SysTick 作为时基。`HAL_InitTick()` 在 `HAL_Init()` 中被调用，配置 SysTick 每 1ms 触发一次中断。

3. **追查 FreeRTOS 时钟源**：阅读 `FreeRTOS/Source/portable/GCC/ARM_CM3/port.c` → `xPortSysTickHandler()` 绑定了 SysTick 中断。`vPortSetupTimerInterrupt()` 也配置 SysTick。

4. **确认资源冲突**：两个系统都试图配置同一个硬件资源——Cortex-M3 的 SysTick 24 位向下计数器。FreeRTOS 在 `osKernelStart()` 时重新配置 SysTick，覆盖了 HAL 的配置。

### 根因

SysTick 是 ARM Cortex-M3 内核**唯一**的系统定时器，只有一个实例。两个系统同时使用会产生冲突：

```
时间线：
  HAL_Init()         → SysTick 配置为 HAL 时基（1ms 中断）
  ...
  osKernelStart()    → FreeRTOS 重新配置 SysTick 为 RTOS tick（覆盖 HAL 配置）
  ...
  HAL_Delay(1000)    → HAL_GetTick() 读取 SysTick 计数器 → 但 SysTick 已被 FreeRTOS 控制
                       → 计数器行为异常 → HAL_GetTick() 永远返回 0 → 死循环
```

### 修复

在 CubeMX 中将 **SYS → Timebase Source** 从 SysTick 改为 **TIM4**：

```c
// stm32f1xx_hal_timebase_tim.c (CubeMX 自动生成)
HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
    __HAL_RCC_TIM4_CLK_ENABLE();       // 使能 TIM4 时钟
    htim4.Instance = TIM4;
    htim4.Init.Period = 999;          // 1MHz/1000 = 1kHz
    htim4.Init.Prescaler = ...;       // 计算预分频
    HAL_TIM_Base_Init(&htim4);        // 初始化 TIM4
    HAL_TIM_Base_Start_IT(&htim4);    // 启动 TIM4 中断
    return HAL_OK;
}
```

修改后：
- **SysTick** 完全交给 FreeRTOS 的 `vPortSetupTimerInterrupt()` 管理
- **TIM4** 由 HAL 的 `HAL_InitTick()` 管理，提供 `HAL_GetTick()` 和 `HAL_Delay()`
- 两个系统使用独立的硬件定时器，互不干扰

### 教训

> STM32 + FreeRTOS + HAL 三件套的项目，**第一步永远是改 HAL Timebase**。这必须在添加 FreeRTOS 之前完成，否则 CubeMX 生成代码时会产生配置冲突。Cortex-M3 只有一个 SysTick，不能被两个系统共享。这个坑的隐蔽性在于：HAL_Delay 卡死不会触发 HardFault，只是静默死循环，调试时极易误判为"程序卡死在某个函数"。

---

## Bug #3：DMA 中断优先级边界违规 — FreeRTOS 的隐性约束

### 现象

在 ADC DMA 完成回调中调用 `osSemaphoreRelease()` 后，系统偶发 HardFault 或任务不再被唤醒。用 GDB 检查发现 HardFault 时 PC 指向 `HardFault_Handler`，调用栈中出现 FreeRTOS 内部的 `pxQueueSend` 函数。

### 调试思路

1. **GDB 检查**：HardFault 发生在 `pxQueueSend` 内部，这是信号量释放的底层实现。说明 FreeRTOS 的内部数据结构在中断上下文中被破坏了。

2. **查阅 FreeRTOS 文档**：Cortex-M3 port 有一个关键参数 `configMAX_SYSCALL_INTERRUPT_PRIORITY`（默认 5）。文档明确说明：**优先级数值 ≤ 此值的中断中，禁止调用任何 FreeRTOS API**，包括 `FromISR` 版本。

3. **理解 Cortex-M3 优先级模型**：STM32 的中断优先级是**数值越小优先级越高**（与直觉相反）。CubeMX 默认给 DMA1_Channel1 分配优先级 5，恰好等于 `configMAX_SYSCALL_INTERRUPT_PRIORITY`。

4. **分析破坏机制**：FreeRTOS 在 `pxQueueSend` 中使用 `BASEPRI` 寄存器临时屏蔽特定优先级的中断来保护临界区。如果调用者已经是相同优先级的中断，`BASEPRI` 无法屏蔽自己，导致临界区保护失效。

### 根因

FreeRTOS 的 Cortex-M3 port 使用 `BASEPRI` 寄存器实现临界区：`configMAX_SYSCALL_INTERRUPT_PRIORITY` 定义了临界区的边界。优先级数值 ≤ 此值的中断可以屏蔽更高的中断，但**不能屏蔽自己**。当这些中断调用 `osXxxFromISR()` 时，FreeRTOS 内部的 `configASSERT` 检测到非法调用，触发 HardFault。

```
优先级数字:  0  1  2  3  4  5  6  7  ...  15
中断优先级:  高 ←────────────────────────────→ 低
FreeRTOS 临界区: |←── 可以调用 API ──→|←── 不能调用 ──→|
                 ↑                     ↑
              configMAX_SYSCALL = 5
```

### 修复

在 CubeMX → NVIC 中将 DMA 中断优先级从 5 改为 **6**。数值越大优先级越低，6 > 5，DMA 中断可以被 FreeRTOS 的临界区屏蔽，安全调用 `osSemaphoreRelease()`。

### 教训

> FreeRTOS 在 Cortex-M 上的中断优先级规则：**只有优先级数值 > `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` 的中断，才能安全调用 FreeRTOS API**。这个阈值是 `FreeRTOSConfig.h` 中的编译时常量，默认值 5。CubeMX 默认给 DMA 分配优先级 5，刚好踩在边界上。这是一个极其隐蔽的配置陷阱——不会编译报错，不会运行时立即崩溃，只在特定时序下偶发 HardFault。

---

## Bug #4：UART 回调中的阻塞操作导致 OTA 数据丢失

### 现象

OTA 上传 100 字节测试固件成功（2 个 DATA 包，CRC32 校验通过），但上传 32KB 真实固件时（508 个 DATA 包），END 包 CRC32 校验失败。设备端计算的 CRC（`0x2CFC43E4`）与上位机发送的 CRC（`0xD866307C`）完全不匹配——这不是"差几个 bit"的问题，而是大量数据损坏。

### 调试思路

**第一轮：验证上位机正确性**

```python
import binascii
with open('build/Debug/gcctest.bin', 'rb') as f:
    data = f.read()
print(f"CRC32: 0x{binascii.crc32(data) & 0xFFFFFFFF:08X}")
# → 0xD866307C，与 Python 脚本输出一致
```
排除上位机问题。

**第二轮：在设备端加回读验证**

在 `OTA_HandlePacket` 的 `CMD_OTA_DATA` 处加验证——写入 SPI Flash 后立即读回，和发送数据 `memcmp`：

```c
W25_WritePage(OTA_FW_ADDR + ota_bytes_written, data, len);
uint8_t verify_buf[64];
W25_Read(OTA_FW_ADDR + ota_bytes_written, verify_buf, len);
if (memcmp(data, verify_buf, len) != 0) {
    // 打印前 3 字节对比
    // w=005000 r=000000  ← 第 0 包就失败
}
```

**第三轮：排除 SPI Flash 问题**

验证方法：检查 Bootloader 能否正常读取 SPI Flash。Bootloader 在 OTA 之前成功读取了 OTA 标志（magic=0x4F544131, state=1, fw_size=32452），说明 SPI Flash 硬件正常。

但如果 SPI Flash 在 App 的 OTA 写入过程中不稳定呢？在面包板上，SPI Flash 写入（页编程）需要 ~3ms 的 BUSY 等待，期间 SPI 时钟停止，可能导致 Flash 内部状态机异常。但 Bootloader 的 `W25_WaitBusy` 有 500ms 超时保护，应该能处理这种情况。

**第四轮：锁定 UART 传输**

问题不在 SPI Flash，而在 UART 数据接收。用一个最简单的 Python 脚本逐字节发送并检查：

```python
import serial, time
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=2)
time.sleep(1)
s.write(b'\xAA')
time.sleep(0.1)
# 检查设备是否收到
```

设备端在 UART 回调中加调试输出（`snprintf` + `HAL_UART_Transmit`）→ 发现在回调执行期间，新的 UART 字节到达但**被丢失**了。

### 根因

`HAL_UARTEx_RxEventCallback` 中有一行调试输出：

```c
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
    if (huart->Instance == USART1) {
        // 调试输出：打印收到的字节数
        char dbg[20];
        int n = snprintf(dbg, sizeof(dbg), "RX:%d\r\n", Size);
        HAL_UART_Transmit(&huart1, (uint8_t *)dbg, (uint16_t)n, 100);  // ← 问题所在
        ...
    }
}
```

**`HAL_UART_Transmit` 是阻塞函数**——它以轮询方式等待 TXE（发送数据寄存器空）标志，每次发送 1 字节约需 87μs（115200 波特率）。发送 "RX:69\r\n"（7 字节）需要约 600μs。

在这 600μs 期间，**UART 的 RXNE（接收数据寄存器非空）中断被屏蔽**（因为 CPU 正在 ISR 上下文中执行阻塞操作）。STM32F103 的 UART 只有 1 字节的接收寄存器，没有 FIFO。如果新字节到达时 RXNE 中断被屏蔽：
1. 新字节进入接收寄存器
2. 如果在 RXNE 被清除前又有字节到达，发生**溢出错误**（ORE），前一个字节被覆盖
3. 丢失的字节永远无法恢复

**为什么 100 字节测试通过但 32KB 失败？**
- 100 字节固件 = 2 个 DATA 包。上位机发完一个包后等 ACK（~10ms），回调有足够时间完成
- 32KB 固件 = 508 个 DATA 包。包间间隔极短（~1ms），回调的 600μs 阻塞导致多个包的字节丢失

### 修复

从 UART 回调中移除所有阻塞操作：

```c
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
    if (huart->Instance == USART1) {
        for (int i = 0; i < Size; i++) {
            OTA_RingBuf_Put(uart_rx_buf[i]);  // 极速：写入环形缓冲区
        }
        osSemaphoreRelease(sem_uart_rxHandle);  // 极速：释放信号量
        HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart_rx_buf, sizeof(uart_rx_buf));  // 重新启动接收
    }
}
```

三步操作都在**微秒级**完成（环形缓冲区写入 ~20μs，信号量释放 ~5μs），不会屏蔽后续的 UART 字节。

### 教训

> **ISR 黄金法则：ISR 中不做任何阻塞操作。** `HAL_UART_Transmit` 是阻塞的（轮询等待 TXE），在 ISR 中调用会阻塞 CPU 直到发送完成。这不仅导致自身延迟，还会屏蔽同优先级和低优先级的中断。STM32F103 的 UART 没有 FIFO，只有 1 字节接收寄存器，任何中断延迟都可能导致字节丢失。正确做法：ISR 只做"放数据到缓冲区 + 通知任务"，耗时操作全部交给任务上下文。这个 Bug 的教训不仅适用于 UART，也适用于所有中断服务程序。

---

## Bug #5：ELF LOAD 段地址对齐导致固件烧录错位

### 现象

用 OpenOCD 的 `program` 命令烧录 App ELF 到 0x08002000 后，Bootloader 读到的 App 向量表是垃圾数据（`0x464c457f 0x00010101`——这是 ELF 文件头的魔数 `\x7FELF`），而不是 App 的真实向量表（应该是 `0x20005000 0x0800a19d`）。

### 调试思路

1. **读取 Flash 内容**：
   ```bash
   openocd -c "mdw 0x08002000 4"
   # → 464c457f 00010101 00000000 00000000
   ```
   这是 ELF 文件头（`\x7FELF` 在内存中为 `464c457f` 小端序表示）。App 的真实向量表没有被写入 0x08002000。

2. **分析 ELF 段布局**：
   ```bash
   arm-none-eabi-readelf -l gcctest.elf
   ```
   ```
   LOAD  0x000000 0x08000000 0x08000000 0x0b200 0x0b200 R E 0x10000
   ```
   
   LOAD 段的 `PhysAddr = 0x08000000`，`Align = 0x10000`（64KB 对齐）。但实际代码段 `.isr_vector` 的 VMA 是 `0x08002000`。

3. **根因定位**：OpenOCD 的 `program` 命令使用 LOAD 段的 `PhysAddr`（0x08000000）作为烧录目标地址。它把整个 ELF 的 LOAD 段（包括 ELF 头部 + 填充 + 代码）写入 0x08000000。结果：
   - 0x08000000 - 0x08001FFF：ELF 头部和填充（无意义数据）→ **覆盖了 Bootloader！**
   - 0x08002000 - 0x0800B200：App 代码 → 地址正确

4. **验证**：用 `arm-none-eabi-objdump -h` 检查 `.isr_vector` 段的地址 → `0x08002000`，确认代码本身没问题。

### 根因

GCC 链接器生成 ELF 文件时，LOAD 段的 `PhysAddr` 由于地址对齐要求（`Align = 0x10000`）被向下取整。当代码从 `0x08002000` 开始时，对齐后的 `PhysAddr` 是 `0x08000000`。

OpenOCD 的 `program` 命令使用 `PhysAddr` 而非实际段地址，导致：
- ELF 头部（52 字节）+ 填充（约 8KB）被写入 0x08000000-0x08001FFF → **覆盖 Bootloader**
- App 代码被正确写入 0x08002000 → 但 Bootloader 已被破坏，无法跳转

### 修复

用 `objcopy` 将 ELF 转为原始 bin 文件（按 section 的 VMA 排列），再用 `flash write_image` 指定正确的烧录地址：

```bash
# ELF → bin（objcopy 按 section VMA 地址排列数据）
arm-none-eabi-objcopy -O binary build/Debug/gcctest.elf /tmp/app.bin

# bin → Flash（指定目标地址）
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "flash write_image /tmp/app.bin 0x08002000 bin"
```

`bin` 格式是纯粹的内存映像——文件的第 0 字节对应 `PhysAddr`（这里通过命令行指定为 0x08002000）。objcopy 生成的 bin 文件按 section VMA 排列，所以第 0 字节就是 `.isr_vector` 的第一个字（App 向量表）。

### 教训

> `arm-none-eabi-objcopy -O binary` 生成的 bin 文件是按 **section VMA 地址**排列的，而 OpenOCD `program` 命令使用的是 ELF LOAD 段的 **PhysAddr**。当两者不一致时（通常因为链接器的地址对齐 `Align` 字段），`program` 会把数据写入错误地址。**烧录多分区固件的标准做法是先 `objcopy` 再 `flash write_image`**，手动指定每个分区的烧录地址。

---

## Bug #6：Bootloader 的 jump_to_app 中编译器生成的 push/pop 导致 HardFault

### 现象

OTA 上传成功，设备复位，Bootloader 搬运固件成功（Flash 内容验证正确），但跳转到 App 后立即崩溃。GDB 显示 `HardFault_Handler`，调用栈中 `__libc_init_array` → `blx r3` 跳转到垃圾地址 `0xd58aebde`。

### 调试思路（最复杂的调试过程，6 轮）

**第一轮：排除法——App 本身有没有问题？**

1. 单独烧录 App 到 0x08002000（不用 Bootloader）→ App 不跑。但这可能是因为 VTOR 没设置。
2. 用 OpenOCD 手动设置 VTOR=0x08002000, SP=0x20005000, PC=0x0800a19d → App 正常运行到 `HAL_Delay`。
3. **结论：App 代码本身没问题，问题出在 Bootloader 的跳转过程。**

**第二轮：异常帧分析——从崩溃现场反推根因**

Cortex-M3 发生异常时，硬件自动将 `{R0,R1,R2,R3,R12,LR,PC,xPSR}` 压入当前 MSP 指向的栈。读 MSP 处的 8 个 word 还原崩溃现场：

```bash
openocd -c "mdw 0x20004fe4 8"
```
```
0x20004fe4: 20000064 00000000 2000355c 00000000
            R0       R1       R2       R3
0x20004fec: 40021000 08003463 d58aebde d5aaebdf
            R12      LR        PC        xPSR
```

- **LR = `0x08003463`**：用 `addr2line` 定位到 `__libc_init_array` 内部（init.c:44）
- **PC = `0xd58aebde`**：垃圾地址，CPU 试图执行这个地址的指令 → BusFault → HardFault
- **xPSR = `0xd5aaebdf`**：异常号位域被破坏（正常 HardFault 的 xPSR 低 9 位应为 3）

结论：`__libc_init_array` 调用 `.init_array` 中的函数指针时，函数指针地址是垃圾值。

**第三轮：验证 Flash 内容——数据到底对不对？**

```bash
# 读 Flash 中 .init_array 的内容
openocd -c "mdw 0x0800b1f8 2"
# → 0x08002179 0x08002151

# 对比 ELF 中 .init_array 的内容
arm-none-eabi-objdump -s -j .init_array gcctest.elf
# → 800b1f8  79210008
```

Flash 内容完全正确！`0x08002179` 是 `frame_dummy` 函数的地址。**问题不在 Flash 数据，而在运行时 CPU 读到了垃圾值。**

**第四轮：排除法——跳转后 App 能不能独立运行？**

用 OpenOCD 手动跳转（设置 VTOR=0x08002000, SP=0x20005000, PC=0x0800a19d）→ App 正常运行。**确认 App 代码无误，问题出在 Bootloader 的跳转过程本身。**

**第五轮：GDB 逐步跟踪——设断点时正常，不设断点时崩溃**

在 Bootloader 的 `jump_to_app` 末尾（`bx r0`）设断点，检查跳转前的寄存器状态：
- `r0 = 0x0800a19d`（App Reset_Handler，正确）
- `r3 = 0x08002000`（APP_START_ADDR，正确）
- `VTOR = 0x08002000`（正确）

继续执行 → App 正常运行！但**去掉断点** → 崩溃。

这是经典的 **海森堡效应（Heisenbug）**——硬件断点暂停/恢复 CPU 的过程改变了系统时序（Flash 访问延迟、中断 pending 状态），恰好避开了崩溃路径。

**第六轮：反汇编——定位根因**

```bash
arm-none-eabi-objdump -d bootloader.elf | grep -A 20 "jump_to_app"
```

```asm
080003c8 <jump_to_app>:
 80003c8:  ldr    r3, [pc, #32]   ; r3 = 0x08002000 (APP_START_ADDR)
 80003ca:  push   {r4}            ; ← 保存 r4 到【Bootloader 的栈】(MSP = 0x20004xxx)
 80003cc:  ldr    r4, [r3, #0]    ; r4 = *(0x08002000) = App SP
 80003ce:  ldr    r0, [r3, #4]    ; r0 = *(0x08002004) = App Reset_Handler
 80003d0:  cpsid  i              ; 关中断
 80003d2:  ... SysTick disable ...
 80003de:  msr    MSP, r4        ; ← MSP 改为 0x20005000（App 栈顶）
 80003e2:  str    r3, [r2, #0xd08]; VTOR = 0x08002000
 80003e6:  cpsie  i              ; 开中断
 80003e8:  pop    {r4}           ; ← 从【新 MSP = 0x20005000】弹出！
 80003ea:  bx     r0             ; 跳转到 App
```

**根因确认：**

1. `push {r4}` 在 `0x08003ca` 将 r4 压入 Bootloader 的栈（MSP 当前指向 `0x20004xxx`）
2. `msr MSP, r4` 在 `0x08003de` 将 MSP 改为 `0x20005000`（App 栈顶）
3. `pop {r4}` 在 `0x08003e8` 试图从 MSP（`0x20005000`）弹出 → **这个地址在 STM32F103C8 的 20KB RAM 边界上**（RAM = 0x20000000~0x20004FFF），读取越界触发 BusFault → HardFault

STM32F103C8T6 的 RAM 是 20KB：`0x20000000` 到 `0x20004FFF`。`0x20005000` 恰好是 RAM 结束地址 + 1，硬件访问会触发总线错误。

### 修复

用 `__attribute__((naked))` 告诉编译器不要生成任何函数序言/尾声，然后用纯内联汇编实现跳转：

```c
static void __attribute__((naked)) jump_to_app(void){
    __asm volatile(
        "cpsid i                    \n"  // 1. 关中断（防止跳转过程中被中断打断）
        "ldr r0, [%0]              \n"  // 2. r0 = App 栈顶地址（从 App 向量表读取）
        "ldr r1, [%0, #4]          \n"  // 3. r1 = App 入口地址（Reset_Handler）
        "msr MSP, r0               \n"  // 4. 设置主栈指针为 App 的栈
        "movw r2, #0xED08           \n"  // 5. r2 = SCB->VTOR 低 16 位
        "movt r2, #0xE000           \n"  // 6. r2 = SCB->VTOR 完整地址
        "str %0, [r2]              \n"  // 7. VTOR 指向 App 向量表
        "cpsie i                    \n"  // 8. 开中断（App 需要中断支持）
        "bx r1                     \n"  // 9. 跳转到 App Reset_Handler
        :
        : "r" (APP_START_ADDR)     // 输入：r0 持有 App 起始地址
        : "r0", "r1", "r2"         // 告知编译器这些寄存器会被修改
    );
}
```

编译后反汇编验证——**零条 push/pop 指令**：

```asm
080003c8 <jump_to_app>:
 80003c8:  ldr    r3, [pc, #24]   ; r3 = 0x08002000
 80003ca:  cpsid  i
 80003cc:  ldr    r0, [r3, #0]    ; App SP
 80003ce:  ldr    r1, [r3, #4]    ; App Reset_Handler
 80003d0:  msr    MSP, r0         ; ← 设置栈指针后，没有任何指令从栈上读数据
 80003d4:  movw   r2, #0xed08
 80003d8:  movt   r2, #0xe000
 80003dc:  str    r3, [r2, #0]    ; VTOR
 80003de:  cpsie  i
 80003e0:  bx     r1              ; 跳转
```

### 教训

> **Cortex-M Bootloader 跳转是嵌入式领域最经典的 Bug 之一。** 核心问题：C 编译器为普通函数生成的 prologue（`push {r4}`）和 epilogue（`pop {r4}`）假定整个函数执行期间栈指针不变。但 `jump_to_app` 需要在函数执行中途修改 MSP——这违反了编译器的假设。`__attribute__((naked))` 是 C 语言提供的唯一解决方案，它告诉编译器"不要帮我生成任何序言和尾声".

> 额外教训：**GDB 设断点时能跑、不设断点时崩溃**是经典的海森堡效应。硬件断点暂停 CPU 后恢复执行，改变了 Flash/SRAM 的访问时序和中断 pending 状态，恰好避开了 `pop {r3}` 越界触发 BusFault 的路径。调试这类问题时，**反汇编分析比 GDB 单步跟踪更可靠**——因为 GDB 的观测行为本身可能改变被观测系统。

---

## Bug #7：面包板 SPI 信号完整性 — 硬件对软件的隐性影响

### 现象

OTA 上传 32KB 固件时，SPI Flash 写入验证失败。每次读回的数据都不同：

```
写入: w=005000    ← 固定
读回: r=000000    ← 第 1 次
读回: r=004000    ← 第 2 次（bit 5 翻转）
读回: r=002000    ← 第 3 次（bit 6 翻转）
读回: r=000080    ← 第 4 次（bit 7 翻转）
```

每次只有一个 bit 不同，不是完全随机噪声。写入数据固定但读回值不断变化——这是典型的**信号完整性问题**。

### 调试思路

1. **排除软件问题**：SPI 配置（CPOL=LOW, CPHA=1EDGE, 16MHz）在 Bootloader 和 App 中完全一致。Bootloader 阶段能正常读取 OTA 标志 → SPI 驱动代码没问题。

2. **对比环境差异**：Bootloader 阶段只有 SPI + GPIO 在运行；App 启动后 DMA（SPI2/ADC/I2C）、ADC 连续转换、I2C OLED/MPU6050 全部启动。GPIO 引脚翻转频率大幅增加。

3. **分析物理路径**：面包板上 SPI Flash 模块通过杜邦线连接，信号路径约 5-10cm。相邻面包板走线间距仅 2.54mm。16MHz 时钟的上升/下降沿通过寄生电容（2-5pF）耦合到相邻走线。

4. **验证方法**：断开 SPI Flash，单独测量 MISO 线上的波形 → 在 App 启动前（Bootloader 阶段）信号干净；App 启动后（DMA/ADC/I2C 运行）信号出现毛刺。

### 根因

面包板的物理特性导致信号完整性问题：
- **寄生电容**：相邻金属簧片之间存在 2-5pF 电容
- **串扰（Crosstalk）**：16MHz SPI 时钟边沿通过寄生电容耦合到 MISO 线
- **接地回路**：面包板的地线阻抗较高，高速信号的回流路径不理想
- **无去耦电容**：SPI Flash 的 VCC 引脚没有去耦电容，电源噪声叠加到信号上

当 App 启动后（DMA、ADC、I2C 同时运行），GPIO 翻转频率增加，串扰加剧，MISO 线上的 bit 被干扰，导致 Flash 读回值错误。

### 修复

1. **杜邦线直连**：将 SPI Flash 从面包板改为杜邦线直接连接 STM32 和 W25D64 芯片引脚，信号路径缩短到 ~3cm

2. **降低 SPI 时钟**：从 16MHz 降至 4MHz 作为备选方案（时钟频率越低，对信号完整性要求越低）

修复后 SPI 通信完全稳定，OTA 上传 32KB 固件 100% 成功。

### 教训

> 嵌入式系统中，**软件的正确性不能脱离硬件环境来保证**。相同的代码在面包板上失败、在 PCB 上成功是完全正常的。调试时如果排除了所有软件可能，必须回到硬件层面检查信号完整性。特别是 SPI 这类高速同步协议，对信号质量的要求远高于 I2C/UART。**面包板只适合低速验证（< 1MHz），高速 SPI 必须用 PCB 或直连。**

---

## Bug #8：FreeRTOS 任务饥饿 — 高优先级任务独占 CPU

### 现象

FreeRTOS 启动后，LED 不闪、OLED 不更新、UART 无响应。但用 GDB 检查发现 CPU 大部分时间在 idle task（`prvIdleTask`），偶尔在 `StartTaskLight` 中。

### 调试思路

1. **GDB 确认**：`bt` 显示 CPU 在 `prvIdleTask`，偶尔在 `HAL_ADC_ConvCpltCallback` → `osSemaphoreRelease` → `StartTaskLight`。说明任务确实在运行，但只有 Light 任务偶尔执行。

2. **分析任务流**：`StartTaskLight` 优先级为 AboveNormal（仅次于 UART），每次 ADC DMA 完成都释放 `sem_adc_ready` 信号量。ADC 配置为连续转换 + DMA 循环，每秒产生数百次中断。

3. **发现饥饿链**：
   ```
   ADC 中断 → 释放 sem_adc_ready
   → StartTaskLight 立刻就绪（优先级高）
   → 抢占其他所有任务
   → 处理完（读 ADC + 状态机 + 发队列）→ 等待下一个信号量
   → 立刻又被唤醒 → 又抢占...
   ```
   `StartTaskLight` 的处理时间（~1ms）远小于 ADC 采样间隔（~10ms），形成无限循环，低优先级任务（OLED/LED/SPIFlash）永远没机会执行。

### 根因

`StartTaskLight` 的 `for(;;)` 循环中缺少 `osDelay`。每次处理完后立即重新等待信号量，由于优先级高于其他任务，信号量一释放就立刻抢占。虽然信号量是二进制的（不会堆积），但 ADC 中断频率太高，任务几乎没有空闲窗口让低优先级任务执行。

### 修复

在 `StartTaskLight` 的循环末尾加 `osDelay(10)`：

```c
void StartTaskLight(void *argument)
{
    for(;;)
    {
        osSemaphoreAcquire(sem_adc_readyHandle, osWaitForever);
        uint16_t cur = adc_buf[0];
        LightState_Update(cur);
        osMessageQueuePut(queue_lightHandle, &state_now, 0, 0);
        osDelay(10);  // 主动让出 CPU 10ms
    }
}
```

`osDelay(10)` 的作用不是简单的"等 10ms"——它将任务状态从"就绪"改为"阻塞"，FreeRTOS 调度器会检查是否有其他就绪任务，如果有就切换到该任务执行。10ms 后 SysTick 中断将任务状态改回"就绪"，任务继续执行。

### 教训

> **RTOS 中，"正确"不等于"好用"。** 即使任务逻辑正确、优先级合理，如果高优先级任务的执行频率太高且没有主动让出 CPU，低优先级任务会被饿死。`osDelay` / `vTaskDelay` 不仅是"等一等"，更是在**主动释放 CPU 时间片**给其他任务——这是 RTOS 调度的核心概念：**协作式让出 + 抢占式响应**。在裸机开发中，`while(1)` 循环配合 `HAL_Delay` 就能工作，但在 RTOS 中，每个任务都必须有明确的"让出点"。

---

## Bug #9：DMA 中断优先级边界 + 初始化顺序 — 双重潜伏 Bug

### 现象

OTA 上传新固件后设备复位，OLED 不更新、LED 不闪、串口无响应。用 OpenOCD halt 后发现 MCU 卡死在 `DMA1_Channel1_IRQHandler` 中，反复读取 PC 永远是同一个地址（`0x0800a014` 或 `0x08004f12`，取决于固件版本），说明 DMA 中断在无限循环。

### 调试思路

1. **确认卡死位置**：连续 halt 5 次，PC 始终在 `DMA1_Channel1_IRQHandler` → `HAL_DMA_IRQHandler`，说明 DMA 中断处理函数无法正常返回。

2. **检查异常帧**：xPSR = `0x6100001b`，ISR 号 27 = DMA1_Channel1。MSP 不变 → CPU 在 ISR 中打转，没有返回到 Thread mode。

3. **分析中断处理链**：
   ```
   DMA 传输完成 → DMA1_Channel1_IRQHandler()
     → HAL_DMA_IRQHandler(&hdma_adc1)
       → HAL_ADC_ConvCpltCallback()
         → osSemaphoreRelease(sem_adc_readyHandle)
   ```

4. **检查信号量状态**：`sem_adc_readyHandle` 在 `MX_FREERTOS_Init()` 中创建（line 287），但 `HAL_ADC_Start_DMA()` 在 line 278 就启动了 DMA。DMA 中断在 `MX_FREERTOS_Init()` **之前**触发时，`sem_adc_readyHandle` 还是 NULL。

5. **追踪 configASSERT**：`osSemaphoreRelease(NULL)` → 内部 `xQueueSendFromISR(NULL, ...)` → `configASSERT(xQueue)` 失败 → `taskDISABLE_INTERRUPTS(); for(;;);`。

6. **关键发现——为什么中断无法屏蔽**：`taskDISABLE_INTERRUPTS()` 设置 BASEPRI = `configMAX_SYSCALL_INTERRUPT_PRIORITY` = `6 << 4 = 0x60`。BASEPRI 屏蔽优先级数值 ≤ 0x5F 的中断。但 DMA 中断优先级 = 6 = `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`，正好等于阈值，**不被 BASEPRI 屏蔽**。DMA 中断持续触发 → 每次触发都进入 `configASSERT` 死循环 → 永远无法恢复。

### 根因

两个 Bug 叠加：

**Bug A：初始化顺序错误**

```c
HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buf, ADC_BUF_SIZE);  // ← DMA 启动
// ...
osKernelInitialize();   // ← FreeRTOS 内核初始化
MX_FREERTOS_Init();     // ← 信号量在这里才创建
osKernelStart();        // ← 调度器启动
```

DMA 在信号量创建之前就启动了。ADC 转换完成触发 DMA 中断 → 回调中调 `osSemaphoreRelease(NULL)` → `configASSERT` 崩溃。

**Bug B：DMA 中断优先级 = FreeRTOS 临界区阈值**

```c
// dma.c
HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 6, 0);  // 优先级 = 6

// FreeRTOSConfig.h
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 6  // 阈值 = 6
```

FreeRTOS 的 `BASEPRI` 临界区只能屏蔽优先级数值 **>** 阈值的中断。优先级 = 阈值的中断**不在屏蔽范围内**。当 `configASSERT` 触发 `taskDISABLE_INTERRUPTS()` 时，DMA 中断（优先级 6）继续触发 → 死循环无法恢复。

### 修复

**修复 A：移动 DMA 启动到 FreeRTOS 初始化之后**

```c
osKernelInitialize();
MX_FREERTOS_Init();          // 信号量、队列在这里创建

HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buf, ADC_BUF_SIZE);  // DMA 现在才启动

osKernelStart();
```

**修复 B：DMA 中断优先级改为 7**

```c
// dma.c — 所有 DMA 通道优先级改为 7
HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 7, 0);  // 7 > 6，可以安全调用 FreeRTOS API
HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 7, 0);
HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 7, 0);
HAL_NVIC_SetPriority(DMA1_Channel6_IRQn, 7, 0);
```

### 教训

> **Bug A（初始化顺序）**：外设中断的启动必须在对应的 IPC 对象（信号量、队列）创建之后。裸机开发中不存在这个问题（中断回调直接处理数据），但 RTOS 中断回调依赖内核对象，必须保证内核对象先存在。这是一个**时序依赖**问题——代码能编译、能烧录、甚至能短暂运行，但在特定时序下崩溃。

> **Bug B（优先级边界）**：FreeRTOS 的 `BASEPRI` 临界区使用 **严格大于**（`>`）而非大于等于（`>=`）来判断哪些中断可以调用 API。CubeMX 默认给 DMA 分配优先级 5 或 6，恰好踩在边界上。**STM32 + FreeRTOS 项目中，所有需要调用 FreeRTOS API 的中断，优先级必须设为 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY + 1` 或更大。** 这个规则在 Bug #3 中已经记录过，但 CubeMX 重新生成代码时会覆盖手动修改，导致 Bug 复发。

---

## Bug #10：Bootloader 搬运固件前缺少内部 Flash 擦除（代码审查发现）

> 来源：OTA 全链路代码审查（fix/ota-bugfix 分支），非硬件现场调试。

### 现象（推演）

在 App 已经在运行的情况下做第二次 OTA 升级，Bootloader 搬运后 App 行为异常或直接 HardFault；且由于 PENDING 标志被保留（回读校验失败 → 重试 → 再失败），设备表现为每次复位都"卡在 Bootloader"。

### 根因

`copy_firmware()` 直接 `HAL_FLASH_Program` 写内部 Flash，**没有先擦除 App 区**。STM32F1 的 Flash 编程只能把 bit 从 1 写成 0（相当于按位与）：

```text
旧 App 字节: 0xB4 (1011_0100)
新固件字节: 0x3C (0011_1100)
编程结果:   0xB4 & 0x3C = 0x34 (0011_0100)   ← 既不是旧也不是新
```

之前 OTA 测试之所以"能过"，是因为 `flash.sh` 每次烧录前都执行 `flash erase_address 0x08000000 0x10000` 全片擦除——App 区碰巧处于全 0xFF 擦除态，掩盖了缺失的擦除步骤。这是一个**测试流程掩盖了代码缺陷**的典型案例。

### 修复

`bootloader/Src/main.c` 的 `copy_firmware()` 在编程前按 1KB 页擦除目标区域：

```c
FLASH_EraseInitTypeDef erase = {0};
uint32_t page_err = 0;
erase.TypeErase    = FLASH_TYPEERASE_PAGES;
erase.Banks        = FLASH_BANK_1;
erase.PageAddress  = dst_addr;
erase.NbPages      = (size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
if (HAL_FLASHEx_Erase(&erase, &page_err) != HAL_OK) {
    HAL_FLASH_Lock();
    return -3;
}
```

同时修复了附带问题：固件大小为奇数时，最后一个半字的高字节会读到 `buf[]` 的越界残留值，现在固定补写 0xFF（保持擦除态）。

### 教训

> Flash 编程模型（只能 1→0）决定了"擦除→编程"是不可拆分的原子操作序列。测试时"每次全片擦除再烧录"的习惯会掩盖缺失的擦除逻辑——**代码正确性不能依赖测试流程的副作用**。回归测试必须覆盖"不擦除直接 OTA"的场景。

---

## Bug #11：OTA 协议解析器三连缺陷 — 状态机卡死 / 越界写栈 / 无帧超时（代码审查发现）

### 现象（推演）

- UART 上出现任何一个 CRC16 校验失败的包之后，后续**所有**包都无法再被解析，OTA 会话永久卡死，上位机只能等超时；
- 一个 `len > 64` 的畸形包（哪怕 CRC 恰好碰对）会让 `memcpy(tmp + 2, pkt_buf, pkt_len)` 越界写穿 66 字节的栈缓冲；
- 任何原因丢失一个字节（如上位机串口抖动），状态机停在 `PKT_WAIT_DATA` 等一个永远不会来的字节，之后整包数据被当作数据字节吞掉。

### 根因

`Pkt_ParseByte()` 的三个独立缺陷：

```c
// 缺陷 1：CRC 失败分支没有复位状态机
if (calc_crc == received_crc) {
    pkt_state = PKT_WAIT_HEADER;   // 只有成功才复位！
    return 1;
}
// CRC 错误后 pkt_state 仍是 PKT_WAIT_CRC，
// 后续每 2 个字节被当作一组"重试的 CRC"，解析器与字节流永久失步

// 缺陷 2：pkt_len 未校验上界
case PKT_WAIT_LEN:
    pkt_len = byte;                // byte 可以是 0~255，但 pkt_buf 只有 64 字节
    ...
// PKT_WAIT_DATA 里"写"有 if 保护，但 CRC 状态里 memcpy(tmp+2, pkt_buf, pkt_len)
// 用的是原始 pkt_len → tmp[66] 栈溢出

// 缺陷 3：无帧内超时
// 丢一个字节 → 永远停在中间状态等剩余字节
```

### 修复

`Core/Src/ota.c`：

1. CRC 校验**无论成败**都先回到 `PKT_WAIT_HEADER`；
2. `PKT_WAIT_LEN` 处校验 `byte > 64` 直接丢弃整包（`tmp` 缓冲也改为 `2 + PKT_DATA_MAX` 显式关联大小）；
3. 新增帧内超时：记录每个包首字节时间戳，停留在中间状态超过 500ms（115200 波特率下字节约 87µs 间隔，余量 5000 倍）即丢弃残包、从当前字节重新找帧头 `0xAA`。

### 教训

> 流式协议解析器的健壮性三要素缺一不可：**错误路径必须与成功路径对称地复位状态**、**长度字段在进入缓冲区前必须校验上界**、**中间状态必须有超时出口**。写解析器时应该问自己：每一个 `case` 的入口，是否都能从任意一个错误场景到达？

---

## Bug #12：UART 溢出错误后接收通道永久失效（代码审查发现）

### 现象（推演）

OTA 传输过程中任何一次 UART 溢出（ORE，例如上位机在设备忙于擦除 SPI Flash 时重发数据），此后设备对串口输入完全无响应，只能断电重启恢复。

### 根因

`HAL_UART_IRQHandler` 对 ORE 的处理是调用 `UART_EndRxTransfer()`：把 RxState 置回 READY 并**关闭 RXNE 中断**，然后调用 `HAL_UART_ErrorCallback()`——而工程没有实现这个回调，弱符号默认实现是空的。于是接收中断被关闭后再也没有人重新挂起 `HAL_UART_Receive_IT`，接收通道"安静地死亡"。

修复前的接收链路只处理了"正常收到字节"这一条路径，错误路径完全没有出口。

### 修复

`Core/Src/main.c` 实现错误回调，清标志后重新挂起接收：

```c
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART1) {
        __HAL_UART_CLEAR_OREFLAG(huart);
        __HAL_UART_CLEAR_NEFLAG(huart);
        __HAL_UART_CLEAR_FEFLAG(huart);
        HAL_UART_Receive_IT(&huart1, uart_rx_buf, 1);
    }
}
```

### 教训

> HAL 的回调族是成对出现的：`RxCpltCallback`（正常路径）与 `ErrorCallback`（错误路径）。只覆盖正常路径的驱动代码，在第一次总线异常时就会失效。**每个启用中断的外设，都必须回答"错误中断来了谁负责恢复"。**

---

## Bug #13：OTA 会话缺少状态与边界校验（代码审查发现）

### 现象（推演）

- 上位机直接发 DATA/END（没有 START），固件会从地址 0 写 SPI Flash 暂存区并回 ACK，破坏上一次会话的数据；
- START 包里 `fw_size` 字段被干扰成 `0xFFFFFF00` 之类的值，擦除循环按 `addr < OTA_FW_ADDR + fw_size` 判断，会把 SPI Flash 上固件区之后的所有分区（配置参数、日志）全部擦光，且每个扇区擦除 ~45ms，设备"假死"数分钟；
- DATA 包超出 56KB 上限时固件**静默丢弃**（裸 `break`，无 NACK），上位机只能干等 5 秒超时；
- `W25_WritePage` 一次写入跨越 256 字节页边界时，芯片行为是地址回卷到页首——尾部数据静默覆盖页内开头的数据（当前 64 字节对齐的分包恰好不跨页，但这是调用约定的巧合而非驱动的保证）。

### 根因

处理函数信任了协议对端：`fw_size`、会话顺序、长度对齐全都假设上位机"一定是善意的、正确的"。错误场景要么没有处理，要么处理后不通知对端。

### 修复

`Core/Src/ota.c` + `Core/Src/w25d64.c`：

1. 新增 `ota_active` 会话标志：START 置位，END/写入失败复位；**没有活跃会话的 DATA/END 一律 NACK**；
2. START 校验 `0 < fw_size <= OTA_FW_MAX_SIZE`，非法值 NACK；擦除范围收窄为"标志区 + 固件实际占用的扇区"，不再多擦也不越界；
3. END 先校验 `ota_bytes_written == ota_fw_size`（缺包不进 CRC 流程），全部错误路径显式 NACK，让上位机秒级失败而不是等超时；
4. DATA 写入失败时置 `ota_active = 0`（数据已错位，会话必须作废重传）；
5. `W25_WritePage` 内部按页边界自动拆分多次编程，驱动不再依赖调用方的对齐约定。

### 教训

> 固件侧协议实现必须把对端当作"会发任何字节序列"的黑盒：**每一个从包里解析出来的数值字段，使用前都要过一遍范围校验**；**每一个错误分支，都要让对端能感知（NACK/超时），否则故障被静默吞掉，表现为难以定位的"对端超时"**。驱动层的硬件约束（页边界、扇区对齐）应该在驱动内部消化，而不是写成对调用方的隐式要求。

---

## Bug #14：启动调试打印在 UART 初始化之前发送（代码审查发现）

### 现象

上电后串口看不到设计中的 `ID:xxxxxx FLAG:xx...` 调试输出。

### 根因

`main()` 里调试打印调用在 `MX_USART1_UART_Init()` 之前，此时 `huart1` 还是全零的静态结构体，`gState = HAL_UART_STATE_RESET ≠ READY`，`HAL_UART_Transmit` 直接返回 `HAL_BUSY`——不崩溃，但一个字节都发不出去。这类"外设句柄未初始化就使用"的错误因为 HAL 的防御性检查而静默失败，极易被忽略。

### 修复

把打印移到 `MX_USART1_UART_Init()` 之后（SPI ID 的读取保持在 DMA 初始化之前不动，只移动发送）。

### 教训

> HAL 句柄是"必须先 Init 才能用"的有状态对象，防御性检查让误用变成静默 no-op。**调试输出不工作是"外设初始化顺序错误"最便宜的探测器**，值得第一时间核对。

---

## Bug #15：IWDG 配置跨复位保留 —— 计划外的回滚加速与搬运窗口风险（上板实测发现）

### 现象

上板验收"坏固件自动回滚"：预期崩溃循环 4 × 26s ≈ 110s 触发回滚（坏固件在 main
第一行以 26s 长窗口重新武装看门狗），实测 **~20s 即完成回滚**；且 OTA 复位后的
Bootloader 搬运（2~4s）实际运行在远小于设计的看门狗窗口内。

### 根因（实测确认的 F1 硬件行为）

IWDG 一旦启动只能靠上电复位停止，且 **PR/RLR 寄存器跨系统复位保留**（仅 KR 归零）。
于是设计中的"26s 早期窗口"只在冷启动成立：

```text
App 运行期 supervisor 收紧到 RLR=1250（8s）
  → OTA END → NVIC_SystemReset
  → IWDG 仍在跑、配置仍是 8s（不是设计假设的 26s！）
  → Bootloader 搬运 43KB（2~4s）+ 坏固件启动崩溃
  → 看门狗按 8s 节奏咬人 → 回滚反而加速（4 次 × ~5s ≈ 20s）
```

对回滚而言是**朝安全方向的偏差**（更快自愈）；但对 Bootloader 搬运是**风险**：
若搬运耗时逼近窗口（慢 Flash + LSI 偏快），看门狗可能在搬运中途复位，造成
安装不完整 → 反复 PENDING 重试。

### 修复

1. `copy_firmware()` 搬运循环内逐块 `IWDG->KR = 0xAAAA` 喂狗（IWDG 未启用时写 KR 是无害 no-op，冷启动路径不受影响）；
2. App 的 OTA END 在 `NVIC_SystemReset()` 前先喂满狗，给 Bootloader 留足启动余量。

### 教训

> "看门狗从 main 第一行武装（26s 长窗口）"的设计只在**冷启动**成立——复位前
> 的 IWDG 配置会原样带过来。**任何"跨复位的安全设计"都必须问一句：还有哪些
> 硬件状态跟着复位一起活下来了？**（IWDG、RTC、备份寄存器、外设寄存器……）
> 好在偏差方向与失败模式都可通过上板验收暴露——这正是"回滚验收测试"的价值。

---

## Bug #16：OTA_SendPacket 满长响应包越界写 1 字节（host fuzz 台发现）

### 现象

fuzz 台 D6 用例（UBSan）：以 `len=64` 构造响应包时，`OTA_SendPacket` 的栈缓冲区
`uint8_t frame[4 + PKT_DATA_MAX + 2]`（70B）被写穿——满帧实为
`1 头+1 cmd+1 len+2 seq+64 data+2 crc = 71B`。

### 根因

缓冲区尺寸公式抄了"无 SEQ 的旧账"：`4+64+2` 漏算帧头。现有响应（QUERY 12B、
GET_LOG 32B）都远小于 64B，**潜伏未被触发**——与 Bug #11 的 len 越界同族：
边界不在当前调用路径上，不代表边界安全。

### 修复

`uint8_t frame[7 + PKT_DATA_MAX]`，并在 fuzz 台加 D6 回归锁死（`test/fuzz/harness.c`）。

### 教训

> 长度公式集中一处定义（`7 + PKT_DATA_MAX`），或干脆 `_Static_assert` 满 frame
> 上限。潜伏越界靠 code review 很难抓——**让满长输入成为常规测试用例**是唯一可靠的防线。

---

## Bug #17：CRC 接收进度跨包残留 —— 半包超时后错杀下一帧合法包（host fuzz 台发现）

### 现象

fuzz 台 O4 oracle（垃圾流后合法 QUERY 必须被应答）以低概率违反：紧随"超时打断
在 CRC 半包"之后的一帧合法包被静默丢弃。

### 根因

`crc_hi/crc_byte_idx` 声明在 `PKT_WAIT_CRC` case 块内（block-scope static），
`Pkt_Reset()` 够不着。超时重同步只复位了 `pkt_state/pkt_idx`，`crc_byte_idx`
残留为 1 → 下一帧的第一个 CRC 字节被当作第二个处理，与上一包的**陈旧高字节**
拼成错误 CRC → 整帧丢弃。后果被 v2 的 DATA 重传机制掩盖（重传一次即恢复），
QUERY 单发场景则表现为偶发"设备无响应"。

### 修复

两个变量移到文件作用域，`Pkt_Reset()` 一并复位（`Core/Src/ota.c`）。

### 教训

> 状态机的"复位"必须覆盖**该状态机的全部状态**——凡 `static` 参与解析，
> 就要问"重同步路径把它们都归零了吗"。块内 static 藏在 case 里最容易漏。

---

## Bug #18：TESTING 回滚门槛只认 IWDG 复位原因 —— 回滚搬运途中断电可永久变砖（模型检查发现）

### 现象

`tools/model_check.py --variant old` 穷举 200 个断电场景，命中唯一 BRICK 路径：
坏固件崩溃 4 次（retry=4）→ Bootloader 开始金固件回滚、擦除内部 Flash →
**恰在搬运途中掉电** → 重新上电（POR）→ IWDG 配置被清、复位原因不是 IWDG →
旧门槛直接跳进半擦除的内部 Flash → 无看门狗可救 → 永久挂死（只能 SWD）。

### 根因

`TESTING` 分支的回滚触发条件是 `reset_by_iwdg` 单项——它隐含假设"回滚动作
一旦开始必然完成"。而回滚本身是多原子操作（擦内部→编程→写 flag），任何一步
断电都会让"已超限的 retry_cnt"与"非 IWDG 的复位原因"同时出现，假设破裂。

### 修复

门槛放宽为 `reset_by_iwdg || retry_cnt > OTA_ROLLBACK_LIMIT`：retry 超限本身就是
"此前已有 4 次看门狗复位的既定事实"，与本次复位原因无关；回滚搬运是幂等操作
（槽 B 完整、每次全量重拷），途中断电重启后继续回滚直至收敛。
修复后同口径穷举 264 个场景 **0 BRICK**（`--variant fixed`）。

### 教训

> "复位原因"是**一次性证据**（读后即焚），不能当作持久状态用。任何
> "A 事件后必然完成 B"的设计，都欠一个"B 中途被打断"的答案——
> 这正是穷举模型检查比测试用例强的地方：它不遗漏你没想起来的组合。

---

## Bug #19：上位机把最小帧当 8 字节 —— 空槽响应永远解析不出，log 扫描必死在数据末尾（真板取证发现）

### 现象

`ota_tool.py log` 扫描黑匣子，每次都在同一条记录之后"设备无响应（重试后仍超时），
中止扫描"（HANDOFF good2 调查记录了两次）。表面证据像"扫描期间设备复位"，
引出一条错误假说。

### 根因

`read_packet_v2` 的同步循环条件 `while len(buf) >= 8`，但协议最小帧是
**7 字节**（`AA CMD LEN SEQ(2) CRC(2)`，0 载荷）。GET_LOG 对无效槽的响应恰是
7 字节空包——设备每次都正确应答，上位机永远不进入解析分支，干等到超时。
有效记录（32B 载荷 = 39B 帧）解析正常，于是扫描**每次都恰好死在第一条空槽
（即数据末尾）**，完美伪装成"设备中途出事"。

### 修复

`len(buf) >= 7`（tools/ota_tool.py）。修复后 128 槽全量扫描一次通过，
并顺带证实黑匣子没有丢失的记录——"可能还有未取到的记录"的担忧解除。

### 教训

> "无响应"有两解：设备没说话，或者**你听不见**。协议最小帧是解析器最容易被
> 忽略的边界（没有数据域的帧最容易漏算）。排障时先用逻辑分析仪/回环确认
> 对端确实发了字节，再怀疑链路对端——这次真凶在自家解析器里。

---

## Bug #20：Bootloader 跟踪波特率按 72MHz 计算 —— 实际时钟是 64MHz，跟踪输出全是乱码（上板验证发现）

### 现象

BOOT_TRACE 首次上板：App banner 清晰，Bootloader 的 `BT ...` 决策行输出为
约 30 字节乱码。

### 根因

bootloader 的 `SystemClock_Config` 用 **HSI/2×16 = 64MHz**（无外部晶振），
而 trace_init 的 BRR 按"惯常的 72MHz"写了 625；正确值 64000000/115200≈556，
波特率偏差 12.5%，远超 UART 容差。App 没踩坑是因为 CubeMX 生成的 App 时钟
（HSE×9=72MHz）与它的 BRR 恰好一致——**两份代码各自配时钟，一份惯性数字
跨了上下文就错了**。

### 修复

`USART1->BRR = 556`（bootloader/Src/main.c，注明 PCLK2=64MHz 出处）。
修复后输出 `BT rst=14000000 id=00207017 st=01 ry=00000000 jmp` 完全可读。

### 教训

> BRR 永远从"本模块实际使用的时钟树"推导，不抄别的模块的数字。
> 上板第一眼验证输出可读性，是所有"可观测性"功能自己的第一步验收。

---

## Bug #21：回滚保护依赖候选固件自己武装看门狗 —— "装得进但起不来"的镜像可绕过回滚（上板发现）

### 现象

v3 HIL 验收：坏固件上传安装后设备既不崩溃循环也不回滚，永久静默挂死（串口
无 banner、QUERY 无响应、黑匣子不再更新）；openocd 读现场：PC 在 App 区乱序
执行、SP 正常、Thread 模式，**IWDG 寄存器为复位默认值（PR=0/RLR=0xFFF）**——
看门狗从未武装，retry 永不增长，回滚条件永远不满足。

### 根因

自动回滚的触发链是"候选固件崩溃 → IWDG 复位 → retry++"。该链的前提
**"候选固件会武装 IWDG"是把自己的安全托付给了被判定为不可信的对象**：
一个被破坏的/构建基址错误的镜像完全可以既不武装看门狗、也不触发 HardFault，
而是安静挂死——保护机制整体失效，唯一出路 SWD。（本次直接诱因是 HIL 喂了
一个旧链接基址的坏镜像构建——错误本身是操作失误，但它精准打穿了设计盲区。）

### 修复

Bootloader 在跳入 **TESTING 候选**前替它武装 26s 长窗 IWDG（bootloader/Src/main.c）：
正常固件毫秒级由 supervisor 接管喂狗，不受影响；安静挂死的候选 26s 内必被咬
→ IWDG 复位 → retry++ → 超限回滚。IDLE 跳转不武装，"用户复位不计入回滚"的
语义保持不变。修复后同场景 HIL 9/9 通过（坏镜像上传→挂死→26s 兜底咬人→
×4→自动回滚→版本恢复）。

### 教训

> 自愈机制的前提不能建立在"故障对象会配合"之上——保护必须由**故障对象
> 之外**的层兜底（这是看门狗的经典用法，也是"不可信代码沙箱"的通用原则）。
> 另：验收用的"坏固件"应该尽量接近真实世界的坏（错基址/半镜像/乱序），
> 而不只是最体面的那种坏（写空指针）。

---

## Bug #22：光照状态机 IDEAL↔GLARE 迟滞接反 —— ADC 落在 (800,1000) 必然无限跳变（用户报告）

### 现象

设备状态在 GLARE 与 IDEAL 之间持续来回跳变（OLED 显示抖动；`today_glare_sec`
与 `today_ideal_sec` 两个统计互相蚕食）。与环境噪声无关：给定恒定光照也照跳。

### 根因（两处叠加）

1. **迟滞接反**：`STATE_IDEAL: if (adc < IDEAL_DOWN=1000) → GLARE`，而
   `STATE_GLARE: if (adc > GLARE_EXIT=800) → IDEAL`。进入阈值 1000 高于离开
   阈值 800 → 区间 **(800, 1000) 双向条件同时成立**：在 IDEAL 判"低于 1000 该
   去 GLARE"，到了 GLARE 又判"高于 800 该回 IDEAL"——去抖计数每 10 拍翻一次，
   无限 ping-pong。对照同文件另两对（DARK↔DIM：2800/3000；DIM↔IDEAL：
   1200/1600）都是正确的"外宽内窄"，唯独这一对接反。
2. **目标粘滞（潜伏）**：`state_target` 在 if-else 链无分支命中时保留上一次的
   瞬态值，而去抖计数按"调用次数"累加而非"条件连续命中次数"——单个噪声尖峰
   也能在 10 拍后推翻状态。仿真：IDEAL 稳定 1010，中间夹一拍 790，状态仍会
   误入 GLARE 并停留 11 拍。

### 修复

- `IDEAL→GLARE` 改用 `GLARE_EXIT(800)`、`GLARE→IDEAL` 改用 `IDEAL_DOWN(1000)`：
  死区 [800,1000]，与另两对同构（Core/Src/main.c）
- 每次评估先 `state_target = state_now`，只有条件**持续**命中才累加计数

### 验证（host 仿真 + 真板部署）

- 恒定 ADC=900（错接区间）2000 拍：修复前 **181 次切换** → 修复后 **0 次**
- 正常过渡仍工作：<800 进 GLARE、>1000 回 IDEAL；三对死区边界
  （2799/3001、1199/1601、799/1001）逐一双侧复核通过
- 单尖峰场景：误入 GLARE 11 拍 → 0 拍
- 修复固件已 OTA 部署（签名链路 + 确认启动全通过）

### 教训

> 多状态分类器的每一对相邻状态都要做"**阈值有序性**"检查：进入某状态的
> 阈值必须严格落在离开它的阈值之外（Schmitt 触发器的充要条件）。三对里两对
> 写对了不代表第三对也对——这类缺陷表现为"抖动"，极易被误判成传感器噪声而
> 去调滤波参数。迟滞死区值得一条仿真/单测守护（本 Bug 的仿真脚本可直接复用）。

---

## Debug 方法论总结

| 方法 | 工具 | 适用场景 | 实际案例 |
|------|------|----------|----------|
| 快速状态检测 | `openocd -c "reg pc"` | 不确定 MCU 卡在哪 | 确认 Bootloader 是在运行还是在 HardFault |
| 异常帧分析 | `openocd -c "mdw <MSP> 8"` | HardFault 后还原崩溃现场 | 从 LR/PC 定位到 `__libc_init_array` |
| 源码定位 | `arm-none-eabi-addr2line` | PC/LR 地址反查源码行号 | `0x08003463` → `__libc_init_array` at init.c:44 |
| 指令级分析 | `arm-none-eabi-objdump -d` | 确认编译器生成的指令序列 | 发现 `push/pop` 导致的栈越界 |
| ELF 结构检查 | `arm-none-eabi-readelf -l/-S` | 检查段地址、LOAD 映射 | 发现 PhysAddr=0x08000000 vs VMA=0x08002000 |
| 内存内容验证 | `openocd -c "mdw <addr>"` | 验证 Flash 数据正确性 | 确认 .init_array 内容正确，排除数据损坏 |
| 远程断点调试 | `gdb-multiarch` + OpenOCD | 需要单步跟踪时 | 跟踪 jump_to_app 的执行过程 |
| 排除法 | GDB 手动设置寄存器 | 隔离问题范围 | 确认 App 单独运行正常 |
| 海森堡效应识别 | 反汇编分析 | 断点能跑但正常不行 | push/pop 时序依赖 |
| 硬件隔离测试 | 拔线/直连/加电容 | 排除信号完整性问题 | 面包板 vs 杜邦线直连 |
