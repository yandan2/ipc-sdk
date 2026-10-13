cmake_minimum_required(VERSION 3.16)
project(ipc_sdk VERSION 1.0.0 LANGUAGES C)
include(GNUInstallDirs)

# 板端使用系统库；交叉编译只在客户提供的目标 sysroot 中查询依赖。
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSROOT)
    set(ENV{PKG_CONFIG_PATH} "")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
    # pkgconf 按标准 sysroot 规则处理路径，避免带空格目录被重复加前缀。
    set(ENV{PKG_CONFIG_FDO_SYSROOT_RULES} "1")
    set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/lib64/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
    set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH FALSE)
    add_link_options("-Wl,-rpath-link,${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu:${CMAKE_SYSROOT}/usr/lib:${CMAKE_SYSROOT}/usr/lib64")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0>=1.18 gstreamer-app-1.0 gstreamer-video-1.0
    gstreamer-rtsp-1.0 gstreamer-sdp-1.0 gio-2.0)
add_library(ipc_sdk SHARED src/ipc_sdk.c)
target_include_directories(ipc_sdk PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(ipc_sdk PRIVATE PkgConfig::GST)
target_compile_features(ipc_sdk PRIVATE c_std_11)
target_compile_options(ipc_sdk PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
set_target_properties(ipc_sdk PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1
    BUILD_WITH_INSTALL_RPATH TRUE)
configure_file(cmake/ipc-sdk.pc.in ipc-sdk.pc @ONLY)
install(TARGETS ipc_sdk LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/ipc_sdk.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ipc-sdk.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
