#!/bin/sh
# 从客户工具链查询目标开发环境，不读取主机的 GStreamer 开发文件。
set -eu
task_cc=${1:?请指定 ARM64 Linux GCC 路径}
task_pkg_config=${PKG_CONFIG:-pkg-config}
task_cc_path=$(command -v "$task_cc") ||
{
    printf '找不到交叉编译器：%s\n' "$task_cc" >&2
    exit 1
}
command -v "$task_pkg_config" >/dev/null 2>&1 ||
{
    printf '%s\n' '编译主机需要安装 pkg-config。' >&2
    exit 1
}
task_bin=$(CDPATH= cd -- "$(dirname -- "$task_cc_path")" && pwd)
task_prefix=$(dirname -- "$task_bin")
task_target=$("$task_cc_path" -dumpmachine)
task_reported=$("$task_cc_path" -print-sysroot)

# 优先使用编译器报告的路径，再检查工具链常见的目标库目录。
for task_candidate in "$task_reported" \
    "$task_prefix/sysroot" "$task_prefix/$task_target/sysroot" \
    "$task_prefix/$task_target/libc" "$task_prefix/$task_target"
do
    [ -n "$task_candidate" ] && [ -d "$task_candidate" ] || continue
    task_candidate=$(CDPATH= cd -- "$task_candidate" && pwd)
    # / 是电脑系统根目录，不能将其中的 x86_64 库用于 ARM64 交叉编译。
    [ "$task_candidate" != / ] || continue
    [ -f "$task_candidate/usr/include/stdio.h" ] || continue
    if env PKG_CONFIG_PATH= PKG_CONFIG_FDO_SYSROOT_RULES=1 \
        PKG_CONFIG_SYSROOT_DIR="$task_candidate" \
        PKG_CONFIG_LIBDIR="$task_candidate/usr/lib/aarch64-linux-gnu/pkgconfig:$task_candidate/usr/lib/pkgconfig:$task_candidate/usr/lib64/pkgconfig:$task_candidate/usr/share/pkgconfig" \
        "$task_pkg_config" --exists 'gstreamer-1.0 >= 1.18' gstreamer-app-1.0 \
        gstreamer-video-1.0 gstreamer-rtsp-1.0 gstreamer-sdp-1.0 gio-2.0
    then
        printf '%s\n' "$task_candidate"
        exit 0
    fi
done

printf '%s\n' '工具链内未找到包含 ARM64 GStreamer/GLib 开发文件的 sysroot。' >&2
printf '%s\n' '请使用带目标开发文件的板卡配套 SDK；设备上的运行库不会自动成为主机的编译依赖。' >&2
exit 1
