/**
 * @file ipc_sdk.c
 * @brief RTSP 接收、MPP 硬解码、像素映射及帧生命周期实现。
 * @version 1.0.0
 * @date 2026-10-08
 *
 * 对外接口：include/ipc_sdk.h；调用示例：examples/read_frames.c。
 * 主路径：rtspsrc -> RTP 解包 -> H.264/H.265 解析 -> mppvideodec
 *       -> 可选像素转换 -> appsink -> IPC_FRAME。
 * GStreamer 流线程执行网络回调；业务线程通过同步接口读取帧。
 * 修改记录：2026-10-08，统一类型命名、非紧凑排版并补充中文维护说明。
 */
#define _POSIX_C_SOURCE 200809L
#include "ipc_sdk.h"
#include <gst/gst.h>
#include <gio/gio.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <gst/rtsp/gstrtspurl.h>
#include <gst/sdp/gstsdpmessage.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

/* RTSP 配置边界及默认值集中维护，单位由名称标明。 */
typedef enum IPC_CONFIG_LIMIT
{
    IPC_DEFAULT_RTSP_PORT = 554,
    IPC_MAX_RTSP_PORT = 65535,
    IPC_DEFAULT_LATENCY_MS = 50,
    IPC_MAX_LATENCY_MS = 60000,
    IPC_DEFAULT_NETWORK_TIMEOUT_MS = 10000,
    IPC_MAX_NETWORK_TIMEOUT_MS = 3600000,
    IPC_DEFAULT_QUEUE_FRAMES = 2,
    IPC_MAX_QUEUE_FRAMES = 64,
    IPC_MAX_URL_LENGTH = 4096,
    IPC_MAX_PATH_LENGTH = 2048
} IPC_CONFIG_LIMIT;

/* 单次 appsink 等待上限为 20 ms，用于及时响应异步错误。 */
static const uint64_t IPC_READ_SLICE_NS = 20000000ULL;

/* 初始 UDP 无数据时，自动模式等待 5 秒后尝试 TCP。 */
static const uint64_t IPC_AUTO_UDP_TIMEOUT_US = 5000000ULL;

struct IPC_HANDLE
{
    GstElement *pipeline; /* 拥有管线内各元素，关闭时统一归还。 */
    GstElement *source;   /* 管线拥有的 RTSP 源，句柄仅保存借用指针。 */
    GstElement *decoder;  /* 显式创建的 MPP 硬解码器，不使用自动软件解码。 */
    GstElement *sink;     /* 管线拥有的 appsink，向同步读取接口提供样本。 */
    GstBus *bus;          /* 句柄持有独立引用，关闭时先卸载同步消息回调。 */
    gint failed;   /* 首个致命错误，流线程与调用线程通过原子操作共享。 */
    gint selected; /* 选中的流编号加一；0 表示尚未选择。 */
    gint linked;   /* 动态 RTP pad 是否已连接，防止重复建链。 */
    gint codec;    /* IPC_CODEC 枚举值，原子发布自动识别结果。 */
    gint eos;      /* 仅由总线 EOS 消息置位，不把连接中误判为流结束。 */
    IPC_ERROR_CODE argument_error; /* 仅业务调用线程访问的最近参数错误。 */
    gboolean ignore_seqnum_base;   /* 是否启用设备 RTP 序号兼容处理。 */
    gboolean layout_cpu;           /* I420 拆分或 GRAY8 视图，输入始终为 NV12。 */
    IPC_FORMAT format;             /* 业务请求的最终像素格式。 */
    IPC_CONVERSION conversion;     /* 实际采用的像素转换路径。 */
};

/* 每帧独立持有资源，因此帧可在 ipc_close() 后继续使用，直至显式释放。 */
typedef struct IPC_FRAME_OWNER
{
    GstSample *sample; /* 持有样本引用，使 MPP 缓冲区在帧使用期间保持有效。 */
    GstVideoFrame mapped; /* CPU 只读映射，与 sample 成对建立/解除。 */
    uint8_t *allocation;  /* I420 自有连续缓冲区；其他格式为 NULL。 */
} IPC_FRAME_OWNER;

static const char *const IPC_FORMAT_NAMES[] =
{
    "NV12",
    "RGB",
    "BGR",
    "NV21",
    "I420",
    "RGBA",
    "BGRA",
    "GRAY8",
};

/* 返回单调时钟纳秒值，用于读取期限和就绪时间，不受系统校时影响。 */
static uint64_t monotonic_ns(void)
{
    struct timespec timestamp;

    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return (uint64_t)timestamp.tv_sec * 1000000000ULL + (uint64_t)timestamp.tv_nsec;
}

/* 公共接口定义及返回值约定详见 ipc_sdk.h；版本由同一宏维护。 */
const char *ipc_version(void)
{
    return IPC_SDK_VERSION;
}

/* 枚举序号必须与格式名称表对应；未知枚举不访问数组。 */
const char *ipc_format_name(IPC_FORMAT format)
{
    return format >= 0 && format < IPC_FORMAT_COUNT ? IPC_FORMAT_NAMES[format] : "UNKNOWN";
}

/* 不区分大小写解析名称；只在匹配成功时写入输出参数。 */
IPC_STATUS ipc_format_parse(const char *name, IPC_FORMAT *format)
{
    int i;

    if (!name || !format)
    {
        return IPC_ERROR;
    }
    for (i = 0; i < IPC_FORMAT_COUNT; ++i)
    {
        if (!g_ascii_strcasecmp(name, IPC_FORMAT_NAMES[i]))
        {
            *format = (IPC_FORMAT)i;
            return IPC_OK;
        }
    }
    if (!g_ascii_strcasecmp(name, "RGB24"))
    {
        *format = IPC_RGB24;
    }
    else if (!g_ascii_strcasecmp(name, "BGR24"))
    {
        *format = IPC_BGR24;
    }
    else if (!g_ascii_strcasecmp(name, "GRAY"))
    {
        *format = IPC_GRAY8;
    }
    else
    {
        return IPC_ERROR;
    }
    return IPC_OK;
}

/* 先清零配置，再设置 AUTO 传输、像素格式、接收时延及队列等默认值。 */
void ipc_config_init(IPC_CONFIG *config)
{
    if (!config)
    {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->format = IPC_NV12;
    config->udp = IPC_TRANSPORT_AUTO;
    config->latency_ms = IPC_DEFAULT_LATENCY_MS;
    config->network_timeout_ms = IPC_DEFAULT_NETWORK_TIMEOUT_MS;
    config->queue_frames = IPC_DEFAULT_QUEUE_FRAMES;
}

/* 保留 IPC_CONFIG 布局，将新传输枚举写入既有存储字段。 */
IPC_STATUS ipc_config_set_transport(IPC_CONFIG *config, IPC_TRANSPORT transport)
{
    if (!config || transport < IPC_TRANSPORT_TCP || transport > IPC_TRANSPORT_AUTO)
    {
        return IPC_ERROR;
    }
    config->udp = (int)transport;
    return IPC_OK;
}

/* AUTO 只允许 UDP 单播与 TCP，不启用组播；协商顺序由 rtspsrc 管理。 */
static GstRTSPLowerTrans transport_protocols(int mode)
{
    switch (mode)
    {
    case IPC_TRANSPORT_AUTO:
        return GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP;
    case IPC_TRANSPORT_UDP:
        return GST_RTSP_LOWER_TRANS_UDP;
    default:
        return GST_RTSP_LOWER_TRANS_TCP;
    }
}

/* 将内部错误码映射为稳定的通用文本，不直接暴露底层错误内容。 */
static const char *error_text(IPC_ERROR_CODE code)
{
    switch (code)
    {
    case IPC_ERR_NONE:
        return "";
    case IPC_ERR_ARGUMENT:
        return "Invalid configuration, handle or unreleased frame";
    case IPC_ERR_INIT:
        return "GStreamer initialization failed";
    case IPC_ERR_MEMORY:
        return "Frame memory allocation failed";
    case IPC_ERR_PLUGIN:
        return "Required GStreamer/MPP plugin missing";
    case IPC_ERR_FORMAT:
        return "Requested pixel format is unavailable in the selected conversion mode";
    case IPC_ERR_CODEC:
        return "No supported H.264/H.265 video stream";
    case IPC_ERR_AUTH:
        return "RTSP authentication or access denied";
    case IPC_ERR_NETWORK:
        return "RTSP connection or stream transport failed";
    case IPC_ERR_DECODE:
        return "MPP hardware decoding failed";
    case IPC_ERR_MAP:
        return "Output frame format or CPU mapping failed";
    default:
        return "Pipeline link, negotiation or streaming failed";
    }
}

/* 流线程可能同时报错；原子比较交换保证保留首个故障原因。 */
static void fail(IPC_HANDLE *handle, IPC_ERROR_CODE code)
{
    g_atomic_int_compare_and_exchange(&handle->failed, IPC_ERR_NONE, code);
}

/* 致命错误优先于可恢复的调用参数错误；只读查询不会清除错误。 */
IPC_ERROR_CODE ipc_last_error_code(IPC_HANDLE *handle)
{
    int code;

    if (!handle)
    {
        return IPC_ERR_ARGUMENT;
    }
    code = g_atomic_int_get(&handle->failed);
    return code ? (IPC_ERROR_CODE)code : handle->argument_error;
}

/* 返回静态字符串，不向调用者转移内存所有权。 */
const char *ipc_last_error(IPC_HANDLE *handle)
{
    return error_text(ipc_last_error_code(handle));
}

/* 编码由动态建链回调发布；读取方使用原子操作获取结果。 */
IPC_CODEC ipc_get_codec(IPC_HANDLE *handle)
{
    return handle ? (IPC_CODEC)g_atomic_int_get(&handle->codec) : IPC_CODEC_UNKNOWN;
}

/*
 * 管线消息在发出它的线程中同步消费，不依赖客户启动 GLib 主循环。
 * 即使客户暂停读取，也不会无限积累消息；仅保存首个致命错误分类。
 * 禁止向外转发可能包含 URL/凭据的 GError 和调试字符串。
 */
static GstBusSyncReply bus_message(GstBus *bus, GstMessage *msg, gpointer data)
{
    IPC_HANDLE *handle;
    GError *err;
    IPC_ERROR_CODE code;

    (void)bus;
    handle = data;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
    {
        err = NULL;
        gst_message_parse_error(msg, &err, NULL);
        code = IPC_ERR_PIPELINE;
        if (err && err->domain == GST_RESOURCE_ERROR
            && err->code == GST_RESOURCE_ERROR_NOT_AUTHORIZED)
        {
            code = IPC_ERR_AUTH;
        }
        else if (handle->source
                 && (GST_MESSAGE_SRC(msg) == GST_OBJECT(handle->source)
                     || gst_object_has_as_ancestor(GST_MESSAGE_SRC(msg),
                                                   GST_OBJECT(handle->source))))
        {
            code = IPC_ERR_NETWORK;
        }
        else if (handle->decoder && GST_MESSAGE_SRC(msg) == GST_OBJECT(handle->decoder))
        {
            code = IPC_ERR_DECODE;
        }
        else if (err && err->domain == GST_STREAM_ERROR
                 && err->code == GST_STREAM_ERROR_CODEC_NOT_FOUND)
        {
            code = IPC_ERR_CODEC;
        }
        fail(handle, code);
        g_clear_error(&err);
    }
    else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
    {
        g_atomic_int_set(&handle->eos, 1);
    }
    gst_message_unref(msg);
    return GST_BUS_DROP;
}

/* 只接收声明为 video 且编码名为 H264/H265 的 RTP 能力。 */
static gboolean supported_caps(GstCaps *caps)
{
    const GstStructure *structure;
    const gchar *media;
    const gchar *encoding;

    if (!caps || gst_caps_is_empty(caps) || gst_caps_is_any(caps))
    {
        return FALSE;
    }
    structure = gst_caps_get_structure(caps, 0);
    media = gst_structure_get_string(structure, "media");
    encoding = gst_structure_get_string(structure, "encoding-name");
    return media && encoding && !g_ascii_strcasecmp(media, "video")
           && (!g_ascii_strcasecmp(encoding, "H264") || !g_ascii_strcasecmp(encoding, "H265"));
}

/*
 * 部分设备在 RTP-Info 中错误声明 seq=1，而实际 RTP 序号持续递增。
 * 当序号位于 16 位范围的后半段时，抖动缓冲器可能把全部包判为旧包。
 * 显式启用兼容选项后仅移除 CAPS 中的错误起点，让缓冲器从实际包学习。
 * 不改变包序号、包顺序或时钟信息，替换事件时保留原事件序号。
 */
static GstPadProbeReturn remove_false_seqnum_base(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    guint i;
    GstEvent *event;
    GstCaps *caps;
    gboolean found;
    GstCaps *fixed;
    GstEvent *replacement;

    (void)pad;
    (void)data;
    event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS)
    {
        return GST_PAD_PROBE_OK;
    }
    gst_event_parse_caps(event, &caps);
    found = FALSE;
    for (i = 0; i < gst_caps_get_size(caps); i++)
    {
        found |= gst_structure_has_field(gst_caps_get_structure(caps, i), "seqnum-base");
    }
    if (!found)
    {
        return GST_PAD_PROBE_OK;
    }
    fixed = gst_caps_copy(caps);
    for (i = 0; i < gst_caps_get_size(fixed); i++)
    {
        gst_structure_remove_field(gst_caps_get_structure(fixed, i), "seqnum-base");
    }
    replacement = gst_event_new_caps(fixed);
    gst_caps_unref(fixed);
    gst_event_set_seqnum(replacement, gst_event_get_seqnum(event));
    GST_PAD_PROBE_INFO_DATA(info) = replacement;
    gst_event_unref(event);
    return GST_PAD_PROBE_OK;
}

/* 在新建抖动缓冲器的入口挂事件探针，使修正发生在序号判断之前。 */
static void new_jitterbuffer(GstElement *manager, GstElement *jitterbuffer, guint session,
                             guint ssrc, gpointer data)
{
    IPC_HANDLE *handle;
    GstPad *pad;

    (void)manager;
    (void)session;
    (void)ssrc;
    handle = data;
    pad = gst_element_get_static_pad(jitterbuffer, "sink");
    if (handle->ignore_seqnum_base)
    {
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, remove_false_seqnum_base, NULL,
                          NULL);
    }
    gst_object_unref(pad);
}

/* RTSP 内部 RTP 管理器创建后，继续监听其抖动缓冲器创建事件。 */
static void new_manager(GstElement *source, GstElement *manager, gpointer data)
{
    (void)source;
    g_signal_connect(manager, "new-jitterbuffer", G_CALLBACK(new_jitterbuffer), data);
}

/* 在网络会话描述到达时检查编码能力；不支持的视频及时报告错误。 */
static void on_sdp(GstElement *source, GstSDPMessage *sdp, gpointer data)
{
    guint i;
    guint j;
    gboolean supported;
    const GstSDPMedia *media;
    const GstSDPAttribute *attribute;
    unsigned payload_type;
    char name[32];

    (void)source;
    supported = FALSE;
    for (i = 0; i < gst_sdp_message_medias_len(sdp); ++i)
    {
        media = gst_sdp_message_get_media(sdp, i);
        if (g_strcmp0(gst_sdp_media_get_media(media), "video"))
        {
            continue;
        }
        for (j = 0; j < gst_sdp_media_attributes_len(media); ++j)
        {
            attribute = gst_sdp_media_get_attribute(media, j);
            if (!g_strcmp0(attribute->key, "rtpmap") && attribute->value
                && sscanf(attribute->value, "%u %31[^/]", &payload_type, name) == 2
                && (!g_ascii_strcasecmp(name, "H264") || !g_ascii_strcasecmp(name, "H265")))
            {
                supported = TRUE;
            }
        }
    }
    if (!supported)
    {
        fail(data, IPC_ERR_CODEC);
    }
}

/* 只选择第一个受支持的视频流，跳过音频和其他流；重复选择保持幂等。 */
static gboolean select_stream(GstElement *source, guint num, GstCaps *caps, gpointer data)
{
    IPC_HANDLE *handle;

    (void)source;
    handle = data;
    if (!supported_caps(caps) || num >= INT_MAX)
    {
        return FALSE;
    }
    return g_atomic_int_compare_and_exchange(&handle->selected, 0, (gint)num + 1)
           || g_atomic_int_get(&handle->selected) == (gint)num + 1;
}

/* 根据 RTP 能力选择 H.264/H.265 解包器与解析器，再连接已创建的 MPP。
 * linked 以原子方式抢占，确保多次 pad-added 回调不会重复创建解码支路。 */
static void source_pad_added(GstElement *source, GstPad *pad, gpointer data)
{
    IPC_HANDLE *handle;
    GstCaps *caps;
    const gchar *encoding;
    gboolean h265;
    GstElement *depay;
    GstElement *parser;
    GstCaps *au;
    gboolean linked;
    GstPad *sink_pad;

    (void)source;
    handle = data;
    caps = gst_pad_get_current_caps(pad);
    if (!caps)
    {
        caps = gst_pad_query_caps(pad, NULL);
    }
    if (!supported_caps(caps) || !g_atomic_int_compare_and_exchange(&handle->linked, 0, 1))
    {
        if (caps)
        {
            gst_caps_unref(caps);
        }
        return;
    }
    encoding = gst_structure_get_string(gst_caps_get_structure(caps, 0), "encoding-name");
    h265 = !g_ascii_strcasecmp(encoding, "H265");
    g_atomic_int_set(&handle->codec, h265 ? IPC_CODEC_H265 : IPC_CODEC_H264);
    depay = gst_element_factory_make(h265 ? "rtph265depay" : "rtph264depay", "depay");
    parser = gst_element_factory_make(h265 ? "h265parse" : "h264parse", "parser");
    gst_caps_unref(caps);
    if (!depay || !parser)
    {
        if (depay)
        {
            gst_object_unref(depay);
        }
        if (parser)
        {
            gst_object_unref(parser);
        }
        fail(handle, IPC_ERR_PLUGIN);
        return;
    }

    /*
     * 两个新元素加入 bin 后由管线接管引用。
     * 解析器输出按访问单元（AU）对齐，为 MPP 提供完整压缩帧边界。
     * 动态加入的元素要同步父状态，才会随已经运行的管线开始工作。
     */
    gst_bin_add_many(GST_BIN(handle->pipeline), depay, parser, NULL);
    au = gst_caps_from_string(h265 ? "video/x-h265,alignment=au" : "video/x-h264,alignment=au");
    linked =
        gst_element_link(depay, parser) && gst_element_link_filtered(parser, handle->decoder, au);
    gst_caps_unref(au);
    if (!linked || !gst_element_sync_state_with_parent(parser)
        || !gst_element_sync_state_with_parent(depay))
    {
        fail(handle, IPC_ERR_PIPELINE);
        return;
    }
    sink_pad = gst_element_get_static_pad(depay, "sink");
    if (gst_pad_link(pad, sink_pad) != GST_PAD_LINK_OK)
    {
        fail(handle, IPC_ERR_PIPELINE);
    }
    gst_object_unref(sink_pad);
}

/* 先停止管线，等待其内部任务退出，再移除总线回调并归还引用。
 * 帧持有独立的样本或内存，不属于句柄的销毁范围。 */
void ipc_close(IPC_HANDLE *handle)
{
    if (!handle)
    {
        return;
    }
    if (handle->pipeline)
    {
        gst_element_set_state(handle->pipeline, GST_STATE_NULL);
    }
    if (handle->bus)
    {
        gst_bus_set_sync_handler(handle->bus, NULL, NULL, NULL);
        gst_object_unref(handle->bus);
    }
    if (handle->pipeline)
    {
        gst_object_unref(handle->pipeline);
    }
    g_free(handle);
}

/* 创建元素并移交给管线；成功返回借用指针，失败自行归还创建的引用。 */
static GstElement *add_element(IPC_HANDLE *handle, const char *factory, const char *name)
{
    GstElement *element;

    element = gst_element_factory_make(factory, name);
    if (element && !gst_bin_add(GST_BIN(handle->pipeline), element))
    {
        gst_object_unref(element);
        return NULL;
    }
    return element;
}

/* 不同 Rockchip 插件版本属性不完全一致，只设置存在且可写的布尔属性。 */
static void optional_bool(GstElement *element, const char *name, gboolean value)
{
    GParamSpec *property;

    property = g_object_class_find_property(G_OBJECT_GET_CLASS(element), name);
    if (property && G_IS_PARAM_SPEC_BOOLEAN(property) && (property->flags & G_PARAM_WRITABLE))
    {
        g_object_set(element, name, value, NULL);
    }
}

/* 通过插件的枚举属性查询格式能力；set 为 TRUE 时同时选择该输出格式。 */
static gboolean mpp_format(GstElement *element, const char *name, gboolean set)
{
    GParamSpec *property;
    GEnumClass *enum_class;
    GEnumValue *value;
    gboolean found;

    property = g_object_class_find_property(G_OBJECT_GET_CLASS(element), "format");
    if (!property || !G_IS_PARAM_SPEC_ENUM(property) || !(property->flags & G_PARAM_WRITABLE))
    {
        return !strcmp(name, "NV12"); /* 未提供格式属性时，只承认解码器原生 NV12 输出。 */
    }
    enum_class = g_type_class_ref(G_PARAM_SPEC_VALUE_TYPE(property));
    value = g_enum_get_value_by_nick(enum_class, name);
    found = value != NULL;
    if (value && set)
    {
        g_object_set(element, "format", value->value, NULL);
    }
    g_type_class_unref(enum_class);
    return found;
}

/* 验证完整 URL，或用 IP、端口和路径构造 URL；返回新分配的字符串。
 * 失败返回 NULL；调用者须 g_free()。认证信息必须通过独立属性设置。 */
static gchar *make_url(const IPC_CONFIG *config)
{
    gchar *url;
    GInetAddress *address;
    gboolean v6;
    const char *path;
    GstRTSPUrl *parsed;
    gboolean valid;
    const unsigned char *character;

    url = NULL;
    if (config->url)
    {
        if (strlen(config->url) > IPC_MAX_URL_LENGTH)
        {
            return NULL;
        }
        url = g_strdup(config->url);
    }
    else
    {
        address = config->ip ? g_inet_address_new_from_string(config->ip) : NULL;
        if (!address)
        {
            return NULL;
        }
        v6 = g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV6;
        g_object_unref(address);
        path = config->path ? config->path : "/stream1";
        if (config->port > IPC_MAX_RTSP_PORT || path[0] != '/'
            || strlen(path) > IPC_MAX_PATH_LENGTH)
        {
            return NULL;
        }
        url = v6 ? g_strdup_printf("rtsp://[%s]:%u%s", config->ip,
                                   config->port ? config->port : IPC_DEFAULT_RTSP_PORT, path)
                 : g_strdup_printf("rtsp://%s:%u%s", config->ip,
                                   config->port ? config->port : IPC_DEFAULT_RTSP_PORT, path);
    }
    parsed = NULL;
    valid = (!g_ascii_strncasecmp(url, "rtsp://", 7) || !g_ascii_strncasecmp(url, "rtsps://", 8))
            && gst_rtsp_url_parse(url, &parsed) == GST_RTSP_OK && parsed && parsed->host
            && *parsed->host && !parsed->user && !parsed->passwd;
    for (character = (const unsigned char *)url; *character; ++character)
    {
        if (*character <= 32 || *character == 127 || *character == '#')
        {
            valid = FALSE;
        }
    }
    if (parsed)
    {
        gst_rtsp_url_free(parsed);
    }
    if (!valid)
    {
        g_clear_pointer(&url, g_free);
    }
    return url;
}

/* 依次校验配置、构建硬解码管线、注册回调并启动异步连接。
 * 部分初始化失败统一走 bad 出口，避免泄漏已创建的 GStreamer 元素。 */
IPC_HANDLE *ipc_open(const IPC_CONFIG *config, char *error, unsigned error_size)
{
    IPC_ERROR_CODE reason;
    IPC_HANDLE *handle;
    gchar *url;
    GstElement *filter;
    const char *format_name;
    gboolean direct;
    gboolean cpu;
    GstCaps *caps;
    GstElement *native;
    GstElement *convert;

    reason = IPC_ERR_ARGUMENT;
    handle = NULL;
    url = NULL;
    if (!config || config->format < 0 || config->format >= IPC_FORMAT_COUNT
        || config->conversion < IPC_CONVERT_AUTO || config->conversion > IPC_CONVERT_CPU
        || config->udp < IPC_TRANSPORT_TCP || config->udp > IPC_TRANSPORT_AUTO
        || config->queue_frames > IPC_MAX_QUEUE_FRAMES || config->latency_ms > IPC_MAX_LATENCY_MS
        || config->network_timeout_ms > IPC_MAX_NETWORK_TIMEOUT_MS)
    {
        goto bad;
    }
    url = make_url(config);
    if (!url)
    {
        goto bad;
    }
    if (!gst_init_check(NULL, NULL, NULL))
    {
        reason = IPC_ERR_INIT;
        goto bad;
    }
    handle = g_try_new0(IPC_HANDLE, 1);
    if (!handle)
    {
        reason = IPC_ERR_MEMORY;
        goto bad;
    }
    handle->format = config->format;
    handle->ignore_seqnum_base = config->ignore_seqnum_base;
    handle->pipeline = gst_pipeline_new(NULL);
    reason = IPC_ERR_PLUGIN;
    if (!handle->pipeline)
    {
        goto bad;
    }
    handle->source = add_element(handle, "rtspsrc", "source");
    handle->decoder = add_element(handle, "mppvideodec", "hardware_decoder");
    handle->sink = add_element(handle, "appsink", "frames");
    filter = add_element(handle, "capsfilter", "output_format");
    if (!handle->source || !handle->decoder || !handle->sink || !filter)
    {
        goto bad;
    }
    format_name = ipc_format_name(config->format);

    /*
     * 目标固件虽声明支持 I420，但实测 RGA 平面输出失败。
     * 固定以 NV12 硬解结果拆分 U/V；GRAY8 直接借用原始 Y 平面。
     * 两种布局均绕过厂商 videoconvert，不影响视频硬解码路径。
     */
    handle->layout_cpu = config->format == IPC_I420 || config->format == IPC_GRAY8;
    direct = !handle->layout_cpu && mpp_format(handle->decoder, format_name, FALSE);
    if (config->conversion == IPC_CONVERT_MPP_ONLY && !direct)
    {
        reason = IPC_ERR_FORMAT;
        goto bad;
    }
    cpu = config->format != IPC_NV12 && (config->conversion == IPC_CONVERT_CPU || !direct);
    handle->conversion = cpu ? IPC_CONVERT_CPU : IPC_CONVERT_MPP_ONLY;

    /*
     * 认证使用独立属性，避免账号密码出现在 URL 中。
     * rtspsrc 的 tcp-timeout 单位为微秒，配置字段单位为毫秒。
     * teardown-timeout 置 0，不额外等待远端确认 TEARDOWN。
     */
    g_object_set(handle->source, "location", url, "latency", config->latency_ms, "protocols",
                 transport_protocols(config->udp), "drop-on-latency", TRUE, "tcp-timeout",
                 (guint64)(config->network_timeout_ms ? config->network_timeout_ms
                                                      : IPC_DEFAULT_NETWORK_TIMEOUT_MS)
                     * 1000,
                 "teardown-timeout", (guint64)0, NULL);
    if (config->udp == IPC_TRANSPORT_AUTO)
    {
        /* 单位为微秒；仅用于初始 UDP 接收失败时的 TCP 回退。 */
        g_object_set(handle->source, "timeout", (guint64)IPC_AUTO_UDP_TIMEOUT_US, NULL);
    }
    if (config->username)
    {
        g_object_set(handle->source, "user-id", config->username, NULL);
    }
    if (config->password)
    {
        g_object_set(handle->source, "user-pw", config->password, NULL);
    }

    /*
     * 客户需要 CPU 可读的线性像素，因此关闭压缩布局及 DMA 特征协商。
     * 插件报告损坏帧时不继续向客户交付；可选属性不存在时保持兼容。
     */
    optional_bool(handle->decoder, "fbc", FALSE);
    optional_bool(handle->decoder, "arm-afbc", FALSE);
    optional_bool(handle->decoder, "dma-feature", FALSE);
    optional_bool(handle->decoder, "ignore-error", FALSE);
    optional_bool(handle->decoder, "discard-corrupted-frames", TRUE);
    if (!mpp_format(handle->decoder, cpu ? "NV12" : format_name, TRUE))
    {
        reason = IPC_ERR_FORMAT;
        goto bad;
    }
    caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
                               handle->layout_cpu ? "NV12" : format_name, NULL);
    g_object_set(filter, "caps", caps, NULL);
    gst_caps_unref(caps);

    /*
     * 不按显示时钟等待帧，队列满时丢弃旧帧，限制业务处理慢时的积压。
     * 禁用 last-sample 引用，避免 appsink 额外长期占用一个解码缓冲区。
     * EOS 时不等待业务主动排空队列，保证关闭流程可以结束。
     */
    g_object_set(handle->sink, "sync", FALSE, "max-buffers",
                 config->queue_frames ? config->queue_frames : (unsigned)IPC_DEFAULT_QUEUE_FRAMES,
                 "drop", TRUE, "enable-last-sample", FALSE, "wait-on-eos", FALSE, NULL);

    /*
     * 普通 CPU 颜色转换链路固定为 MPP -> NV12 -> videoconvert -> 目标格式。
     * I420/GRAY8 在 map_sample() 处理布局，此处仍让 appsink 接收 NV12。
     * MPP 路径只连接格式约束和 appsink，不建立软件视频解码器。
     */
    if (cpu && !handle->layout_cpu)
    {
        native = add_element(handle, "capsfilter", "native_nv12");
        convert = add_element(handle, "videoconvert", "pixel_conversion");
        if (!native || !convert)
        {
            goto bad;
        }
        caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV12", NULL);
        g_object_set(native, "caps", caps, NULL);
        gst_caps_unref(caps);
        reason = IPC_ERR_FORMAT;
        if (!gst_element_link_many(handle->decoder, native, convert, filter, handle->sink, NULL))
        {
            goto bad;
        }
    }
    else
    {
        reason = IPC_ERR_FORMAT;
        if (!gst_element_link_many(handle->decoder, filter, handle->sink, NULL))
        {
            goto bad;
        }
    }

    /*
     * RTSP 视频 pad 在会话协商后才出现，由回调补齐前端解包/解析支路。
     * 总线回调必须先于 PLAYING 安装，避免遗漏异步启动阶段的错误。
     */
    g_signal_connect(handle->source, "on-sdp", G_CALLBACK(on_sdp), handle);
    g_signal_connect(handle->source, "select-stream", G_CALLBACK(select_stream), handle);
    g_signal_connect(handle->source, "pad-added", G_CALLBACK(source_pad_added), handle);
    if (config->ignore_seqnum_base)
    {
        g_signal_connect(handle->source, "new-manager", G_CALLBACK(new_manager), handle);
    }
    handle->bus = gst_element_get_bus(handle->pipeline);
    gst_bus_set_sync_handler(handle->bus, bus_message, handle, NULL);
    if (gst_element_set_state(handle->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
        reason = ipc_last_error_code(handle);
        if (!reason)
        {
            reason = IPC_ERR_PIPELINE;
        }
        goto bad;
    }
    g_free(url);
    if (error && error_size)
    {
        error[0] = 0;
    }
    return handle;
bad:
    g_free(url);
    if (error && error_size)
    {
        snprintf(error, error_size, "%s", error_text(reason));
    }
    ipc_close(handle);
    return NULL;
}

/* 计算有效平面大小，不包含硬件行对齐；YUV420 的色度尺寸向上取整。
 * 宽度先限制到 INT_MAX / 4，保证后续四字节像素及 stride 运算安全。 */
static gboolean frame_layout(IPC_FRAME *frame)
{
    unsigned width;
    unsigned height;

    width = frame->width;
    height = frame->height;
    if (!width || !height || width > INT_MAX / 4 || height > INT_MAX)
    {
        return FALSE;
    }
    frame->row_bytes[0] = width;
    frame->rows[0] = height;
    switch (frame->format)
    {
    case IPC_NV12:
    case IPC_NV21:
        frame->planes = 2;
        frame->row_bytes[1] = ((width + 1) / 2) * 2;
        frame->rows[1] = (height + 1) / 2;
        break;
    case IPC_I420:
        frame->planes = 3;
        frame->row_bytes[1] = frame->row_bytes[2] = (width + 1) / 2;
        frame->rows[1] = frame->rows[2] = (height + 1) / 2;
        break;
    case IPC_RGB24:
    case IPC_BGR24:
        frame->planes = 1;
        frame->row_bytes[0] = width * 3;
        break;
    case IPC_RGBA:
    case IPC_BGRA:
        frame->planes = 1;
        frame->row_bytes[0] = width * 4;
        break;
    case IPC_GRAY8:
        frame->planes = 1;
        break;
    default:
        return FALSE;
    }
    return TRUE;
}

/* 先生成规范布局进行逐项比对，再检查行跨度、指针偏移和累加溢出。
 * 所有平面通过校验后才返回长度，供复制函数在写入之前判断容量。 */
size_t ipc_frame_size(const IPC_FRAME *frame)
{
    unsigned plane;
    IPC_FRAME expected;
    size_t bytes;
    uint64_t stride;

    if (!frame)
    {
        return 0;
    }
    memset(&expected, 0, sizeof(expected));
    expected.width = frame->width;
    expected.height = frame->height;
    expected.format = frame->format;
    if (!frame_layout(&expected) || frame->planes != expected.planes)
    {
        return 0;
    }
    bytes = 0;
    for (plane = 0; plane < frame->planes; ++plane)
    {
        stride = frame->stride[plane] < 0 ? -(int64_t)frame->stride[plane] : frame->stride[plane];
        if (!frame->data[plane] || frame->rows[plane] != expected.rows[plane]
            || frame->row_bytes[plane] != expected.row_bytes[plane]
            || stride < frame->row_bytes[plane] || stride * (frame->rows[plane] - 1) > PTRDIFF_MAX
            || (uint64_t)frame->row_bytes[plane] * frame->rows[plane] > SIZE_MAX - bytes)
        {
            return 0;
        }
        bytes += (size_t)frame->row_bytes[plane] * frame->rows[plane];
    }
    return bytes;
}

/* 逐平面、逐行复制有效字节，支持带填充或负 stride 的只读输入帧。 */
IPC_STATUS ipc_frame_copy(const IPC_FRAME *frame, void *dst, size_t capacity)
{
    unsigned plane;
    unsigned row;
    size_t size;
    uint8_t *out;

    size = ipc_frame_size(frame);
    if (!dst || !size || capacity < size)
    {
        return IPC_ERROR;
    }
    out = dst;
    for (plane = 0; plane < frame->planes; ++plane)
    {
        for (row = 0; row < frame->rows[plane]; ++row)
        {
            memcpy(out, frame->data[plane] + (ptrdiff_t)row * frame->stride[plane],
                   frame->row_bytes[plane]);
            out += frame->row_bytes[plane];
        }
    }
    return IPC_OK;
}

/* 映射依赖样本引用，须先解除映射，再归还样本；最后释放自有内存。
 * 将公开结构体清零后，重复释放同一个帧不会再次访问旧资源。 */
void ipc_frame_release(IPC_FRAME *frame)
{
    IPC_FRAME_OWNER *owner;

    if (!frame)
    {
        return;
    }
    owner = frame->private_data;
    if (owner)
    {
        if (owner->sample)
        {
            gst_video_frame_unmap(&owner->mapped);
            gst_sample_unref(owner->sample);
        }
        g_free(owner->allocation);
        g_free(owner);
    }
    memset(frame, 0, sizeof(*frame));
}

/* 接管 appsink 返回的样本引用，校验格式后映射为只读 CPU 地址。
 * 成功时把资源交给 frame；所有失败分支均在本层释放样本及临时分配。 */
static IPC_STATUS map_sample(IPC_HANDLE *handle, GstSample *sample, uint64_t ready,
                             IPC_FRAME *frame)
{
    unsigned plane;
    unsigned row;
    unsigned col;
    unsigned i;
    GstBuffer *buffer;
    GstVideoInfo info;
    IPC_FRAME_OWNER *owner;
    size_t y_bytes;
    size_t chroma_bytes;
    const uint8_t *y_data;
    const uint8_t *uv_data;
    int y_stride;
    int uv_stride;
    size_t offset;

    /* 样本拥有 buffer/caps，这里取得借用指针；sample 引用持续到映射解除。 */
    buffer = gst_sample_get_buffer(sample);
    gst_video_info_init(&info);
    owner = g_try_new0(IPC_FRAME_OWNER, 1);
    if (!owner)
    {
        gst_sample_unref(sample);
        fail(handle, IPC_ERR_MEMORY);
        return IPC_ERROR;
    }
    if (!buffer || !gst_video_info_from_caps(&info, gst_sample_get_caps(sample))
        || GST_VIDEO_INFO_FORMAT(&info)
               != (handle->layout_cpu
                       ? GST_VIDEO_FORMAT_NV12
                       : gst_video_format_from_string(ipc_format_name(handle->format)))
        || !gst_video_frame_map(&owner->mapped, &info, buffer, GST_MAP_READ))
    {
        g_free(owner);
        gst_sample_unref(sample);
        fail(handle, IPC_ERR_MAP);
        return IPC_ERROR;
    }
    owner->sample = sample;
    memset(frame, 0, sizeof(*frame));
    frame->width = GST_VIDEO_INFO_WIDTH(&info);
    frame->height = GST_VIDEO_INFO_HEIGHT(&info);
    frame->format = handle->format;
    frame->private_data = owner;
    if (!frame_layout(frame)
        || (handle->layout_cpu ? 2u : frame->planes) != GST_VIDEO_FRAME_N_PLANES(&owner->mapped))
    {
        goto bad;
    }

    /* 媒体 PTS 可能无效或超出有符号范围；用 -1 明确表示不能使用。 */
    frame->pts_ns = GST_BUFFER_PTS_IS_VALID(buffer) && GST_BUFFER_PTS(buffer) <= INT64_MAX
                        ? (int64_t)GST_BUFFER_PTS(buffer)
                        : -1;
    frame->ready_monotonic_ns = ready;
    frame->codec = ipc_get_codec(handle);
    frame->conversion = handle->conversion;

    /*
     * NV12 色度是 U/V 交错数据；I420 需要分别保存 U、V 平面。
     * 自有缓冲区按 Y/U/V 顺序连续分配，逐行去掉原硬件 stride 填充。
     * 复制完成后立即归还 MPP 样本，客户持有的是独立像素内存。
     */
    if (handle->layout_cpu && handle->format == IPC_I420)
    {
        y_bytes = (size_t)frame->width * frame->height;
        chroma_bytes = (size_t)frame->row_bytes[1] * frame->rows[1];
        if (chroma_bytes > (SIZE_MAX - y_bytes) / 2)
        {
            goto bad;
        }
        owner->allocation = g_try_malloc(y_bytes + 2 * chroma_bytes);
        if (!owner->allocation)
        {
            ipc_frame_release(frame);
            fail(handle, IPC_ERR_MEMORY);
            return IPC_ERROR;
        }
        y_data = GST_VIDEO_FRAME_PLANE_DATA(&owner->mapped, 0);
        uv_data = GST_VIDEO_FRAME_PLANE_DATA(&owner->mapped, 1);
        y_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&owner->mapped, 0);
        uv_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&owner->mapped, 1);
        if (!y_data || !uv_data || y_stride < (int)frame->width
            || uv_stride < (int)(frame->row_bytes[1] * 2))
        {
            goto bad;
        }
        frame->data[0] = owner->allocation;
        frame->data[1] = owner->allocation + y_bytes;
        frame->data[2] = owner->allocation + y_bytes + chroma_bytes;
        for (plane = 0; plane < 3; ++plane)
        {
            frame->stride[plane] = (int)frame->row_bytes[plane];
        }
        for (row = 0; row < frame->height; ++row)
        {
            memcpy(owner->allocation + (size_t)row * frame->width,
                   y_data + (ptrdiff_t)row * y_stride, frame->width);
        }

        /* 每个交错色度位置拆成一对 U/V；色度尺寸已在 frame_layout() 向上取整。 */
        for (row = 0; row < frame->rows[1]; ++row)
        {
            for (col = 0; col < frame->row_bytes[1]; ++col)
            {
                offset = (size_t)row * frame->row_bytes[1] + col;
                owner->allocation[y_bytes + offset] = uv_data[(ptrdiff_t)row * uv_stride + 2 * col];
                owner->allocation[y_bytes + chroma_bytes + offset] =
                    uv_data[(ptrdiff_t)row * uv_stride + 2 * col + 1];
            }
        }
        gst_video_frame_unmap(&owner->mapped);
        gst_sample_unref(owner->sample);
        owner->sample = NULL;
    }
    else
    {
        for (i = 0; i < frame->planes; ++i)
        {
            frame->data[i] = GST_VIDEO_FRAME_PLANE_DATA(&owner->mapped, i);
            frame->stride[i] = GST_VIDEO_FRAME_PLANE_STRIDE(&owner->mapped, i);
        }
    }
    if (!ipc_frame_size(frame))
    {
        goto bad;
    }
    return IPC_OK;
bad:
    ipc_frame_release(frame);
    fail(handle, IPC_ERR_MAP);
    return IPC_ERROR;
}

/* 用短片段等待样本，定期检查异步致命错误，并跳过已标记损坏的帧。
 * 超时不关闭连接；成功返回时由调用者负责释放 frame。 */
IPC_STATUS ipc_read(IPC_HANDLE *handle, unsigned timeout_ms, IPC_FRAME *frame)
{
    uint64_t deadline;
    uint64_t now;
    uint64_t wait;
    GstSample *sample;
    uint64_t ready;
    GstBuffer *buffer;

    if (!handle)
    {
        return IPC_ERROR;
    }
    handle->argument_error = IPC_ERR_NONE;
    if (!frame || frame->private_data)
    {
        handle->argument_error = IPC_ERR_ARGUMENT;
        return IPC_ERROR;
    }
    memset(frame, 0, sizeof(*frame));

    /* 截止时间使用单调时钟；乘法先转为 64 位，避免毫秒参数换算时溢出。 */
    deadline = monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    for (;;)
    {
        if (g_atomic_int_get(&handle->failed))
        {
            return IPC_ERROR;
        }
        now = monotonic_ns();
        wait = now < deadline ? deadline - now : 0;
        if (wait > IPC_READ_SLICE_NS)
        {
            wait = IPC_READ_SLICE_NS;
        }
        /* appsink 在队列无帧时使用条件变量等待；帧到达立即唤醒，
         * 这里的 20 ms 分段不是固定延迟，也不是无休止检查 socket。 */
        sample = gst_app_sink_try_pull_sample(GST_APP_SINK(handle->sink), wait);
        ready = monotonic_ns();
        if (g_atomic_int_get(&handle->failed))
        {
            if (sample)
            {
                gst_sample_unref(sample);
            }
            return IPC_ERROR;
        }
        if (sample)
        {
            buffer = gst_sample_get_buffer(sample);
            if (buffer && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED))
            {
                gst_sample_unref(sample);
                if (ready >= deadline)
                {
                    return IPC_TIMEOUT;
                }
                continue;
            }
            return map_sample(handle, sample, ready, frame);
        }

        /* appsink 启动前也可能报告 is_eos；只认总线真正收到的 EOS，
         * 避免将仍在连接或认证的 RTSP 会话误判为流结束。 */
        if (g_atomic_int_get(&handle->eos))
        {
            return IPC_EOS;
        }
        if (ready >= deadline)
        {
            return IPC_TIMEOUT;
        }

        /* 尚未启动的 appsink 会立即返回，短暂让出 CPU，避免连接期间忙轮询。 */
        if (ready - now < 1000000ULL)
        {
            g_usleep((gulong)MIN((deadline - ready) / 1000, 1000));
        }
    }
}
