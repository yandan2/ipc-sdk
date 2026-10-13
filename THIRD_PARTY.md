# 第三方依赖

| 组件 | 用途 |
| --- | --- |
| GStreamer / GLib / GIO | RTSP 会话、RTP 解包、码流解析和帧队列 |
| Rockchip MPP / mppvideodec | H.264/H.265 硬件解码 |
| Rockchip RGA / videoconvert | 像素格式转换 |

上述组件由目标 Linux 固件及配套开发环境提供。SDK 源码包不分发第三方库。板端编译使用系统开发文件，交叉编译使用客户提供的目标 sysroot；运行时使用固件中的 GStreamer 插件、MPP/RGA 和内核驱动。

对应开源项目：[GStreamer](https://gstreamer.freedesktop.org/)、[GLib](https://gitlab.gnome.org/GNOME/glib)、[Rockchip MPP](https://github.com/rockchip-linux/mpp)、[Rockchip RGA](https://github.com/airockchip/librga)、[Debian 源码包](https://sources.debian.org/)。
