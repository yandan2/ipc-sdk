#!/bin/sh
# 客户构建入口：编译器和 sysroot 通过标准 CMake 参数传入，不下载或安装依赖。
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_mode=${1:-cross}
if [ "$#" -gt 0 ]; then
    shift
fi
case "$task_mode" in
    cross)
        task_build="$task_root/build-aarch64"
        cmake -S "$task_root" -B "$task_build" \
            -DCMAKE_TOOLCHAIN_FILE="$task_root/cmake/aarch64-linux-gnu.cmake" \
            -DCMAKE_BUILD_TYPE=Release "$@"
        ;;
    native)
        task_build="$task_root/build-native"
        cmake -S "$task_root" -B "$task_build" -DCMAKE_BUILD_TYPE=Release "$@"
        ;;
    *)
        printf '%s\n' '用法：sh tools/build.sh cross|native [CMake 参数]' >&2
        exit 2
        ;;
esac
cmake --build "$task_build" --parallel "${JOBS:-4}"
