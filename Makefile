CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BUILD_DIR ?= build
SYSROOT ?=
# 只需指定 CROSS_COMPILE 前缀或 ARM64 CC 路径，自动查询工具链开发环境。
IPC_CROSS_BUILD :=
ifneq ($(strip $(CROSS_COMPILE)),)
IPC_CROSS_BUILD := 1
ifneq ($(origin CC),command line)
CC := $(CROSS_COMPILE)gcc
endif
else
IPC_COMPILER_TARGET := $(shell $(CC) -dumpmachine 2>/dev/null)
ifneq ($(filter aarch64% arm64%,$(IPC_COMPILER_TARGET)),)
ifeq ($(filter aarch64 arm64,$(shell uname -m)),)
IPC_CROSS_BUILD := 1
endif
endif
endif
ifneq ($(IPC_CROSS_BUILD),)
ifeq ($(strip $(SYSROOT)),)
SYSROOT := $(shell sh tools/find_sysroot.sh "$(CC)")
endif
ifeq ($(strip $(SYSROOT)),)
$(error Cannot locate target development files in the selected toolchain)
endif
endif
PKG_CONFIG_COMMAND = $(PKG_CONFIG)
ifneq ($(strip $(SYSROOT)),)
PKG_CONFIG_COMMAND = env PKG_CONFIG_PATH= PKG_CONFIG_FDO_SYSROOT_RULES=1 PKG_CONFIG_SYSROOT_DIR="$(SYSROOT)" PKG_CONFIG_LIBDIR="$(SYSROOT)/usr/lib/aarch64-linux-gnu/pkgconfig:$(SYSROOT)/usr/lib/pkgconfig:$(SYSROOT)/usr/lib64/pkgconfig:$(SYSROOT)/usr/share/pkgconfig" $(PKG_CONFIG)
LDFLAGS += -Wl,-rpath-link,"$(SYSROOT)/usr/lib/aarch64-linux-gnu:$(SYSROOT)/usr/lib:$(SYSROOT)/usr/lib64"
CFLAGS += --sysroot="$(SYSROOT)"
endif
PACKAGES = gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-rtsp-1.0 gstreamer-sdp-1.0 gio-2.0
CPPFLAGS += -Iinclude $(shell $(PKG_CONFIG_COMMAND) --cflags $(PACKAGES))
CFLAGS += -O2 -Wall -Wextra -Werror -Wdeclaration-after-statement -std=c11 -fPIC
LDLIBS += $(shell $(PKG_CONFIG_COMMAND) --libs $(PACKAGES))
LIB = $(BUILD_DIR)/libipc_sdk.so.1.0.0
EXAMPLE = $(BUILD_DIR)/read_frames
all: $(LIB) $(EXAMPLE)
$(BUILD_DIR):
	mkdir -p $@
$(LIB): src/ipc_sdk.c include/ipc_sdk.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -shared -Wl,-z,defs -Wl,-soname,libipc_sdk.so.1 -o $@ $< $(LDLIBS)
	ln -sfn libipc_sdk.so.1.0.0 $(BUILD_DIR)/libipc_sdk.so.1
	ln -sfn libipc_sdk.so.1 $(BUILD_DIR)/libipc_sdk.so
$(EXAMPLE): examples/read_frames.c include/ipc_sdk.h $(LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< -L$(BUILD_DIR) -lipc_sdk -lpthread $(LDLIBS)
	ln -sfn $(EXAMPLE) read_frames
install: all
	install -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	install -m 644 include/ipc_sdk.h $(DESTDIR)$(PREFIX)/include/
	install -m 755 $(LIB) $(DESTDIR)$(PREFIX)/lib/
	ln -sfn libipc_sdk.so.1.0.0 $(DESTDIR)$(PREFIX)/lib/libipc_sdk.so.1
	ln -sfn libipc_sdk.so.1 $(DESTDIR)$(PREFIX)/lib/libipc_sdk.so
clean:
	rm -f $(BUILD_DIR)/libipc_sdk.so*
.PHONY: all install clean
