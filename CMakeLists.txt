cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME gcctest)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/oled.c              
    Core/Src/MPU6050.c
    Core/Src/w25d64.c
    Core/Src/ota.c
    Core/Src/supervisor.c
    Core/Src/blackbox.c
    Core/Src/light_ctrl.c
)

# 固件版本注入：git short hash（构建时生成，QUERY / 串口 banner 可读）
# 找不到 git（如无 .git 目录的拷贝构建）时退化为 0xdeadbeef
execute_process(
    COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short=8 HEAD
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RESULT
)
if(NOT GIT_RESULT EQUAL 0 OR GIT_HASH STREQUAL "")
    set(GIT_HASH "deadbeef")
endif()
# 验收测试镜像可用环境变量覆盖版本号（如 APP_VERSION_OVERRIDE=badf0001），
# 便于 QUERY/黑匣子里一眼区分坏固件
if(DEFINED ENV{APP_VERSION_OVERRIDE})
    set(GIT_HASH "$ENV{APP_VERSION_OVERRIDE}")
endif()
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    APP_VERSION=0x${GIT_HASH}
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
