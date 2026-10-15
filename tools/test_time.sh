#!/bin/sh
# 用法: ./test_time.sh [NV12|RGB|...] [帧数]
# t=0   写入 PPS（B = 当前秒 + 2，脉冲在 2 s 后触发）
# t=1.5 启动 read_frames，使其能拍到 t=2 s 的 PPS 脉冲

FORMAT="${1:-NV12}"
FRAMES="${2:-120}"
TMPFILE=$(mktemp /tmp/ipc_time_XXXXXX.txt)

A=$(date +%s)
B=$((A + 2))

# t=0：立即配置 PPS，脉冲将在 B 时刻触发
echo "$B" | sudo tee /sys/devices/platform/pps/time > /dev/null
echo "格式=$FORMAT 帧数=$FRAMES"
echo "A=$A  B=$B（PPS 脉冲将在 $B 触发）"
echo "---"

# t=1：在脉冲前 1 s 启动 read_frames
sleep 1
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
