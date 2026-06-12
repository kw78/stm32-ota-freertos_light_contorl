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

> **Cortex-M Bootloader 跳转是嵌入式领域最经典的 Bug 之一。** 核心问题：C 编译器为普通函数生成的 prologue（`push {r4}`）和 epilogue（`pop {r4}`）假定整个函数执行期间栈指针不变。但 `jump_to_app` 需要在函数执行中途修改 MSP——这违反了编译器的假设。`__attribute__((naked))` 是 C 语言提供的唯一解决方案，它告诉编译器"不要帮我生成任何序言和尾声"，让开发者完全控制指令序列。这在 ARM Cortex-M 的 Bootloader、RTOS 上下文切换、中断返回等场景中是**必须掌握的技巧**。

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
2. **去耦电容**：在 W25D64 的 VCC-GND 之间焊接 100nF 陶瓷电容，紧贴芯片引脚
3. **降低 SPI 时钟**：从 16MHz 降至 4MHz 作为备选方案（时钟频率越低，对信号完整性要求越低）

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
