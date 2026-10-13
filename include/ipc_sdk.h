/**
 * @file ipc_sdk.h
 * @brief RK3588 IPC 取流 SDK 的公共 C 接口。
 *
 * 接收 RTSP 中的 H.264/H.265 视频，使用 Rockchip MPP 硬件解码，输出原始像素。
 * 此头文件不暴露 GStreamer 类型；客户程序可用 C 或 C++ 调用。
 * 同一句柄由一个业务线程串行操作，禁止 ipc_read() 与 ipc_close() 并发执行。
 * 所有自定义类型、枚举项和宏使用大写名称，函数及成员使用小写下划线名称。
 */
#ifndef IPC_SDK_H
#define IPC_SDK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** SDK 版本字符串；接口布局及枚举数值保持 ABI 1 兼容。 */
#define IPC_SDK_VERSION "1.0.0"

/**
 * 一路取流会话的不透明类型：此处仅前向声明并定义类型别名，不分配内存。
 * 完整成员在 src/ipc_sdk.c 中定义，由 SDK 管理管线、解码器和会话状态。
 * 客户只持有 IPC_HANDLE *，通过 ipc_open() 获取、ipc_close() 释放。
 * 公共头文件不提供成员布局，不能据此声明实体、访问成员或计算结构体大小。
 */
typedef struct IPC_HANDLE IPC_HANDLE;

/** 原始像素格式。RGB24/BGR24 的数值与最初版本保持一致，不按展示顺序重排。 */
typedef enum IPC_FORMAT
{
    IPC_NV12 = 0,    /**< Y 平面 + UV 交错平面，8 位 YUV420。 */
    IPC_RGB24 = 1,   /**< 单平面，每像素依次为 R、G、B，共 3 字节。 */
    IPC_BGR24 = 2,   /**< 单平面，每像素依次为 B、G、R，共 3 字节。 */
    IPC_NV21 = 3,    /**< Y 平面 + VU 交错平面，8 位 YUV420。 */
    IPC_I420 = 4,    /**< Y、U、V 三个独立平面，8 位 YUV420。 */
    IPC_RGBA = 5,    /**< 单平面，每像素依次为 R、G、B、A，共 4 字节。 */
    IPC_BGRA = 6,    /**< 单平面，每像素依次为 B、G、R、A，共 4 字节。 */
    IPC_GRAY8 = 7,   /**< 单平面，每像素为原始 Y 分量，保留其亮度范围。 */
    IPC_FORMAT_COUNT /**< 格式数量及校验上界，不可作为输出格式。 */
} IPC_FORMAT;

/** 根据 RTSP SDP/RTP 能力自动识别的视频编码。 */
typedef enum IPC_CODEC
{
    IPC_CODEC_UNKNOWN, /**< 尚未识别，或句柄无效。 */
    IPC_CODEC_H264,    /**< H.264 / AVC。 */
    IPC_CODEC_H265     /**< H.265 / HEVC。 */
} IPC_CODEC;

/**
 * 像素转换策略。三种策略的视频解码均由 MPP 硬件完成。
 * CPU 路径仅作用于硬解后的像素排列或颜色转换，不代表软件视频解码。
 */
typedef enum IPC_CONVERSION
{
    IPC_CONVERT_AUTO, /**< 优先使用 MPP 插件能力，不支持的布局采用 CPU 处理。 */
    IPC_CONVERT_MPP_ONLY, /**< 只允许 MPP 直接输出；I420/GRAY8 及不支持的格式报错。 */
    IPC_CONVERT_CPU       /**< MPP 输出 NV12 后处理像素；NV12 本身无需转换。 */
} IPC_CONVERSION;

/** RTP 传输选择；AUTO 在连接阶段先尝试 UDP，必要时回退 TCP。 */
typedef enum IPC_TRANSPORT
{
    IPC_TRANSPORT_TCP = 0, /**< 固定使用 TCP 交织传输。 */
    IPC_TRANSPORT_UDP = 1, /**< 固定使用 UDP 单播传输。 */
    IPC_TRANSPORT_AUTO = 2 /**< UDP 不支持或初始接收超时后尝试 TCP。 */
} IPC_TRANSPORT;

/** API 返回状态。超时与流结束不是 IPC_ERROR，应由业务分别处理。 */
typedef enum IPC_STATUS
{
    IPC_OK = 0,      /**< 操作成功；ipc_read() 成功时产生一帧。 */
    IPC_TIMEOUT = 1, /**< 本次等待期内没有可用帧，可继续读取。 */
    IPC_EOS = 2,     /**< 管线已经收到流结束消息。 */
    IPC_ERROR = -1   /**< 操作失败；句柄相关错误可进一步查询错误码。 */
} IPC_STATUS;

/** 错误分类。致命错误在当前句柄中保留，关闭并重新打开后才能清除。 */
typedef enum IPC_ERROR_CODE
{
    IPC_ERR_NONE,     /**< 没有记录错误。 */
    IPC_ERR_ARGUMENT, /**< 配置、句柄或帧参数无效，或复用了尚未释放的帧。 */
    IPC_ERR_INIT,     /**< GStreamer 初始化失败。 */
    IPC_ERR_MEMORY,   /**< SDK 内存分配失败。 */
    IPC_ERR_PLUGIN,   /**< 缺少必要的 GStreamer / MPP 插件。 */
    IPC_ERR_FORMAT,   /**< 当前转换策略无法输出所请求的像素格式。 */
    IPC_ERR_CODEC,    /**< 没有受支持的 H.264/H.265 视频流。 */
    IPC_ERR_AUTH,     /**< RTSP 认证失败或访问被拒绝。 */
    IPC_ERR_NETWORK,  /**< RTSP 连接或传输失败。 */
    IPC_ERR_DECODE,   /**< MPP 硬解码器报告错误。 */
    IPC_ERR_MAP,      /**< 帧格式、布局或 CPU 映射校验失败。 */
    IPC_ERR_PIPELINE  /**< 其他管线连接、协商或运行错误。 */
} IPC_ERROR_CODE;

/**
 * 打开取流连接所需的配置。请先调用 ipc_config_init()，再修改所需字段。
 * 所有字符串由调用者持有，在 ipc_open() 返回前必须有效；返回后可释放。
 * 这里的 ip 是连接目标，不会修改摄像头本身的网络配置。
 */
typedef struct IPC_CONFIG
{
    /** 完整 rtsp:// 或 rtsps:// URL；非 NULL 时优先于 ip、path、port。
     * 支持主机名，不允许内嵌账号密码、空白、控制字符或 # 片段。 */
    const char *url;

    /** RTSP 用户名；NULL 表示不设置认证用户名。 */
    const char *username;

    /** RTSP 密码；NULL 表示不设置密码。不要将真实密码写入交付源码。 */
    const char *password;

    /** url 为 NULL 时使用的 IPv4/IPv6 字面地址；IPv6 不带方括号。 */
    const char *ip;

    /** url 为 NULL 时使用的流路径，须以 / 开头；NULL 默认 /stream1。 */
    const char *path;

    /** url 为 NULL 时使用的 RTSP 端口，0 表示 554，最大值 65535。 */
    unsigned port;

    /** 期望输出的像素格式，默认 IPC_NV12。 */
    IPC_FORMAT format;

    /** RTP 抖动缓冲时延，单位毫秒；默认 50，允许 0～60000。 */
    unsigned latency_ms;

    /** 传输模式存储字段，保留原字段名和布局；0=TCP、1=UDP、2=AUTO。
     * 初始化后默认 AUTO；需要修改时通过 ipc_config_set_transport() 设置。 */
    int udp;

    /** 非 0 启用设备兼容处理：忽略错误的 RTP 初始序号声明。
     * 仅对已确认存在此问题的设备启用；不改写实际 RTP 包序号。 */
    int ignore_seqnum_base;

    /** RTSP TCP 连接/读取超时，单位毫秒；0 表示 10000，最大 3600000。
     * 此字段控制 rtspsrc 的 tcp-timeout；与 ipc_read() 的等待超时独立。 */
    unsigned network_timeout_ms;

    /** appsink 最大排队帧数；0 表示 2，最大 64，队列满时丢弃最旧帧。 */
    unsigned queue_frames;

    /** 像素转换策略，默认 IPC_CONVERT_AUTO。 */
    IPC_CONVERSION conversion;
} IPC_CONFIG;

/**
 * 一帧只读图像及其布局信息。首次使用须将整个结构体清零。
 * ipc_read() 成功后必须调用 ipc_frame_release()；禁止复制后分别释放两个副本。
 * SDK 返回的平面指针在释放前有效，即使取流句柄已经关闭。
 * 不保证平面连续或 stride 等于有效行字节数；禁止直接按宽 × 高跨平面读取。
 */
typedef struct IPC_FRAME
{
    /** 视频有效宽度，单位像素；不包含行对齐填充。 */
    unsigned width;

    /** 视频有效高度，单位像素。 */
    unsigned height;

    /** 有效平面数：NV12/NV21 为 2，I420 为 3，RGB 类和 GRAY8 为 1。 */
    unsigned planes;

    /** 实际输出的像素格式。 */
    IPC_FORMAT format;

    /** 各平面首行只读地址；仅下标 [0, planes) 有效。 */
    const uint8_t *data[4];

    /** 各平面相邻两行的字节偏移，可含填充；通用复制接口支持负 stride。 */
    int stride[4];

    /** 媒体显示时间戳，单位纳秒，无效时为 -1；不是摄像头曝光 UTC 时间。 */
    int64_t pts_ns;

    /** SDK 取到样本时的 CLOCK_MONOTONIC 纳秒值，不是端到端时延。 */
    uint64_t ready_monotonic_ns;

    /** SDK 私有所有权信息。调用者不得修改、释放或解释其内容。 */
    void *private_data;

    /** 各平面每行的有效字节数，不包括行尾填充。 */
    unsigned row_bytes[4];

    /** 各平面的有效行数；YUV420 色度宽高按奇数尺寸向上取整。 */
    unsigned rows[4];

    /** 此帧所属流的编码类型。 */
    IPC_CODEC codec;

    /** 实际采用的转换路径，为 MPP_ONLY 或 CPU；GRAY8 的 Y 平面视图记为 CPU。 */
    IPC_CONVERSION conversion;
} IPC_FRAME;

/**
 * @brief 初始化配置默认值，其余字段清零。
 * @param config 待初始化的配置；NULL 时不执行操作。
 * @note 默认 NV12、AUTO、50 ms 抖动缓冲、10000 ms TCP 超时及 2 帧队列。
 *       此函数重置配置，不连接设备。必须先调用，再填写 IP、账号等自定义参数。
 */
void ipc_config_init(IPC_CONFIG *config);

/**
 * @brief 修改传输模式，在 ipc_open() 前按需调用；不改变已打开的连接。
 * @param config 已初始化的配置。
 * @param transport TCP、UDP 或 AUTO。
 * @return 成功返回 IPC_OK；参数无效返回 IPC_ERROR，原配置保持不变。
 * @note ipc_config_init() 已默认设置 AUTO，使用默认模式时无需调用本函数。
 *       AUTO 先协商 UDP 单播；不支持 UDP 或初始 UDP 接收 5 秒无数据时尝试 TCP。
 *       流已正常建立后的中断仍由业务通过读取超时/错误执行重连。
 */
IPC_STATUS ipc_config_set_transport(IPC_CONFIG *config, IPC_TRANSPORT transport);

/** @return SDK 静态版本字符串；调用者不得修改或释放。 */
const char *ipc_version(void);

/** @return 格式的静态标准名称（如 RGB/BGR/GRAY8）；无效格式返回 UNKNOWN。 */
const char *ipc_format_name(IPC_FORMAT format);

/**
 * @brief 解析不区分大小写的格式名称，同时接受 RGB24、BGR24、GRAY 别名。
 * @param name 输入字符串。
 * @param format 成功时写入格式；失败时保持原值。
 * @return IPC_OK 或 IPC_ERROR；两个指针均不得为 NULL。
 */
IPC_STATUS ipc_format_parse(const char *name, IPC_FORMAT *format);

/**
 * @brief 创建 MPP 解码管线并启动异步 RTSP 连接。
 * @param config 配置指针，不得为 NULL。
 * @param error 可选的错误文本缓冲区；内容为不含凭据的通用描述。
 * @param error_size 缓冲区总字节数，包含结尾的 NUL；0 表示不写入。
 * @return 成功时返回句柄，失败返回 NULL；成功时清空有效的 error 缓冲区。
 * @note 成功返回只代表管线已启动，后续认证、协商和解码错误由 ipc_read() 报告。
 */
IPC_HANDLE *ipc_open(const IPC_CONFIG *config, char *error, unsigned error_size);

/**
 * @brief 等待并取得一帧。
 * @param handle 有效句柄。
 * @param timeout_ms 本次等待样本的最大毫秒数；0 为立即轮询。
 * @param frame 调用者提供的已清零或已释放的帧结构体。
 * @return IPC_OK / IPC_TIMEOUT / IPC_EOS / IPC_ERROR。
 * @note 超时仅限制等待，帧映射、转换及调度有额外耗时；SDK 不自动重连。
 *       成功后应及时释放帧，避免长期占用 MPP 缓冲池。禁止覆盖未释放的帧。
 *       持续取流线程建议使用 200 ms 等非零超时；无帧时在内部帧队列上等待。
 *       不要循环零超时轮询；ERROR/EOS 会立即返回，应退出或退避后重连。
 */
IPC_STATUS ipc_read(IPC_HANDLE *handle, unsigned timeout_ms, IPC_FRAME *frame);

/**
 * @brief 归还帧持有的映射、样本或自有内存，并将结构体清零。
 * @param frame SDK 返回的帧或已清零的帧；允许为 NULL。
 * @note 已清零的同一个结构体可以重复释放；释放后所有原像素指针失效。
 */
void ipc_frame_release(IPC_FRAME *frame);

/** @return 最近错误的静态通用描述；不含 RTSP 凭据，不需调用者释放。 */
const char *ipc_last_error(IPC_HANDLE *handle);

/** @return 首个致命错误，或最近的读取参数错误；NULL 句柄返回 IPC_ERR_ARGUMENT。 */
IPC_ERROR_CODE ipc_last_error_code(IPC_HANDLE *handle);

/** @return 已识别的编码类型；尚未识别或句柄为 NULL 时返回 IPC_CODEC_UNKNOWN。 */
IPC_CODEC ipc_get_codec(IPC_HANDLE *handle);

/**
 * @brief 校验帧布局并计算去除行填充后所需的连续存储字节数。
 * @param frame 布局、平面指针和 stride 均有效的帧。
 * @return 所需字节数；参数无效、布局不匹配或整数溢出时返回 0。
 */
size_t ipc_frame_size(const IPC_FRAME *frame);

/**
 * @brief 按平面顺序逐行复制有效像素，去除每行填充。
 * @param frame 有效源帧；源内存必须可读。
 * @param dst 调用者分配的可写连续缓冲区，不得与源帧内存重叠。
 * @param capacity dst 的总字节数，至少为 ipc_frame_size() 的返回值。
 * @return IPC_OK 或 IPC_ERROR；参数或容量校验失败时不会执行部分复制。
 * @note NV12 为 Y/UV，NV21 为 Y/VU，I420 为 Y/U/V；RGB 类按原像素顺序复制。
 */
IPC_STATUS ipc_frame_copy(const IPC_FRAME *frame, void *dst, size_t capacity);

/**
 * @brief 停止管线并销毁句柄；允许传入 NULL。
 * @note 调用后句柄失效；不得与该句柄的其他操作并发。
 *       已取出的帧仍须由调用者独立释放，可在关闭后继续读取帧像素。
 */
void ipc_close(IPC_HANDLE *handle);

#ifdef __cplusplus
}
#endif

#endif /* IPC_SDK_H */
