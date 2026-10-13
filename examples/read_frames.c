/**
 * @file read_frames.c
 * @brief 客户集成示例：设置连接参数、同步取帧，并可将首帧保存为原始像素。
 * @version 1.0.0
 * @date 2026-10-08
 *
 * 依赖公共头文件 ipc_sdk.h 和动态库 libipc_sdk，不直接访问 GStreamer。
 * 账号密码通过环境变量传入；日志仅显示格式、布局和脱敏后的错误信息。
 * 原始文件不含宽高等头信息，需结合输出日志中的格式和尺寸解释。
 */
#define _POSIX_C_SOURCE 200809L
#include "ipc_sdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>

static volatile sig_atomic_t g_interrupted = 0;

/* 信号处理器只置位退出标志，避免在异步信号上下文调用非安全库函数。 */
static void stop(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

/* 读取并校验无符号环境变量：缺省使用 fallback，非法值返回 0。
 * 同时检查空串、负号、溢出、尾部字符和业务上限。 */
static int env_uint(const char *name, unsigned fallback, unsigned max, unsigned *out)
{
    const char *text = NULL;
    char *end = NULL;
    unsigned long value = 0;

    text = getenv(name);
    if (!text)
    {
        *out = fallback;
        return 1;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (!*text || *text == '-' || errno || *end || value > max)
    {
        fprintf(stderr, "Invalid %s\n", name);
        return 0;
    }
    *out = (unsigned)value;
    return 1;
}

/* 先复制为无行填充的连续像素，再写入文件；失败时归还内存并关闭文件。 */
static int save_raw(const char *path, const IPC_FRAME *frame)
{
    size_t bytes = 0;
    void *pixels = NULL;
    FILE *out = NULL;
    int ok = 0;

    bytes = ipc_frame_size(frame);
    pixels = malloc(bytes);
    if (!pixels || ipc_frame_copy(frame, pixels, bytes) != IPC_OK)
    {
        free(pixels);
        return 0;
    }
    out = fopen(path, "wb");
    if (out)
    {
        ok = fwrite(pixels, 1, bytes, out) == bytes;
        if (fclose(out))
        {
            ok = 0;
        }
    }
    free(pixels);
    return ok;
}

/* 按需覆盖传输策略；未配置时保留 SDK 默认 AUTO，兼容旧 IPC_UDP 参数。 */
static int configure_transport(IPC_CONFIG *config)
{
    const char *name = getenv("IPC_TRANSPORT");
    const char *legacy = getenv("IPC_UDP");
    IPC_TRANSPORT transport = IPC_TRANSPORT_AUTO;

    if (!name && !legacy)
    {
        return 1;
    }
    if (name)
    {
        if (!strcmp(name, "tcp"))
        {
            transport = IPC_TRANSPORT_TCP;
        }
        else if (!strcmp(name, "udp"))
        {
            transport = IPC_TRANSPORT_UDP;
        }
        else if (strcmp(name, "auto"))
        {
            return 0;
        }
    }
    else if (legacy)
    {
        if (!strcmp(legacy, "0"))
        {
            transport = IPC_TRANSPORT_TCP;
        }
        else if (!strcmp(legacy, "1"))
        {
            transport = IPC_TRANSPORT_UDP;
        }
        else
        {
            return 0;
        }
    }
    /* 只有请求的模式与当前配置不同时，才调用修改接口。 */
    if ((int)transport == config->udp)
    {
        return 1;
    }
    return ipc_config_set_transport(config, transport) == IPC_OK;
}

/* 解析配置后按 open/read/release/close 顺序操作；错误和信号退出均关闭句柄。 */
int main(int argc, char **argv)
{
    unsigned retry = 0;
    unsigned plane = 0;
    IPC_CONFIG config =
    {
        0,
    };
    unsigned frames = 0;
    const char *conversion = NULL;
    char error[192] =
    {
        0,
    };
    IPC_HANDLE *handle = NULL;
    int result = 0;
    unsigned count = 0;
    IPC_FRAME frame =
    {
        0,
    };
    IPC_STATUS status = IPC_TIMEOUT;

    if (argc < 3 || argc > 4)
    {
        fprintf(stderr,
                "Usage: %s IP|rtsp-url NV12|NV21|I420|RGB|BGR|RGBA|BGRA|GRAY8 [first-frame.raw]\n"
                "IPC_USER / IPC_PASSWORD: credentials; IPC_PATH: vendor RTSP path.\n"
                "IPC_FRAMES (120), IPC_TRANSPORT (auto|tcp|udp),\n"
                "IPC_LATENCY_MS (50), IPC_TIMEOUT_MS (10000), IPC_PORT (554),\n"
                "IPC_CONVERSION (auto|mpp|cpu). Ctrl-C stops capture.\n",
                argv[0]);
        return 2;
    }
    ipc_config_init(&config);
    if (ipc_format_parse(argv[2], &config.format) != IPC_OK)
    {
        return 2;
    }
    if (strstr(argv[1], "://"))
    {
        config.url = argv[1];
    }
    else
    {
        config.ip = argv[1];
    }
    config.path = getenv("IPC_PATH");
    config.username = getenv("IPC_USER");
    config.password = getenv("IPC_PASSWORD");
    if (!env_uint("IPC_FRAMES", 120, UINT_MAX, &frames) || !frames
        || !env_uint("IPC_LATENCY_MS", 50, 60000, &config.latency_ms)
        || !env_uint("IPC_TIMEOUT_MS", 10000, 3600000, &config.network_timeout_ms)
        || !env_uint("IPC_PORT", 554, 65535, &config.port))
    {
        return 2;
    }
    if (!configure_transport(&config))
    {
        fprintf(stderr, "Invalid transport\n");
        return 2;
    }
    conversion = getenv("IPC_CONVERSION");
    if (conversion)
    {
        if (!strcmp(conversion, "cpu"))
        {
            config.conversion = IPC_CONVERT_CPU;
        }
        else if (!strcmp(conversion, "mpp"))
        {
            config.conversion = IPC_CONVERT_MPP_ONLY;
        }
        else if (strcmp(conversion, "auto"))
        {
            return 2;
        }
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    handle = ipc_open(&config, error, sizeof(error));
    if (!handle)
    {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    while (count < frames && !g_interrupted)
    {
        memset(&frame, 0, sizeof(frame));

        /* 每次最多等待 200 ms，以便响应退出信号；连续 15 秒无帧时终止。 */
        status = IPC_TIMEOUT;
        for (retry = 0; retry < 75 && !g_interrupted && status == IPC_TIMEOUT; ++retry)
        {
            status = ipc_read(handle, 200, &frame);
        }
        if (g_interrupted)
        {
            ipc_frame_release(&frame);
            break;
        }
        if (status != IPC_OK)
        {
            fprintf(stderr, "read status=%d error=%d %s\n", status, ipc_last_error_code(handle),
                    ipc_last_error(handle));
            result = 1;
            break;
        }
        if (!count)
        {
            printf("codec=%s decoder=mppvideodec conversion=%s format=%s %ux%u planes=%u "
                   "packed_bytes=%zu\n",
                   frame.codec == IPC_CODEC_H265 ? "H265" : "H264",
                   frame.conversion == IPC_CONVERT_CPU ? "CPU" : "MPP",
                   ipc_format_name(frame.format), frame.width, frame.height, frame.planes,
                   ipc_frame_size(&frame));
            for (plane = 0; plane < frame.planes; ++plane)
            {
                printf("plane=%u stride=%d row_bytes=%u rows=%u\n", plane, frame.stride[plane],
                       frame.row_bytes[plane], frame.rows[plane]);
            }
            if (argc == 4 && !save_raw(argv[3], &frame))
            {
                fprintf(stderr, "Cannot save raw frame\n");
                result = 1;
            }
            fflush(stdout);
        }
        ++count;
        ipc_frame_release(&frame);
        if (result)
        {
            break;
        }
    }
    ipc_close(handle);
    printf("frames=%u status=%s\n", count,
           result          ? "FAILED"
           : g_interrupted ? "INTERRUPTED"
                           : "OK");
    return g_interrupted ? 130 : result;
}
