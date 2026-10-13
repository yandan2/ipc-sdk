# RK3588 IPC 取流 SDK

Linux C 接口，通过 RTSP 接收 IPC 视频，自动识别 H.264/H.265，使用 Rockchip MPP 硬件解码，输出指定格式的原始像素。支持配置设备 IP、端口、流路径、用户名和密码。

## 文件说明

| 文件 | 用途 |
| --- | --- |
| `include/ipc_sdk.h` | 公共接口、配置参数和帧结构定义 |
| `src/ipc_sdk.c` | 取流、硬件解码和像素输出实现 |
| `examples/read_frames.c` | 同步取帧及首帧保存示例 |
| `examples/read_thread.c` | pthread 持续取流示例 |
| `Makefile` / `CMakeLists.txt` | 编译和安装 |
| `cmake/aarch64-linux-gnu.cmake` | 客户工具链的 ARM64 交叉编译配置 |
| `tools/build.sh` | CMake 构建入口 |

## 编译和安装

GStreamer 和 Rockchip MPP/RGA 由目标 Linux 固件提供。编译环境需要 GCC、CMake（3.16 或更新）、Make、pkg-config，以及匹配目标固件的 GStreamer/GLib 开发头文件、链接库和 `.pc` 文件。SDK 源码包不包含第三方库或工具链。

### 在 Linux x86_64 主机交叉编译

解压后进入 SDK 目录，只需指定板卡配套 SDK 中的 ARM64 Linux GCC 路径。构建脚本自动查询编译器的 sysroot 和工具链常见目标库目录：

```sh
sh tools/build.sh cross \
    -DCMAKE_C_COMPILER=/opt/toolchain/bin/aarch64-linux-gnu-gcc
```

等效的标准 CMake 命令：

```sh
cmake -S . -B build-aarch64 \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
    -DCMAKE_C_COMPILER=/opt/toolchain/bin/aarch64-linux-gnu-gcc \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-aarch64 -j4
```

工具链开发环境需要包含匹配固件的 ARM64 GStreamer/GLib 头文件、链接库和 `.pc` 文件。板上已有运行库与主机具备编译依赖是两件事；若使用的独立 GCC 未附带这些开发文件，需要使用完整的板卡配套 SDK。更换工具链时使用新的构建目录。

使用 Make 交叉编译：

```sh
make CROSS_COMPILE=/opt/toolchain/bin/aarch64-linux-gnu- BUILD_DIR=build-cross-make
```

也可直接指定编译器：

```sh
make CC=/opt/toolchain/bin/aarch64-linux-gnu-gcc BUILD_DIR=build-cross-make
```

### 在 RK3588 板端编译

板端直接使用已安装的 GStreamer/GLib 开发文件，通过系统 pkg-config 获取编译参数：

```sh
sh tools/build.sh native
# 或直接使用 CMake。
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release
cmake --build build-native -j4
# 或使用 Make，产物在 build/。
make
```

### 构建结果和客户链接

Make、CMake 和构建脚本均只生成 SDK 动态库 `libipc_sdk.so.1.0.0`，以及 `libipc_sdk.so.1`、`libipc_sdk.so` 链接名称。安装时仅安装库、公共头文件及 CMake 生成的 pkg-config 文件。`examples/` 保留示例源码，不参与 SDK 的构建和安装。

将交叉编译产物整理到交付目录：

```sh
cmake --install build-aarch64 --prefix "$PWD/output-aarch64"
```

在板端安装及链接客户程序：

```sh
sudo make install PREFIX=/usr/local
sudo ldconfig
cc customer.c -I/usr/local/include -L/usr/local/lib -lipc_sdk -o customer
```

使用 pthread 的客户程序在链接时增加 `-pthread`。CMake 安装提供 `ipc-sdk.pc`，可通过 `pkg-config --cflags --libs ipc-sdk` 获取编译参数。公共头文件兼容 C++，SDK 和 C 示例使用 C11。

板端运行使用目标固件提供的 GStreamer 插件、`mppvideodec`、Rockchip MPP/RGA 和内核驱动。

## 示例源码使用

`examples/` 中的 C 文件供客户参考接入方式。需要运行示例时，可在板端单独编译；以下以 Make 生成的 `build/` 目录为例：

```sh
cc -std=c11 -Wall -Wextra -Werror examples/read_frames.c \
    -Iinclude -Lbuild -lipc_sdk -Wl,-rpath,'$ORIGIN/build' -o read_frames
./read_frames 192.168.1.60 NV12
./read_frames 192.168.1.60 BGR first.bgr
```

使用 CMake 时，将链接目录 `-Lbuild` 和运行时查找路径 `$ORIGIN/build` 中的 `build` 改为对应构建目录。账号和密码通过 `IPC_USER` / `IPC_PASSWORD` 环境变量传入。

同步示例默认读取 120 帧。可使用以下环境变量配置：

| 环境变量 | 含义 | 默认值 |
| --- | --- | --- |
| `IPC_USER` / `IPC_PASSWORD` | 用户名 / 密码 | 空 |
| `IPC_PATH` | 设备流路径 | `/stream1` |
| `IPC_PORT` | RTSP 端口 | 554 |
| `IPC_FRAMES` | 读取帧数 | 120 |
| `IPC_LATENCY_MS` | 抖动缓冲时延，毫秒 | 50 |
| `IPC_TIMEOUT_MS` | RTSP 连接超时，毫秒 | 10000 |
| `IPC_CONVERSION` | 像素转换方式：auto / mpp / cpu | auto |

设备流路径按 IPC 实际配置填写。地址参数也可使用完整的 `rtsp://host/path` URL。保存文件为连续原始像素，读取时使用对应的格式、宽度和高度。

## C 接口使用

1. 声明并清零 `IPC_CONFIG`，调用 `ipc_config_init()` 设置默认参数。
2. 设置 `ip`、`path`、`username`、`password`、`format` 等字段。
3. 调用 `ipc_open()` 创建取流句柄。
4. 将 `IPC_FRAME` 清零，循环调用 `ipc_read()`。
5. 使用图像后调用 `ipc_frame_release()`，结束时调用 `ipc_close()`。

`ipc_config_init()` 会清零配置并填入默认值，不连接设备。因此必须先初始化，再设置 IP、账号、格式等参数；若先设置再初始化，这些自定义值会被覆盖。

`ipc_open()` 启动异步连接；连接、认证或解码过程中的错误通过后续 `ipc_read()` 和错误查询接口获取。

| 接口 | 用途 |
| --- | --- |
| `ipc_config_init()` | 初始化配置 |
| `ipc_config_set_transport()` | 按需修改传输模式 |
| `ipc_open()` / `ipc_close()` | 打开 / 关闭取流 |
| `ipc_read()` | 等待并读取一帧 |
| `ipc_frame_release()` | 释放帧资源 |
| `ipc_frame_size()` | 获取去除行填充后的像素字节数 |
| `ipc_frame_copy()` | 将有效像素复制到调用者缓冲区 |
| `ipc_get_codec()` | 获取自动识别的编码类型 |
| `ipc_last_error_code()` / `ipc_last_error()` | 获取错误码 / 错误描述 |
| `ipc_format_parse()` / `ipc_format_name()` | 转换格式名称与枚举值 |

`IPC_FRAME.data[]` 和 `stride[]` 分别表示平面地址及相邻行的字节偏移；`row_bytes[]` 和 `rows[]` 表示有效行字节数及行数。平面可能不连续，按 stride 逐行处理，或使用 `ipc_frame_copy()` 获得连续像素。帧数据在 `ipc_frame_release()` 前有效；释放后可复用已清零的帧结构体。每帧只释放一次，不复制私有所有权字段后分别释放。

## C 接入代码示例

将以下代码保存为 `customer.c`。示例输出 NV12，将每帧复制到可复用的业务缓冲区，并保存首帧为 `first-frame.raw`，读取 120 帧后结束；连续 15 秒无帧时退出，也可按 Ctrl-C 停止。账号和密码通过 `IPC_USER` / `IPC_PASSWORD` 环境变量提供。

```c
#include "ipc_sdk.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#define EXAMPLE_FRAME_COUNT 120U
#define EXAMPLE_READ_TIMEOUT_MS 200U
#define EXAMPLE_MAX_TIMEOUTS 75U

static volatile sig_atomic_t g_stop = 0;

/* Ctrl-C 只设置停止标志，资源在取流循环结束后统一释放。 */
static void stop_capture(int signal_number)
{
    (void)signal_number;
    g_stop = 1;
}

int main(void)
{
    IPC_CONFIG config =
    {
        0,
    };
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
    unsigned count = 0;
    unsigned timeouts = 0;
    int result = 0;
    uint8_t *pixels = NULL;
    uint8_t *resized = NULL;
    size_t image_bytes = 0;
    size_t image_capacity = 0;
    size_t written = 0;
    FILE *output = NULL;
    int close_result = 0;

    /* 先重置配置，再填写连接参数；ipc_config_init() 本身不连接设备。 */
    ipc_config_init(&config);
    config.ip = "192.168.1.60";
    config.port = 554;
    config.path = "/stream1";
    config.username = getenv("IPC_USER");
    config.password = getenv("IPC_PASSWORD");
    config.format = IPC_NV12;

    signal(SIGINT, stop_capture);
    signal(SIGTERM, stop_capture);
    handle = ipc_open(&config, error, sizeof(error));
    if (!handle)
    {
        fprintf(stderr, "打开失败：%s\n", error);
        return 1;
    }

    while (!g_stop && count < EXAMPLE_FRAME_COUNT)
    {
        /* 无帧时阻塞等待；有帧立即返回，无需额外 sleep。 */
        status = ipc_read(handle, EXAMPLE_READ_TIMEOUT_MS, &frame);
        if (g_stop)
        {
            ipc_frame_release(&frame);
            break;
        }
        if (status == IPC_TIMEOUT)
        {
            ++timeouts;
            if (timeouts >= EXAMPLE_MAX_TIMEOUTS)
            {
                fprintf(stderr, "连续 15 秒未收到图像\n");
                result = 1;
                break;
            }
            continue;
        }
        if (status == IPC_EOS)
        {
            break;
        }
        if (status != IPC_OK)
        {
            fprintf(stderr, "取帧失败：%s\n", ipc_last_error(handle));
            result = 1;
            break;
        }

        timeouts = 0;
        if (count == 0)
        {
            printf("图像：%ux%u，格式：%s，平面数：%u\n", frame.width, frame.height,
                   ipc_format_name(frame.format), frame.planes);
        }

        /* 将 SDK 帧复制到业务缓冲区，缓冲区可复用，尺寸变化时再扩容。 */
        image_bytes = ipc_frame_size(&frame);
        if (image_bytes == 0)
        {
            ipc_frame_release(&frame);
            result = 1;
            break;
        }
        if (image_bytes > image_capacity)
        {
            resized = realloc(pixels, image_bytes);
            if (!resized)
            {
                ipc_frame_release(&frame);
                result = 1;
                break;
            }
            pixels = resized;
            image_capacity = image_bytes;
        }
        if (ipc_frame_copy(&frame, pixels, image_capacity) != IPC_OK)
        {
            ipc_frame_release(&frame);
            result = 1;
            break;
        }

        /* pixels 已独立持有连续图像；先归还解码帧，再执行文件等业务操作。 */
        ipc_frame_release(&frame);
        if (count == 0)
        {
            output = fopen("first-frame.raw", "wb");
            if (!output)
            {
                result = 1;
                break;
            }
            written = fwrite(pixels, 1, image_bytes, output);
            close_result = fclose(output);
            output = NULL;
            if (written != image_bytes || close_result != 0)
            {
                result = 1;
                break;
            }
            printf("首帧已保存：first-frame.raw，%zu 字节\n", image_bytes);
        }
        ++count;
    }

    free(pixels);
    pixels = NULL;
    ipc_close(handle);
    printf("已读取 %u 帧\n", count);
    return result;
}
```

在 SDK 源码目录中编译、运行：

```sh
make
cc -std=c11 -Wall -Wextra -Werror customer.c -Iinclude -Lbuild \
    -lipc_sdk -Wl,-rpath,'$ORIGIN/build' -o customer
./customer
```

需要其他输出格式时修改 `config.format`；例如 `IPC_BGR24` 表示每像素按 B、G、R 排列。使用独立取流线程的完整程序见 `examples/read_thread.c`。

## IPC_HANDLE 句柄类型

头文件中的声明：

```c
typedef struct IPC_HANDLE IPC_HANDLE;
```

为 `struct IPC_HANDLE` 提供类型别名 `IPC_HANDLE`，并对结构体作前向声明。它不包含成员定义，也不分配内存。完整结构体在 `src/ipc_sdk.c` 中定义，用于保存一路取流会话的管线、解码器、帧队列入口、编码类型和错误状态。

客户程序使用 `IPC_HANDLE *handle` 保存 `ipc_open()` 返回的对象地址，将同一个指针传给 `ipc_read()` 等接口，最后通过 `ipc_close()` 释放资源。每次成功打开都会得到独立的会话对象。

这种方式称为“不透明句柄”：头文件只公开类型名称，内部成员由 SDK 管理。客户可以声明、保存和传递句柄指针，但不能通过公共头文件访问成员、声明 `IPC_HANDLE handle` 实体或计算 `sizeof(IPC_HANDLE)`；也不应自行 `malloc/free` 该对象。这样可保留指针类型检查，并将 GStreamer 等内部实现与客户接口分开。

## 持续取流线程

`examples/read_thread.c` 展示主线程创建取流线程、请求停止和回收线程的完整流程。需要运行时在板端单独编译：

```sh
cc -std=c11 -Wall -Wextra -Werror -pthread examples/read_thread.c \
    -Iinclude -Lbuild -lipc_sdk -Wl,-rpath,'$ORIGIN/build' -o read_thread
# 持续运行，Ctrl-C 停止。
./read_thread 192.168.1.60 NV12
# 运行 30 秒后停止。
./read_thread 192.168.1.60 BGR 30
```

线程示例支持通过 `IPC_USER`、`IPC_PASSWORD`、`IPC_PATH` 设置连接信息。每帧复制到线程自有缓冲区，并将首帧保存到 `first-frame.raw`。同一句柄的打开、读取和关闭由取流线程串行执行；主线程设置原子停止标志，再调用 `pthread_join()` 等待退出。

取流线程使用 `ipc_read(handle, 200, &frame)`。有帧时立即返回，无帧时阻塞等待，按返回状态处理：

- `IPC_OK`：处理图像并释放帧，再读取下一帧。
- `IPC_TIMEOUT`：检查停止条件，继续等待。
- `IPC_EOS` / `IPC_ERROR`：结束本次连接；需要重连时，在关闭后退避一段时间再打开。

`timeout_ms=0` 为立即轮询，持续取流应使用非零超时。`ipc_read()` 从内部帧队列等待数据，不需要客户额外调用 `select()`。不要对错误或 EOS 紧密循环重试，也不要在另一个线程中并发关闭正在读取的句柄。

收到停止请求后，取流线程在本次读取返回后退出，并完成帧资源释放和管线关闭。耗时算法可将像素复制到有界业务队列，再及时释放 SDK 帧。

## 图像格式

| 格式 | 内存排列 | 偶数宽高时的连续像素大小 |
| --- | --- | --- |
| `IPC_NV12` | Y 平面 + UV 交错平面 | W × H × 3/2 |
| `IPC_NV21` | Y 平面 + VU 交错平面 | W × H × 3/2 |
| `IPC_I420` | Y、U、V 三平面 | W × H × 3/2 |
| `IPC_RGB24` | R、G、B | W × H × 3 |
| `IPC_BGR24` | B、G、R | W × H × 3 |
| `IPC_RGBA` | R、G、B、A | W × H × 4 |
| `IPC_BGRA` | B、G、R、A | W × H × 4 |
| `IPC_GRAY8` | Y 单平面 | W × H |

输出保持解码尺寸；奇数宽高的色度尺寸向上取整，实际大小通过 `ipc_frame_size()` 获取。GRAY8 保留 Y 分量原有亮度范围。

视频解码始终使用 MPP。`IPC_CONVERT_AUTO` 优先选择 MPP 插件的像素转换能力；I420 在 CPU 上拆分 NV12 的 UV，GRAY8 提供 Y 平面视图。`IPC_CONVERT_CPU` 使用硬解后的 NV12 做像素处理；`IPC_CONVERT_MPP_ONLY` 只接受 MPP 插件可直接输出的格式，I420/GRAY8 返回格式错误。每帧的 `conversion` 字段给出实际采用的路径。

队列默认保留 2 帧，满时丢弃最旧帧，可通过 `queue_frames` 调整。`pts_ns` 为媒体时间戳，无效时为 -1；`ready_monotonic_ns` 为 SDK 取到帧时的单调时钟纳秒值。
