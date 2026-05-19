#!/bin/bash
#
# flash.sh - Automate flashing STM32 firmware via OpenOCD
#
# All-in-one script: self-contained udev rules, auto usbipd attach (WSL),
# build, and flash.
#
# Usage:
#   ./flash.sh                              Flash Debug build (default)
#   ./flash.sh Release                      Flash Release build
#   ./flash.sh path/to/firmware.elf         Flash specific ELF
#   ./flash.sh --install-udev               Install ST-Link udev rules (Linux)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ========================
# Embedded udev rules content
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
# Install udev rules
# ========================
install_udev_rules() {
    local rules_file="/etc/udev/rules.d/99-stlink.rules"
    echo "Installing ST-Link udev rules to ${rules_file}..."
    if [ "$(id -u)" -ne 0 ]; then
        echo "sudo required. Re-running with sudo..."
        exec sudo "$0" --install-udev
    fi
    echo "$STLINK_UDEV_RULES" > "$rules_file"
    udevadm control --reload-rules
    udevadm trigger
    echo "ST-Link udev rules installed successfully."
    exit 0
}

# ========================
# Parse arguments
# ========================
if [ "$1" = "--install-u.dev" ]; then
    install_udev_rules
fi

if [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
    echo "Usage:"
    echo "  ./flash.sh                              Flash Debug build (default)"
    echo "  ./flash.sh Release                      Flash Release build"
    echo "  ./flash.s
    h path/to/firmware.elf         Flash specific ELF"
    echo "  ./flash.sh --install-udev               Install ST-Link udev rules (Linux)"
    echo ""
    echo "WSL: The script auto-attaches ST-Link via usbipd if not detected."
    echo "  usbipd-win is required on Windows (https://github.com/dorssel/usbipd-win)"
    exit 0
fi

QUIET=false
ARGS=()
for arg in "$@"; do
    if [ "$arg" = "--quiet" ] || [ "$arg" = "-q" ]; then
        QUIET=true
    else
        ARGS+=("$arg")
    fi
done
set -- "${ARGS[@]}"

BUILD_TYPE="${1:-Debug}"

# Determine the ELF file path
if [ -z "$1" ]; then
    ELF_FILE="${SCRIPT_DIR}/build/Debug/${PROJECT_NAME:-gcctest}.elf"
elif [[ "$1" == "Release" || "$1" == "Debug" ]]; then
    BUILD_TYPE="$1"
    ELF_FILE="${SCRIPT_DIR}/build/${BUILD_TYPE}/${PROJECT_NAME:-gcctest}.elf"
else
    ELF_FILE="$1"
fi

# Override PROJECT_NAME if CMakeLists.txt exists
if [ -f "${SCRIPT_DIR}/CMakeLists.txt" ]; then
    PROJECT_NAME=$(grep -oP 'set\(CMAKE_PROJECT_NAME\s+\K\w+' "${SCRIPT_DIR}/CMakeLists.txt" || true)
    if [ -n "$PROJECT_NAME" ]; then
        if [ -z "$1" ] || [ "$1" = "Debug" ] || [ "$1" = "Release" ]; then
            ELF_FILE="${SCRIPT_DIR}/build/${BUILD_TYPE}/${PROJECT_NAME}.elf"
        fi
    fi
fi

# Check if the ELF file exists
if [ ! -f "${ELF_FILE}" ]; then
    echo "Error: ELF file not found: ${ELF_FILE}"
    echo ""
    echo "Usage:"
    echo "  ./flash.sh                              Flash Debug build (default)"
    echo "  ./flash.sh Release                      Flash Release build"
    echo "  ./flash.sh path/to/firmware.elf         Flash specific ELF"
    echo "  ./flash.sh --install-udev               Install ST-Link udev rules (Linux)"
    exit 1
fi

echo "========================================"
echo " Flashing STM32 Firmware"
echo "========================================"
echo " Build type : ${BUILD_TYPE}"
echo " ELF file   : ${ELF_FILE}"
echo "========================================"

# ========================
# Auto-attach ST-Link via usbipd (WSL)
# ========================
if ! lsusb 2>/dev/null | grep -qi "0483:3748"; then
    echo "ST-Link not detected. Attempting to attach via usbipd (WSL)..."
    USBIPD_OUTPUT=$(powershell.exe -Command "usbipd list" 2>&1)
    STLINK_BUSID=$(echo "$USBIPD_OUTPUT" | grep -i "0483\|STM32.*STLink" | grep -oP '^\s*\S+' | head -1)
    if [ -z "$STLINK_BUSID" ]; then
        echo "Error: ST-Link not found in usbipd list either."
        echo "Please connect ST-Link to your computer and ensure it appears in 'usbipd list'."
        echo "(Run 'usbipd list' in Windows PowerShell)"
        exit 1
    fi
    echo "Found ST-Link on bus $STLINK_BUSID. Attaching..."
    powershell.exe -Command "usbipd attach --wsl --busid $STLINK_BUSID" 2>&1
    sleep 3
    if ! lsusb 2>/dev/null | grep -qi "0483:3748"; then
        echo "Error: Failed to attach ST-Link. Try attaching manually:"
        echo "  powershell.exe -Command \"usbipd attach --wsl --busid $STLINK_BUSID\""
        exit 1
    fi
    echo "ST-Link attached successfully."
fi

# Build the project first
if [ "${BUILD_TYPE}" = "Debug" ]; then
    ninja -C "${SCRIPT_DIR}/build/Debug" 2>&1 || true
fi

# Flash using OpenOCD
openocd \
    -f interface/stlink.cfg \
    -f target/stm32f1x.cfg \
    -c "program ${ELF_FILE} verify reset exit"

echo ""
echo "Flash completed successfully!"