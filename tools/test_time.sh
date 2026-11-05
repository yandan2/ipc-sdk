#!/bin/sh
# 用法: ./test_time.sh [NV12|RGB|...] [帧数]
# 以 root 运行整个脚本，用毫秒精度等到整秒 B，拉高 GPIO153（LED 灭）。

[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

FORMAT="${1:-NV12}"
FRAMES="${2:-120}"
TMPFILE=$(mktemp /tmp/ipc_time_XXXXXX.txt)

# 导出 GPIO153 并设置为输出
echo 153 > /sys/class/gpio/export 2>/dev/null || true
echo high > /sys/class/gpio/gpio153/direction

A=$(date +%s)
B=$((A + 2))
echo "格式=$FORMAT 帧数=$FRAMES"
echo "A=$A  B=$B（GPIO153 将在 B 拉高，LED 灭）"
echo "---"

# 后台：毫秒精度等到 B 时刻再拉高 GPIO153
(
    now_ms=$(date +%s%3N)
    target_ms=$((B * 1000))
    delay_ms=$((target_ms - now_ms))
    if [ "$delay_ms" -gt 0 ]; then
        sleep $(printf "%d.%03d" $((delay_ms / 1000)) $((delay_ms % 1000)))
    fi
    echo 1 > /sys/class/gpio/gpio153/value
    echo "GPIO153=1 at $(date +%H:%M:%S.%3N)"
) &

# 在 B 前 1s 启动 read_frames
sleep 1
IPC_FRAMES="$FRAMES" IPC_USER=admin IPC_PASSWORD="Geek_2023" \
    ../read_frames rtsp://10.10.10.98/stream1 "$FORMAT" 2>/dev/null | tee "$TMPFILE"

# 恢复 GPIO
echo 0 > /sys/class/gpio/gpio153/value
echo 153 > /sys/class/gpio/unexport 2>/dev/null || true

echo "---"
# 统计由 read_frames 在末尾直接打印，从 TMPFILE 中提取并显示
grep -E "^decode\+convert:|^frames=" "$TMPFILE"

rm -f "$TMPFILE"
