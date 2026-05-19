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
#   ./flash.sh --init-vscode                生成 VS Code 调试配置
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
    # 从 CMakeLists.txt 读取项目名
    if [ -f "${SCRIPT_DIR}/CMakeLists.txt" ]; then
        PROJECT_NAME=$(grep -oP 'set\(CMAKE_PROJECT_NAME\s+\K\w+' "${SCRIPT_DIR}/CMakeLists.txt" || true)
    fi
    if [ -z "$PROJECT_NAME" ]; then
        PROJECT_NAME=$(basename "$SCRIPT_DIR")
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
# 生成 VS Code 调试配置
# ========================
init_vscode_config() {
    read_project_config

    echo "========================================"
    echo " 初始化 VS Code 调试配置"
    echo "========================================"
    echo " 项目名: ${PROJECT_NAME}"
    echo "========================================"

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
            "device": "STM32F103xB",
            "interface": "swd",
            "runToMain": true,
            "preLaunchTask": "build-debug",
            "configFiles": [
                "interface/stlink.cfg",
                "target/stm32f1x.cfg"
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

    cat > "$TASKS_FILE" << TASKSJSON
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
                "\$gcc"
            ]
        }
    ]
}
TASKSJSON

    echo ""
    echo "========================================"
    echo " VS Code 调试配置已生成！"
    echo "========================================"
    echo ""
    echo " 已创建以下文件:"
    echo "   ${LAUNCH_FILE}"
    echo "   ${TASKS_FILE}"
    echo "   ${EXT_FILE} (已更新)"
    echo ""
    echo " 接下来:"
    echo "   1. 安装 Cortex-Debug 扩展 (marus25.cortex-debug)"
    echo "   2. 按 Ctrl+Shift+P → Reload Window"
    echo "   3. 按 F5 开始调试!"
    echo ""
    echo "   注意: 调试前请确保 ST-Link 已连接。"
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
    --init-vscode|-i)
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
        echo "  ./flash.sh --init-vscode                生成 VS Code 调试配置"
        echo "  ./flash.sh --install-udev               安装 ST-Link udev 规则"
        echo ""
        echo "GDB 命令行调试:"
        echo "  1. 脚本会自动启动 OpenOCD GDB 服务器"
        echo "  2. 支持 arm-none-eabi-gdb 和 gdb-multiarch"
        echo "  3. 连接到 localhost:3333"
        echo ""
        echo "VS Code 图形化调试:"
        echo "  1. 运行 ./flash.sh --init-vscode  生成调试配置"
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
    echo "  ./flash.sh --init-vscode                生成 VS Code 调试配置"
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
if ! lsusb 2>/dev/null | grep -qi "0483:3748"; then
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
    if ! lsusb 2>/dev/null | grep -qi "0483:3748"; then
        echo "错误: 挂载 ST-Link 失败。请手动挂载:"
        echo "  powershell.exe -Command \"usbipd attach --wsl --busid $STLINK_BUSID\""
        exit 1
    fi
    echo "ST-Link 挂载成功。"
fi

# 构建项目
if [ "${BUILD_TYPE}" = "Debug" ]; then
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
    openocd -f interface/stlink.cfg -f target/stm32f1x.cfg &
    OPENOCD_PID=$!
    sleep 2

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
    openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "program ${ELF_FILE} verify reset exit"

    echo ""
    echo "烧录成功!"
fi
