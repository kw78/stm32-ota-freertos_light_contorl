# 开发过程中遇到的 Bug 和经验记录

## 1. SPI 发送 uint32_t 字节序反了（小端序陷阱）

**现象：** W25D64 Flash 读 ID 返回 0x000000 或 0xFFFFFF，SPI 通信不通。

**原因：** 把命令和地址拼成 `uint32_t` 后直接按字节发送：

```c
// 错误写法
uint32_t cmd = 0x03001234;  // 命令 0x03 + 地址 0x001234
HAL_SPI_Transmit(&hspi2, &cmd, 4, 100);
```

ARM Cortex-M3 是**小端序**（Little-Endian），`uint32_t` 在内存中的布局是低字节在前：

```
内存地址:  &cmd+0  &cmd+1  &cmd+2  &cmd+3
期望发出:  0x03    0x00    0x12    0x34    (命令在前)
实际发出:  0x34    0x12    0x00    0x03    (低字节在前，命令跑最后了)
```

Flash 收到的第一个字节是 `0x34`，不是命令 `0x03`，所以完全无法通信。

**正确写法：** 用字节数组，手动控制字节顺序：

```c
uint8_t cmd[4] = {
    0x03,                  // 命令
    (addr >> 16) & 0xFF,  // 地址高字节
    (addr >>  8) & 0xFF,  // 地址中字节
    (addr      ) & 0xFF   // 地址低字节
};
HAL_SPI_Transmit(&hspi2, cmd, 4, 100);
```

**教训：** SPI/I2C/UART 等串行协议都是按字节流发送的，永远不要把多字节数据拼成 `uint32_t` 后直接发送，必须用字节数组控制顺序。

---

## 2. FreeRTOS 和 HAL 抢 SysTick

**现象：** 加入 FreeRTOS 后 `HAL_Delay()` 卡死或 Tick 不走。

**原因：** FreeRTOS 的 port.c 硬编码使用 SysTick 作为 Tick 中断源，HAL 默认也用 SysTick 做 `HAL_GetTick()`/`HAL_Delay()`。两者冲突。

**解决：** 在 CubeMX 里把 SYS → Timebase Source 改成 **TIM6**（或 TIM4），让 HAL 用独立定时器，SysTick 留给 FreeRTOS。

**教训：** FreeRTOS + HAL 的项目，第一步就是改 HAL Timebase，必须在添加 FreeRTOS 之前做。

---

## 3. DMA 中断优先级不能调用 FreeRTOS API

**现象：** 在 ADC DMA 完成回调里调用 `osSemaphoreRelease()` 后程序卡死或 HardFault。

**原因：** Cortex-M3 的规则：中断优先级 ≤ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`（默认 5）的中断里，不能调用 FreeRTOS API。DMA 中断优先级恰好是 5，等于阈值，属于禁止范围。

**解决：** 在 CubeMX NVIC 里把 DMA 中断优先级改成 **6**（数值越大优先级越低），使其 > 5，就可以安全调用 FreeRTOS API。

**教训：** FreeRTOS 项目里，凡是需要在中断中调用 `osXxxFromISR()` 的中断，其优先级数值必须大于 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`。

---

## 4. CubeMX 重新生成代码覆盖用户文件

**现象：** 每次在 CubeMX 改配置后重新 Generate Code，CMakeLists.txt 被覆盖或删除，用户添加的源文件（oled.c、MPU6050.c）丢失。

**原因：** CubeMX 的 `stm32cubemx/CMakeLists.txt` 会重新生成，但主 `CMakeLists.txt` 可能被重置。用户通过 `target_sources` 添加的自定义源文件会被清除。

**解决：** 每次 CubeMX 重新生成后，检查 `CMakeLists.txt` 里的 `target_sources` 是否还在，手动补回自定义源文件。

**教训：** CubeMX 只管理它自己生成的文件，用户的自定义文件需要手动维护。

---

## 5. volatile 在中断和任务间的必要性

**现象：** 中断里修改的变量，在任务里读到的值不更新。

**原因：** 编译器优化时，如果发现一个变量在当前循环里没有被修改，会把它缓存到 CPU 寄存器里，不再从内存重新读取。中断在另一个上下文里修改了内存中的值，但任务读的还是寄存器里的旧值。

**解决：** 在中断和任务之间共享的变量加 `volatile` 关键字：

```c
volatile LightState_t state_now;  // 告诉编译器：每次都从内存读，别优化
```

**教训：** `volatile` 不是给程序员看的，是给编译器看的。凡是"一个上下文写、另一个上下文读"的全局变量，都需要 `volatile`。

---

## 6. extern 声明的必要性

**现象：** 在 freertos.c 里引用 main.c 里定义的变量，编译报 "undefined reference"。

**原因：** C 语言中每个 `.c` 文件是独立的编译单元，互相看不到对方的变量。`extern` 告诉编译器"这个变量定义在别的文件里，链接器会找到它"。

**解决：** 在需要使用的文件里加 `extern` 声明：

```c
extern uint16_t adc_buf[];           // 变量在 main.c 里定义
extern volatile LightState_t state_now;
void LightState_Update(uint16_t v);  // 函数默认 extern，不加也行
```

**例外：** typedef 不能 extern，必须放在头文件（如 main.h）里，通过 `#include` 共享。

**教训：** 跨文件共享的类型定义放 `.h` 文件，变量用 `extern` 声明，函数不用加 `extern`（默认就是 extern）。

---

