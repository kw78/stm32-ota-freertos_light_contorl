#!/bin/bash
#
# flash.sh - STM32 一键烧录脚本
#
# 功能:
#   - 构建 + 烧录一步完成
#   - VS Code 调试配置自动生成
#   - ST-Link udev 规则安装
#   - WSL usbipd 自动挂载
#
# 用法:
#   ./flash.sh                              烧录 Debug 版
#   ./flash.sh Release                      烧录 Release 版
#   ./flash.sh path/to/firmware.elf         烧录指定 ELF 文件
#   ./flash.sh --gdb                        烧录 + 启动 GDB 调试
#   ./flash.sh --gdb Release                烧录 Release + 启动 GDB
#   ./flash.sh --gdb path/to/firmware.elf   烧录指定文件 + GDB
#   ./flash.sh --init                初始化项目配置（CMake + .vscode + .gitignore）
#   ./flash.sh --install-udev               安装 ST-Link udev 规则
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ========================
# 内嵌 udev 规则
# ========================
STLINK_UDEV_RULES='# ST-Link V1
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="3744", MODE="0666", GROUP="plugdev"
# ST-Link V2
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="3748", MODE="0666", GROUP="plugdev"
# ST-Link V2.1
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="374b", MODE="0666", GROUP="plugdev"
# ST-Link V3
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="3752", MODE="0666", GROUP="plugdev"
'

# ========================
# 从项目文件读取配置
# ========================
read_project_config() {
    PROJECT_NAME=""
    CHIP_DEFINE=""
    CHIP_DEVICE=""
    OPENOCD_TARGET=""
    OPENOCD_INTERFACE="stlink"
    # 从 CMakeLists.txt 读取项目名
    if [ -f "${SCRIPT_DIR}/CMakeLists.txt" ]; then
        PROJECT_NAME=$(grep -oP 'set\(CMAKE_PROJECT_NAME\s+\K\w+' "${SCRIPT_DIR}/CMakeLists.txt" || true)
    fi
    if [ -z "$PROJECT_NAME" ]; then
        PROJECT_NAME=$(basename "$SCRIPT_DIR")
    fi
    # 从 CubeMX CMakeLists.txt 读取芯片 define（如 STM32F103xB）
    local MX_CMAKE="${SCRIPT_DIR}/cmake/stm32cubemx/CMakeLists.txt"
    if [ -f "$MX_CMAKE" ]; then
        CHIP_DEFINE=$(grep -oP 'STM32F[0-9]+[a-zA-Z]+' "$MX_CMAKE" | head -1 || true)
    fi
    if [ -z "$CHIP_DEFINE" ]; then
        CHIP_DEFINE="STM32F103xB"
    fi
    # 从 .ioc 读取完整芯片型号（用于 OpenOCD device）
    local IOC_FILE
    IOC_FILE=$(find "${SCRIPT_DIR}" -maxdepth 1 -name "*.ioc" -print -quit 2>/dev/null)
    if [ -f "$IOC_FILE" ]; then
        CHIP_DEVICE=$(grep -oP 'Mcu\.CPN=\K\S+' "$IOC_FILE" || true)
        # 推断 OpenOCD target（STM32 系列）
        local FAMILY
        FAMILY=$(grep -oP 'Mcu\.Family=\K\S+' "$IOC_FILE" || true)
        if [[ "$FAMILY" == "STM32F1" ]]; then
            OPENOCD_TARGET="stm32f1x"
        elif [[ "$FAMILY" == "STM32F4" ]]; then
            OPENOCD_TARGET="stm32f4x"
        elif [[ "$FAMILY" == "STM32L4" ]]; then
            OPENOCD_TARGET="stm32l4x"
        elif [[ "$FAMILY" == "STM32G4" ]]; then
            OPENOCD_TARGET="stm32g4x"
        elif [[ "$FAMILY" == "STM32H7" ]]; then
            OPENOCD_TARGET="stm32h7x"
        else
            OPENOCD_TARGET="stm32f1x"
        fi
    fi
    if [ -z "$CHIP_DEVICE" ]; then
        CHIP_DEVICE="STM32F103C8"
    fi
    if [ -z "$OPENOCD_TARGET" ]; then
        OPENOCD_TARGET="stm32f1x"
    fi
}

# ========================
# 安装 udev 规则
# ========================
install_udev_rules() {
    local rules_file="/etc/udev/rules.d/99-stlink.rules"
    echo "正在安装 ST-Link udev 规则到 ${rules_file}..."
    if [ "$(id -u)" -ne 0 ]; then
        echo "需要 root 权限，正在通过 sudo 重新执行..."
        exec sudo "$0" --install-udev
    fi
    echo "$STLINK_UDEV_RULES" > "$rules_file"
    udevadm control --reload-rules
    udevadm trigger
    echo "ST-Link udev 规则安装成功。"
    exit 0
}

# ========================
# 初始化项目配置（.vscode + .gitignore + CMake）
# ========================
init_vscode_config() {
    read_project_config

    echo "========================================"
    echo " 初始化项目配置"
    echo "========================================"
    echo " 项目名: ${PROJECT_NAME}"
    echo "========================================"

    # 生成 CMakeLists.txt（如果不存在）
    local CMAKE_FILE="${SCRIPT_DIR}/CMakeLists.txt"
    if [ ! -f "$CMAKE_FILE" ]; then
        echo "正在生成 ${CMAKE_FILE}..."
        cat > "$CMAKE_FILE" << CMAKELISTS
cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_PROJECT_NAME ${PROJECT_NAME})
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
project(\${CMAKE_PROJECT_NAME})
message("Build type: " \${CMAKE_BUILD_TYPE})

enable_language(C ASM)
add_executable(\${CMAKE_PROJECT_NAME})
add_subdirectory(cmake/stm32cubemx)

target_link_directories(\${CMAKE_PROJECT_NAME} PRIVATE)

target_sources(\${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
)

target_include_directories(\${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

target_compile_definitions(\${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

target_link_libraries(\${CMAKE_PROJECT_NAME}
    stm32cubemx
)
CMAKELISTS
    fi

    # 生成 CMakePresets.json（如果不存在）
    local PRESETS_FILE="${SCRIPT_DIR}/CMakePresets.json"
    if [ ! -f "$PRESETS_FILE" ]; then
        echo "正在生成 ${PRESETS_FILE}..."
        cat > "$PRESETS_FILE" << 'PRESETSJSON'
{
    "version": 6,
    "configurePresets": [
        {
            "name": "Debug",
            "displayName": "Debug",
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/build/Debug",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Debug",
                "CMAKE_TOOLCHAIN_FILE": "${sourceDir}/cmake/gcc-arm-none-eabi.cmake"
            }
        },
        {
            "name": "Release",
            "displayName": "Release",
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/build/Release",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Release",
                "CMAKE_TOOLCHAIN_FILE": "${sourceDir}/cmake/gcc-arm-none-eabi.cmake"
            }
        }
    ]
}
PRESETSJSON
    fi

    mkdir -p "${SCRIPT_DIR}/.vscode"

    # 处理 extensions.json
    local EXT_FILE="${SCRIPT_DIR}/.vscode/extensions.json"
    local HAS_CORTEX=false
    if [ -f "$EXT_FILE" ] && grep -qi "cortex-debug" "$EXT_FILE"; then
        HAS_CORTEX=true
    fi

    if [ "$HAS_CORTEX" = false ]; then
        echo "正在更新 ${EXT_FILE} 以推荐 Cortex-Debug 扩展..."
        if [ -f "$EXT_FILE" ]; then
            python3 -c "
import json
with open('$EXT_FILE') as f:
    data = json.load(f)
if 'recommendations' not in data:
    data['recommendations'] = []
if 'marus25.cortex-debug' not in data['recommendations']:
    data['recommendations'].append('marus25.cortex-debug')
with open('$EXT_FILE', 'w') as f:
    json.dump(data, f, indent=4)
" 2>&1 || echo "警告: 更新 extensions.json 失败，请手动安装 Cortex-Debug"
        else
            cat > "$EXT_FILE" << 'EXTJSON'
{
    "recommendations": [
        "marus25.cortex-debug"
    ]
}
EXTJSON
        fi
        echo "已添加 Cortex-Debug 到工作区推荐扩展。"
    fi

    # 生成 settings.json
    local SETTINGS_FILE="${SCRIPT_DIR}/.vscode/settings.json"
    echo "正在生成 ${SETTINGS_FILE}..."
    cat > "$SETTINGS_FILE" << 'SETTINGSJSON'
{
    "cmake.useCMakePresets": "always",
    "cmake.configureOnOpen": true,
    "cmake.configureOnEdit": true,
    "cmake.automaticReconfigure": true,
    "cmake.buildBeforeRun": true,
    "cmake.parallelJobs": 0,
    "editor.formatOnSave": true,
    "editor.defaultFormatter": "ms-vscode.cpptools",
    "C_Cpp.clang_format_style": "file",
    "C_Cpp.clang_format_fallbackStyle": "WebKit",
    "editor.tabSize": 4,
    "editor.insertSpaces": true,
    "files.associations": {
        "*.c": "c",
        "*.h": "c"
    },
    "C_Cpp.errorSquiggles": "enabled"
}
SETTINGSJSON

    # 生成 c_cpp_properties.json（IntelliSense 直接读 build 目录，不复制）
    local CCPP_FILE="${SCRIPT_DIR}/.vscode/c_cpp_properties.json"
    echo "正在生成 ${CCPP_FILE}..."
    cat > "$CCPP_FILE" << CCPPJSON
{
    "configurations": [
        {
            "name": "STM32",
            "compileCommands": "\${workspaceFolder}/build/Debug/compile_commands.json",
            "includePath": [
                "\${workspaceFolder}/Core/Inc",
                "\${workspaceFolder}/Drivers/STM32F1xx_HAL_Driver/Inc",
                "\${workspaceFolder}/Drivers/STM32F1xx_HAL_Driver/Inc/Legacy",
                "\${workspaceFolder}/Drivers/CMSIS/Device/ST/STM32F1xx/Include",
                "\${workspaceFolder}/Drivers/CMSIS/Include",
                "\${workspaceFolder}/Middlewares/Third_Party/FreeRTOS/Source/include",
                "\${workspaceFolder}/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2",
                "\${workspaceFolder}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM3"
            ],
            "defines": [
                "USE_HAL_DRIVER",
                "${CHIP_DEFINE}"
            ],
            "compilerPath": "/usr/bin/arm-none-eabi-gcc",
            "cStandard": "c11",
            "intelliSenseMode": "linux-gcc-arm"
        }
    ],
    "version": 4
}
CCPPJSON

    # 生成 launch.json
    local LAUNCH_FILE="${SCRIPT_DIR}/.vscode/launch.json"
    echo "正在生成 ${LAUNCH_FILE}..."

    local GDB_PATH
    GDB_PATH=$(command -v arm-none-eabi-gdb || command -v gdb-multiarch || echo "arm-none-eabi-gdb")
    local GCC_DIR
    GCC_DIR=$(dirname "$(command -v arm-none-eabi-gcc 2>/dev/null)" 2>/dev/null || echo "")

    cat > "$LAUNCH_FILE" << LAUNCHJSON
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "STM32 Debug (${PROJECT_NAME})",
            "type": "cortex-debug",
            "request": "launch",
            "cwd": "\${workspaceFolder}",
            "executable": "\${workspaceFolder}/build/Debug/${PROJECT_NAME}.elf",
            "servertype": "openocd",
            "device": "${CHIP_DEVICE}",
            "interface": "swd",
            "runToMain": true,
            "preLaunchTask": "build-debug",
            "configFiles": [
                "interface/${OPENOCD_INTERFACE}.cfg",
                "target/${OPENOCD_TARGET}.cfg"
            ],
            "svdFile": "",
            "gdbPath": "${GDB_PATH}",
            "armToolchainPath": "${GCC_DIR}"
        }
    ]
}
LAUNCHJSON

    # 生成 tasks.json
    local TASKS_FILE="${SCRIPT_DIR}/.vscode/tasks.json"
    echo "正在生成 ${TASKS_FILE}..."

    cat > "$TASKS_FILE" << 'TASKSJSON'
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "build-debug",
            "type": "shell",
            "command": "cmake --build build/Debug",
            "group": {
                "kind": "build",
                "isDefault": true
            },
            "problemMatcher": [
                "$gcc"
            ]
        }
    ]
}
TASKSJSON

    # 生成 .gitignore
    local GITIGNORE_FILE="${SCRIPT_DIR}/.gitignore"
    if [ ! -f "$GITIGNORE_FILE" ]; then
        echo "正在生成 ${GITIGNORE_FILE}..."
        cat > "$GITIGNORE_FILE" << 'GITIGNORE'
# Build
build/
compile_commands.json

# Binary
*.elf
*.bin
*.hex
*.map
*.o
*.d
*.su

# OS
.DS_Store
GITIGNORE
    fi

    echo ""
    echo "========================================"
    echo " VS Code 配置已生成！"
    echo "========================================"
    echo ""
    echo " 已创建以下文件:"
    [ -f "$CMAKE_FILE" ] && echo "   ${CMAKE_FILE}"
    [ -f "$PRESETS_FILE" ] && echo "   ${PRESETS_FILE}"
    echo "   ${SETTINGS_FILE}"
    echo "   ${CCPP_FILE}"
    echo "   ${LAUNCH_FILE}"
    echo "   ${TASKS_FILE}"
    echo "   ${EXT_FILE} (已更新)"
    [ -f "$GITIGNORE_FILE" ] && echo "   ${GITIGNORE_FILE}"
    echo ""
    echo " 接下来:"
    echo "   1. cmake --preset Debug  (配置项目)"
    echo "   2. 安装 Cortex-Debug 扩展"
    echo "   3. 按 F5 开始调试!"
    echo "========================================"

    exit 0
}

# ========================
# 查找 GDB 可执行文件
# ========================
find_gdb() {
    if command -v arm-none-eabi-gdb &>/dev/null; then
        echo "arm-none-eabi-gdb"
    elif command -v gdb-multiarch &>/dev/null; then
        echo "gdb-multiarch"
    else
        echo ""
    fi
}

# ========================
# 参数解析
# ========================
GDB_MODE=false

case "$1" in
    --install-udev)
        install_udev_rules
        ;;
    --init|-i)
        init_vscode_config
        ;;
    --gdb)
        GDB_MODE=true
        shift
        ;;
    --help|-h)
        echo "用法:"
        echo "  ./flash.sh                              烧录 Debug 版 (默认)"
        echo "  ./flash.sh Release                      烧录 Release 版"
        echo "  ./flash.sh path/to/firmware.elf         烧录指定 ELF 文件"
        echo "  ./flash.sh --gdb                        烧录 + 启动 GDB 调试"
        echo "  ./flash.sh --gdb Release                烧录 Release + GDB"
        echo "  ./flash.sh --gdb path/to/firmware.elf   烧录指定文件 + GDB"
        echo "  ./flash.sh --init                初始化项目配置（CMake + .vscode + .gitignore）"
        echo "  ./flash.sh --install-udev               安装 ST-Link udev 规则"
        echo ""
        echo "GDB 命令行调试:"
        echo "  1. 脚本会自动启动 OpenOCD GDB 服务器"
        echo "  2. 支持 arm-none-eabi-gdb 和 gdb-multiarch"
        echo "  3. 连接到 localhost:3333"
        echo ""
        echo "VS Code 图形化调试:"
        echo "  1. 运行 ./flash.sh --init  生成调试配置"
        echo "  2. 安装 Cortex-Debug 扩展"
        echo "  3. 按 F5 开始调试"
        exit 0
        ;;
esac

read_project_config

BUILD_TYPE="${1:-Debug}"

if [ -z "$1" ]; then
    ELF_FILE="${SCRIPT_DIR}/build/Debug/${PROJECT_NAME:-gcctest}.elf"
elif [[ "$1" == "Release" || "$1" == "Debug" ]]; then
    BUILD_TYPE="$1"
    ELF_FILE="${SCRIPT_DIR}/build/${BUILD_TYPE}/${PROJECT_NAME:-gcctest}.elf"
else
    ELF_FILE="$1"
fi

# 检查 ELF 文件是否存在
if [ ! -f "${ELF_FILE}" ]; then
    echo "错误: ELF 文件未找到: ${ELF_FILE}"
    echo ""
    echo "用法:"
    echo "  ./flash.sh                              烧录 Debug 版"
    echo "  ./flash.sh Release                      烧录 Release 版"
    echo "  ./flash.sh path/to/firmware.elf         烧录指定 ELF 文件"
    echo "  ./flash.sh --init                初始化项目配置（CMake + .vscode + .gitignore）"
    echo "  ./flash.sh --install-udev               安装 udev 规则"
    exit 1
fi

echo "========================================"
echo " 烧录 STM32 固件"
echo "========================================"
echo " 构建类型: ${BUILD_TYPE}"
echo " ELF 文件 : ${ELF_FILE}"
echo "========================================"

# ========================
# WSL 下自动挂载 ST-Link
# ========================
detect_stlink() {
    lsusb 2>/dev/null | grep -qi "0483:374[48b]" || lsusb 2>/dev/null | grep -qi "0483:3752"
}

if ! detect_stlink; then
    echo "未检测到 ST-Link，尝试通过 usbipd (WSL) 挂载..."
    USBIPD_OUTPUT=$(powershell.exe -Command "usbipd list" 2>&1)
    STLINK_BUSID=$(echo "$USBIPD_OUTPUT" | grep -i "0483\|STM32.*STLink" | grep -oP '^\s*\S+' | head -1)
    if [ -z "$STLINK_BUSID" ]; then
        echo "错误: usbipd 列表中也未找到 ST-Link。"
        echo "请连接 ST-Link 并在 Windows PowerShell 中运行 'usbipd list' 检查。"
        exit 1
    fi
    echo "在总线 $STLINK_BUSID 上找到 ST-Link，正在挂载..."
    powershell.exe -Command "usbipd attach --wsl --busid $STLINK_BUSID" 2>&1
    sleep 3
    if ! detect_stlink; then
        echo "错误: 挂载 ST-Link 失败。请手动挂载:"
        echo "  powershell.exe -Command \"usbipd attach --wsl --busid $STLINK_BUSID\""
        exit 1
    fi
    echo "ST-Link 挂载成功。"
fi

# 构建项目
if [ "${BUILD_TYPE}" = "Debug" ]; then
    echo "正在重新配置 CMake..."
    cmake --preset Debug 2>&1 || true
    echo "正在构建项目..."
    ninja -C "${SCRIPT_DIR}/build/Debug" 2>&1 || true
fi

# ========================
# GDB 调试模式
# ========================
if [ "$GDB_MODE" = true ]; then
    GDB=$(find_gdb)
    if [ -z "$GDB" ]; then
        echo "错误: 未找到 ARM GDB。请安装:"
        echo "  sudo apt install gdb-multiarch"
        echo "  或 sudo apt install arm-none-eabi-gdb"
        exit 1
    fi
    echo "使用 GDB: ${GDB}"

    echo "正在启动 OpenOCD GDB 服务器..."
    openocd -f "interface/${OPENOCD_INTERFACE}.cfg" -f "target/${OPENOCD_TARGET}.cfg" > /tmp/openocd.log 2>&1 &
    OPENOCD_PID=$!

        # 等待 OpenOCD 真正就绪，最多等 10 秒
    for i in $(seq 1 20); do
        if grep -q "Listening on port 3333 for gdb connections" /tmp/openocd.log 2>/dev/null; then
            break
        fi
        sleep 0.5
    done

    if ! kill -0 "$OPENOCD_PID" 2>/dev/null; then
        echo "错误: OpenOCD 启动失败。"
        exit 1
    fi

    echo ""
    echo "========================================"
    echo " GDB 调试会话"
    echo "========================================"
    echo " 目标文件 : ${ELF_FILE}"
    echo " GDB      : ${GDB}"
    echo ""
    echo " 常用 GDB 命令:"
    echo "   target remote :3333   连接到 OpenOCD"
    echo "   load                  加载固件"
    echo "   continue              继续运行"
    echo "   break main            在 main 设置断点"
    echo "   step/next             单步执行"
    echo "   print var             打印变量值"
    echo "   monitor reset halt    复位并暂停"
    echo "========================================"
    echo ""

    ${GDB} -ex "file ${ELF_FILE}" -ex "target remote :3333" -ex "monitor reset halt" -ex "load" -ex "monitor reset halt"

    echo "正在清理 OpenOCD..."
    kill "${OPENOCD_PID}" 2>/dev/null || true
    wait "${OPENOCD_PID}" 2>/dev/null || true
    echo "调试会话已结束。"
else
    # 普通烧录模式
    echo "正在通过 OpenOCD 烧录..."
    openocd -f "interface/${OPENOCD_INTERFACE}.cfg" -f "target/${OPENOCD_TARGET}.cfg" -c "program ${ELF_FILE} verify reset exit"

    echo ""
    echo "烧录成功!"
fi
