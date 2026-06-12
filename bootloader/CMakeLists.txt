cmake_minimum_required(VERSION 3.22)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_PROJECT_NAME bootloader)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
project(${CMAKE_PROJECT_NAME})
set(CMAKE_EXE_LINKER_FLAGS "")
enable_language(C ASM)
add_executable(${CMAKE_PROJECT_NAME})

#工程路径
set(PARENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

#编译选项
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    -mcpu=cortex-m3 -mthumb -mfloat-abi=soft
    -Wall -Wextra -fdata-sections -ffunction-sections -Os -g3
)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -mcpu=cortex-m3 -mthumb -mfloat-abi=soft
    -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    --specs=nano.specs -Wl,--gc-sections -Wl,--print-memory-usage
)

#宏定义
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    USE_HAL_DRIVER
    STM32F103xB
)

#Include 路径
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Src
    ${PARENT_DIR}/Core/Inc
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Inc
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Inc/Legacy
    ${PARENT_DIR}/Drivers/CMSIS/Device/ST/STM32F1xx/Include
    ${PARENT_DIR}/Drivers/CMSIS/Include
)

#源文件
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Bootloader 自己的
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/main.c
    # 共享的
    ${PARENT_DIR}/Core/Src/w25d64.c
    ${PARENT_DIR}/Core/Src/ota.c
    ${PARENT_DIR}/Core/Src/system_stm32f1xx.c
    ${PARENT_DIR}/Core/Src/stm32f1xx_hal_msp.c
    ${PARENT_DIR}/startup_stm32f103xb.s
    # HAL 库
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal.c
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_cortex.c
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc.c
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc_ex.c
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio.c
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio_ex.c
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_spi.c
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash.c
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash_ex.c
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_dma.c
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_pwr.c
    ${PARENT_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_exti.c
)

list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)