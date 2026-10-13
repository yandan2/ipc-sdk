/**
 * @file read_thread.c
 * @brief pthread 持续取流示例：专用线程阻塞读帧，主线程发出停止请求并等待退出。
 *
 * 用法：ipc-sdk-thread-example IP|rtsp-url FORMAT [运行秒数，0 表示持续运行]
 * 认证从 IPC_USER / IPC_PASSWORD 环境变量读取，流路径由 IPC_PATH 指定。
 * 默认持续读取，Ctrl-C 退出；同一句柄的打开、读取和关闭均由取流线程负责。
 * 慢速算法应复制像素到业务队列，再及时释放 SDK 帧，避免占用解码缓冲池。
 */
#define _POSIX_C_SOURCE 200809L
#include "ipc_sdk.h"
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* 控制线程只修改 stop；done 发布后或 pthread_join() 返回后才读取 result。 */
typedef struct IPC_CAPTURE_CONTEXT
{
    IPC_CONFIG config; /* 创建线程前设置，在线程退出前保持有效。 */
    atomic_int stop;   /* 主线程请求停止，由读取线程在两次读取之间检查。 */
    atomic_int done;   /* 读取线程退出前发布完成状态。 */
    int result;        /* 0 为主动停止，1 为连接或读取失败。 */
} IPC_CAPTURE_CONTEXT;

static volatile sig_atomic_t g_interrupted = 0;

/* 信号处理器只设置标志；句柄关闭和线程回收在普通线程上下文完成。 */
static void stop_signal(int signal_number)
{
    (void)signal_number;
    g_interrupted = 1;
}

/* 单调时钟用于示例的运行期限；不用于衡量视频曝光时间。 */
static double monotonic_seconds(void)
{
    struct timespec now =
    {
        0,
    };

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

/*
 * pthread 入口：该线程独占句柄，完成 open/read/release/close 整个生命周期。
 * 正常运行中无帧时等待最多 200 ms；有帧立即返回，不会固定睡满 200 ms。
 * TIMEOUT 允许继续等；ERROR/EOS 必须退出或进入有退避的重连流程，不能空转。
 */
static void *capture_thread(void *argument)
{
    IPC_CAPTURE_CONTEXT *context = NULL;
    IPC_HANDLE *handle = NULL;
    IPC_FRAME frame =
    {
        0,
    };
    IPC_STATUS status = IPC_TIMEOUT;
    char error[192] =
    {
        0,
    };
    uint64_t frames = 0;
    uint8_t *pixels = NULL;
    uint8_t *resized = NULL;
    size_t image_bytes = 0;
    size_t image_capacity = 0;
    size_t written = 0;
    FILE *output = NULL;
    int close_result = 0;

    context = argument;
    handle = ipc_open(&context->config, error, sizeof(error));
    if (!handle)
    {
        fprintf(stderr, "open failed: %s\n", error);
        context->result = 1;
        atomic_store(&context->done, 1);
        return NULL;
    }

    while (!atomic_load(&context->stop))
    {
        status = ipc_read(handle, 200, &frame);
        if (status == IPC_TIMEOUT)
        {
            continue;
        }
        if (status != IPC_OK)
        {
            fprintf(stderr, "read status=%d error=%d %s\n", status, ipc_last_error_code(handle),
                    ipc_last_error(handle));
            context->result = 1;
            break;
        }

        if (frames == 0)
        {
            printf("codec=%s format=%s %ux%u\n", frame.codec == IPC_CODEC_H265 ? "H265" : "H264",
                   ipc_format_name(frame.format), frame.width, frame.height);
            fflush(stdout);
        }

        /* 将 SDK 帧复制到业务缓冲区，缓冲区可复用，尺寸变化时再扩容。 */
        image_bytes = ipc_frame_size(&frame);
        if (image_bytes == 0)
        {
            ipc_frame_release(&frame);
            context->result = 1;
            break;
        }
        if (image_bytes > image_capacity)
        {
            resized = realloc(pixels, image_bytes);
            if (!resized)
            {
                ipc_frame_release(&frame);
                context->result = 1;
                break;
            }
            pixels = resized;
            image_capacity = image_bytes;
        }
        if (ipc_frame_copy(&frame, pixels, image_capacity) != IPC_OK)
        {
            ipc_frame_release(&frame);
            context->result = 1;
            break;
        }

        /* pixels 已独立持有连续图像；先归还解码帧，再执行文件等业务操作。 */
        ipc_frame_release(&frame);
        if (frames == 0)
        {
            output = fopen("first-frame.raw", "wb");
            if (!output)
            {
                context->result = 1;
                break;
            }
            written = fwrite(pixels, 1, image_bytes, output);
            close_result = fclose(output);
            output = NULL;
            if (written != image_bytes || close_result != 0)
            {
                context->result = 1;
                break;
            }
            printf("首帧已保存：first-frame.raw，%zu 字节\n", image_bytes);
        }
        ++frames;
    }

    free(pixels);
    pixels = NULL;
    ipc_close(handle);
    printf("frames=%llu status=%s\n", (unsigned long long)frames,
           context->result ? "FAILED" : "STOPPED");
    atomic_store(&context->done, 1);
    return NULL;
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

/* 主线程创建工作线程并管理退出，不会在 ipc_read() 运行期间并发关闭句柄。 */
int main(int argc, char **argv)
{
    IPC_CAPTURE_CONTEXT context =
    {
        0,
    };
    pthread_t thread =
    {
        0,
    };
    struct timespec interval =
    {
        0,
    };
    unsigned long seconds = 0;
    char *end = NULL;
    double deadline = 0;
    int error = 0;

    if (argc < 3 || argc > 4)
    {
        fprintf(stderr, "Usage: %s IP|rtsp-url FORMAT [seconds; 0=continuous]\n", argv[0]);
        return 2;
    }
    if (argc == 4)
    {
        errno = 0;
        seconds = strtoul(argv[3], &end, 10);
        if (!argv[3][0] || argv[3][0] == '-' || *end || errno || seconds > 86400)
        {
            fprintf(stderr, "Invalid duration\n");
            return 2;
        }
    }

    ipc_config_init(&context.config);
    if (ipc_format_parse(argv[2], &context.config.format) != IPC_OK)
    {
        return 2;
    }
    if (strstr(argv[1], "://"))
    {
        context.config.url = argv[1];
    }
    else
    {
        context.config.ip = argv[1];
    }
    if (!configure_transport(&context.config))
    {
        fprintf(stderr, "Invalid transport\n");
        return 2;
    }
    context.config.path = getenv("IPC_PATH");
    context.config.username = getenv("IPC_USER");
    context.config.password = getenv("IPC_PASSWORD");
    context.result = 0;
    atomic_init(&context.stop, 0);
    atomic_init(&context.done, 0);
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);

    error = pthread_create(&thread, NULL, capture_thread, &context);
    if (error)
    {
        fprintf(stderr, "pthread_create: %s\n", strerror(error));
        return 1;
    }
    deadline = monotonic_seconds() + (double)seconds;
    while (!g_interrupted && !atomic_load(&context.done))
    {
        if (seconds && monotonic_seconds() >= deadline)
        {
            break;
        }
        interval.tv_sec = 0;
        interval.tv_nsec = 100000000;
        nanosleep(&interval, NULL);
    }

    /* 停止请求在下次读取返回后生效，另外还需帧处理和管线关闭时间。 */
    atomic_store(&context.stop, 1);
    error = pthread_join(thread, NULL);
    if (error)
    {
        fprintf(stderr, "pthread_join: %s\n", strerror(error));
        return 1;
    }
    return context.result;
}
