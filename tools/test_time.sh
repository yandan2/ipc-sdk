#!/bin/sh
# 用法: ./test_time.sh [NV12|RGB|...] [帧数]
# 导出 GPIO153 为输出，在 B 时刻拉高（LED 灭）让相机捕捉延时。

FORMAT="${1:-NV12}"
FRAMES="${2:-120}"
TMPFILE=$(mktemp /tmp/ipc_time_XXXXXX.txt)

# 导出 GPIO153 并设置为输出
echo 153 | sudo tee /sys/class/gpio/export > /dev/null 2>&1
echo out | sudo tee /sys/class/gpio/gpio153/direction > /dev/null

A=$(date +%s)
B=$((A + 2))
echo "格式=$FORMAT 帧数=$FRAMES"
echo "A=$A  B=$B（GPIO153 将在 B 拉高，LED 灭）"
echo "---"

# 后台：等到 B 时刻拉高 GPIO153
(
    sleep $((B - $(date +%s)))
    echo 1 | sudo tee /sys/class/gpio/gpio153/value > /dev/null
    echo "GPIO153=1 at $(date +%H:%M:%S.%3N)"
) &


IPC_FRAMES="$FRAMES" sudo IPC_USER=admin IPC_PASSWORD="Geek_2023" \
    ../read_frames rtsp://10.10.10.98/stream1 "$FORMAT" 2>/dev/null | tee "$TMPFILE"


echo "---"
awk '
/\[ipc\] decode\+convert=/ {
    split($0, a, "=")
    split(a[2], b, " ")
    val = b[1] + 0
    total++
    if (total <= 2) next
    n++
    sum += val
    if (n == 1 || val < min) min = val
    if (val > max) max = val
}
END {
    if (n > 0)
        printf "统计帧数=%d  min=%.2f ms  max=%.2f ms  avg=%.2f ms\n", n, min, max, sum/n
    else
        print "无耗时数据（帧数不足或格式不支持）"
}
' "$TMPFILE"

rm -f "$TMPFILE"
