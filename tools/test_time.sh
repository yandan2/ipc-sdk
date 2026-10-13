#!/bin/sh
# 用法: ./test_time.sh [NV12|RGB|...] [帧数]
# 采集指定格式的帧，统计解码+格式转换耗时（跳过前两帧热身帧）。

FORMAT="${1:-NV12}"
FRAMES="${2:-120}"
TMPFILE=$(mktemp /tmp/ipc_time_XXXXXX.txt)
MAX_RETRY=5

echo "格式=$FORMAT 帧数=$FRAMES"
echo "---"

i=0
while [ $i -lt $MAX_RETRY ]; do
    i=$((i + 1))
    IPC_FRAMES="$FRAMES" sudo IPC_USER=admin IPC_PASSWORD="Twb2674535338..." \
        IPC_TRANSPORT=tcp \
        ./read_frames rtsp://10.10.10.98/stream1 "$FORMAT" 2>/dev/null | tee "$TMPFILE"
    grep -q "frames=0" "$TMPFILE" || break
    echo "连接失败，第 $i 次重试..."
done

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
