# AI 辅助从零搭建 STM32 现代开发环境

## 起点：为什么放弃 Keil IDE？

作为一个嵌入式开发者，我最初也是 Keil MDK 的用户。但随着项目迭代，几个痛点越来越突出：

- **平台锁定**：只能在 Windows 上使用，无法利用 Linux 生态
- **版本管理困难**：.uvprojx 是二进制文件，无法做有意义的 git diff
- **自动化受限**：命令行编译、CI/CD 几乎不可能
- **调试体验**：Keil 的调试器功能有限，无法与 VS Code 深度集成

于是决定迁移到 **WSL2 + CMake + GCC + OpenOCD + GDB** 这套现代工具链。

## AI 的角色：我的"结对编程"伙伴

这个项目是我第一次从零搭建 STM32 的 CMake 工程，在关键环节 AI（Claude Code / Hermes）帮了大忙：

### 1. 构建系统：CMakePresets.json

传统的 CMake 方案需要手动指定工具链、传递 `-DCMAKE_TOOLCHAIN_FILE` 参数。AI 向我推荐了 **CMakePresets** 的方案——把编译配置声明式地写在 `CMakePresets.json` 中，一键 `cmake --preset debug` 即可配置，不同机器之间也便于复用：

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "debug",
      "displayName": "Debug",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/Debug",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_TOOLCHAIN_FILE": "${sourceDir}/cmake/gcc-arm-none-eabi.cmake"
      }
    }
  ]
}
```

### 2. 烧录脚本：flash.sh

AI 帮我生成了基于 OpenOCD 的烧录脚本，包含：
- 自动检查参数合法性
- 搜索 OpenOCD 脚本路径（解决 WSL2 下路径差异）
- 启动 OpenOCD 后台烧录并通过 PID 文件管理进程生命周期
- 用 `wait` 替代硬编码 `sleep 2` 修复时序竞态 bug

```bash
openocd -f "$BOARD_CFG" -c "init; program \"$ELF\" verify reset exit" &
OCD_PID=$!
wait $OCD_PID 2>/dev/null  # 这里 debug 过时序问题
```

### 3. udev 规则与调试配置

AI 帮我解释了 JLINK 和 ST-LINK 的 udev 规则配置，并生成了 VS Code 的 `launch.json` 和 `tasks.json`，实现一键编译 + 烧录 + GDB 调试的完整工作流。

### 4. 修复 DIM_UP 阈值 bug

在 `main.c` 的 `DimUp()` 函数中，AI 帮我识别出低亮度跳变阈值设置过高的时序问题，将其从 4 调整到 5，解决了视觉上亮度不平滑的问题。

## 我的主导权：关键决策都是我做的

AI 提供的是"工具箱"和"咨询"，而技术决策始终由我掌控：

| 决策 | 我的选择 | 被否决的方案 |
|------|----------|-------------|
| 虚拟化方案 | **WSL2**（原生 Linux 内核、VS Code 深度集成） | VMware（资源开销大、与宿主机交互不自然） |
| 构建配置 | **CMake Presets**（声明式、跨机器可复现） | 手动 Kit 选择（易错、不可复现） |
| 版本回退策略 | **git reset --hard** 回退到可工作版本 | 逐行 revert（耗时，引入新 bug 风险高） |
| CMSIS 依赖管理 | **STM32CubeMX 生成 + CMake FetchContent** | 手动复制（无法版本化、更新困难） |

## 最终成果

一套 **可复用、跨平台、版本化** 的嵌入式开发环境

**一键编译**：`cmake --preset debug && cmake --build build/Debug`

**一键烧录 + 调试**：VS Code F5 或命令行 `./flash.sh`

**完整的 GDB 调试能力**：断点、单步、变量监视、外设寄存器查看

项目代码和工具链配置全部纳入 **Git 版本管理**

## 写在最后

这次从 Keil 到开源工具链的迁移，如果靠传统方式查手册、翻论坛，可能需要一两周。借助 AI 辅助，我只用了一个周末就完成了从零搭建到可用的完整流程。

AI 不是替代开发者的"银弹"，而是一个 7×24 小时在线的结对编程伙伴——它擅长解决"怎么做"的问题，而"做什么"和"为什么这样做"的决策权，始终握在开发者自己手中。