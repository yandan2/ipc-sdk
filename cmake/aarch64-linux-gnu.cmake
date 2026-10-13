# Linux x86_64 主机交叉编译到 RK3588 ARM64。
# 通过 -DCMAKE_C_COMPILER=/完整路径/工具链-gcc 选择客户自己的编译器。
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(CMAKE_C_COMPILER)
    set(IPC_C_COMPILER "${CMAKE_C_COMPILER}")
elseif(NOT IPC_C_COMPILER)
    find_program(IPC_C_COMPILER NAMES aarch64-linux-gnu-gcc)
    if(NOT IPC_C_COMPILER)
        message(FATAL_ERROR "Specify -DCMAKE_C_COMPILER=/path/to/your/aarch64-gcc")
    endif()
endif()
set(CMAKE_C_COMPILER "${IPC_C_COMPILER}")
# try_compile() 的临时项目也必须使用同一套客户工具链。
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES IPC_C_COMPILER)

# 根据工具链自动查找目标开发环境；也兼容显式指定 CMAKE_SYSROOT。
if(NOT CMAKE_SYSROOT)
    execute_process(
        COMMAND sh "${CMAKE_CURRENT_LIST_DIR}/../tools/find_sysroot.sh" "${CMAKE_C_COMPILER}"
        RESULT_VARIABLE IPC_SYSROOT_RESULT
        OUTPUT_VARIABLE IPC_DETECTED_SYSROOT
        ERROR_VARIABLE IPC_SYSROOT_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT IPC_SYSROOT_RESULT EQUAL 0)
        message(FATAL_ERROR "${IPC_SYSROOT_ERROR}")
    endif()
    set(CMAKE_SYSROOT "${IPC_DETECTED_SYSROOT}" CACHE PATH "Target sysroot detected from compiler")
endif()
if(NOT EXISTS "${CMAKE_SYSROOT}/usr/include/stdio.h")
    message(FATAL_ERROR "Missing ARM64 sysroot: ${CMAKE_SYSROOT}")
endif()

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
