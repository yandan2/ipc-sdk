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
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

static volatile sig_atomic_t g_interrupted = 0;

/* ---------- 保存线程 ---------- */

#define SAVE_DEPTH 8 /* 环形队列深度；队满时丢帧并打印警告。 */

typedef struct
{
    void  *pixels; /* 连续像素缓冲区，由生产者 malloc，消费者 free。 */
    size_t size;
    char   path[256];
} SaveItem;

typedef struct
{
    SaveItem        items[SAVE_DEPTH];
    int             head, tail, count;
    volatile int    done; /* 置 1 后消费者排空队列再退出。 */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
} SaveQueue;

static void *save_thread_fn(void *arg)
{
    SaveQueue       *q = arg;
    SaveItem         item;
    FILE            *f;
    struct timespec  t0, t1;
    long             save_us;

    for (;;)
    {
        pthread_mutex_lock(&q->lock);
        while (q->count == 0 && !q->done)
        {
            pthread_cond_wait(&q->not_empty, &q->lock);
        }
        if (q->count == 0)
        {
            pthread_mutex_unlock(&q->lock);
            break;
        }
        item    = q->items[q->head];
        q->head = (q->head + 1) % SAVE_DEPTH;
        q->count--;
        pthread_mutex_unlock(&q->lock);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        f = fopen(item.path, "wb");
        if (f)
        {
            fwrite(item.pixels, 1, item.size, f);
            fclose(f);
        }
        else
        {
            fprintf(stderr, "save: cannot open %s\n", item.path);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        save_us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L;
        printf("saved %s  %.2f ms\n", item.path, save_us / 1000.0);
        free(item.pixels);
    }
    return NULL;
}

/* 生产者：复制帧像素并入队；队满时丢帧。 */
static void enqueue_save(SaveQueue *q, const IPC_FRAME *frame,
                         const char *dir, unsigned frame_num)
{
    struct timespec wall;
    struct tm       tm_buf;
    char            ts[32];
    SaveItem       *slot;
    size_t          sz;
    void           *buf;

    clock_gettime(CLOCK_REALTIME, &wall);
    localtime_r(&wall.tv_sec, &tm_buf);
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_buf);

    sz  = ipc_frame_size(frame);
    buf = malloc(sz);
    if (!buf)
    {
        return;
    }
    if (ipc_frame_copy(frame, buf, sz) != IPC_OK)
    {
        free(buf);
        return;
    }

    pthread_mutex_lock(&q->lock);
    if (q->count == SAVE_DEPTH)
    {
        pthread_mutex_unlock(&q->lock);
        free(buf);
        fprintf(stderr, "save queue full, frame %u dropped\n", frame_num);
        return;
    }
    slot         = &q->items[q->tail];
    slot->pixels = buf;
    slot->size   = sz;
    snprintf(slot->path, sizeof(slot->path), "%s/frame_%s_%03ld_%04u.raw",
             dir, ts, wall.tv_nsec / 1000000L, frame_num);
    q->tail = (q->tail + 1) % SAVE_DEPTH;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

/* ---------- 工具函数 ---------- */

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

/* ---------- 主程序 ---------- */

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
    const char *savedir = NULL;
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
    SaveQueue sq;
    pthread_t save_tid;

    if (argc < 3 || argc > 4)
    {
        fprintf(stderr,
                "Usage: %s IP|rtsp-url NV12|NV21|I420|RGB|BGR|RGBA|BGRA|GRAY8 [first-frame.raw]\n"
                "IPC_USER / IPC_PASSWORD: credentials; IPC_PATH: vendor RTSP path.\n"
                "IPC_FRAMES (120), IPC_TRANSPORT (auto|tcp|udp),\n"
                "IPC_LATENCY_MS (50), IPC_TIMEOUT_MS (10000), IPC_PORT (554),\n"
                "IPC_CONVERSION (auto|mpp|cpu).\n"
                "IPC_SAVEDIR: 保存目录，默认 frames；设为空串禁用保存。Ctrl-C stops capture.\n",
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

    /* 初始化保存队列并启动保存线程；IPC_SAVEDIR="" 禁用保存。 */
    savedir = getenv("IPC_SAVEDIR");
    if (!savedir)
    {
        savedir = "/home/geekplus/temp/ipc/picture";
    }
    memset(&sq, 0, sizeof(sq));
    pthread_mutex_init(&sq.lock, NULL);
    pthread_cond_init(&sq.not_empty, NULL);
    if (*savedir)
    {
        mkdir(savedir, 0755);
        pthread_create(&save_tid, NULL, save_thread_fn, &sq);
        printf("保存目录: %s\n", savedir);
    }

    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    handle = ipc_open(&config, error, sizeof(error));
    if (!handle)
    {
        fprintf(stderr, "%s\n", error);
        if (*savedir)
        {
            pthread_mutex_lock(&sq.lock);
            sq.done = 1;
            pthread_cond_signal(&sq.not_empty);
            pthread_mutex_unlock(&sq.lock);
            pthread_join(save_tid, NULL);
        }
        pthread_cond_destroy(&sq.not_empty);
        pthread_mutex_destroy(&sq.lock);
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

        /* 将本帧异步提交给保存线程。 */
        if (*savedir)
        {
            enqueue_save(&sq, &frame, savedir, count);
        }

        ipc_frame_release(&frame);
        if (result)
        {
            break;
        }
    }
    ipc_close(handle);

    /* 通知保存线程排空队列后退出，再回收资源。 */
    if (*savedir)
    {
        pthread_mutex_lock(&sq.lock);
        sq.done = 1;
        pthread_cond_signal(&sq.not_empty);
        pthread_mutex_unlock(&sq.lock);
        pthread_join(save_tid, NULL);
    }
    pthread_cond_destroy(&sq.not_empty);
    pthread_mutex_destroy(&sq.lock);

    printf("frames=%u status=%s\n", count,
           result          ? "FAILED"
           : g_interrupted ? "INTERRUPTED"
                           : "OK");
    return g_interrupted ? 130 : result;
}
