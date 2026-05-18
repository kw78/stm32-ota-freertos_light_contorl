#!/bin/bash
#
# flash.sh - Automate flashing STM32 firmware via OpenOCD
#
# Automatically attaches ST-Link via usbipd (WSL) if not already connected.
#
# Usage:
#   ./flash.sh              Flash using default Debug build
#   ./flash.sh Release      Flash using Release build
#   ./flash.sh path/to/firmware.elf   Flash a specific ELF file
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Handle --help flag
if [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
    echo "Usage:"
    echo "  ./flash.sh                             Flash using default Debug build"
    echo "  ./flash.sh Release                  Flash using Release build"
    echo "  ./flash.sh path/to/firmware.elf     Flash a specific ELF file"
    exit 0
fi

# Parse --quiet flag
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
    echo "  ./flash.sh                Flash using default Debug build"
    echo "  ./flash.sh Release        Flash using Release build"
    echo "  ./flash.sh path/to/file.elf  Flash a specific ELF file"
    exit 1
fi

echo "========================================"
echo " Flashing STM32 Firmware"
echo "========================================"
echo " Build type : ${BUILD_TYPE}"
echo " ELF file   : ${ELF_FILE}"
echo "========================================"

# ---- Auto-attach ST-Link via usbipd on WSL ----
# Check if ST-Link is visible via lsusb
if ! lsusb 2>/dev/null | grep -qi "0483:3748"; then
    echo "ST-Link not detected. Attempting to attach via usbipd (WSL)..."
    # Detect the BUSID from Windows usbipd
    USBIPD_OUTPUT=$(powershell.exe -Command "usbipd list" 2>&1)
    STLINK_BUSID=$(echo "$USBIPD_OUTPUT" | grep -i "0483\|STM32.*STLink" | grep -oP '^\s*\S+' | head -1)
    if [ -z "$STLINK_BUSID" ]; then
        echo "Error: ST-Link not found in usbipd list either."
        echo "Please connect ST-Link to your computer and ensure it appears in 'usbipd list'."
        exit 1
    fi
    echo "Found ST-Link on bus $STLINK_BUSID. Attaching..."
    powershell.exe -Command "usbipd attach --wsl --busid $STLINK_BUSID" 2>&1
    sleep 3
    # Verify attachment
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
</final_file_content>

IMPORTANT: For any future changes to this file, use the final_file_content shown above as your reference. This content reflects the current state of the file, including any auto-formatting (e.g., if you used single quotes but the formatter converted them to double quotes). Always base your SEARCH/REPLACE operations on this final version to ensure accuracy.



# task_progress RECOMMENDED

When starting a new task, it is recommended to include a todo list using the task_progress parameter.


1. Include a todo list using the task_progress parameter in your next tool call
2. Create a comprehensive checklist of all steps needed
3. Use markdown format: - [ ] for incomplete, - [x] for complete

**Benefits of creating a todo/task_progress list now:**
	- Clear roadmap for implementation
	- Progress tracking throughout the task
	- Nothing gets forgotten or missed
	- Users can see, monitor, and edit the plan

**Example structure:**```
- [ ] Analyze requirements
- [ ] Set up necessary files
- [ ] Implement main functionality
- [ ] Handle edge cases
- [ ] Test the implementation
- [ ] Verify results```

Keeping the task_progress list updated helps track progress and ensures nothing is missed.

<environment_details>
# Visual Studio Code Visible Files
flash.sh

# Visual Studio Code Open Tabs
Core/Src/main.c
flash.sh
99-stlink.rules

# Current Time
5/19/2026, 12:05:08 AM (Asia/Shanghai, UTC+8:00)

# Context Window Usage
41,951 / 128K tokens used (33%)

# Current Mode
ACT MODE
</environment_details>