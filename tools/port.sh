#!/bin/bash
# port.sh - SpiritFoxOS 第三方库构建工具
# 用法: ./port.sh build libdrm
#       ./port.sh install libdrm

PORT_NAME=$1
PORT_DIR="userland/ports/$PORT_NAME"
SYSROOT="$PWD/sysroot"
PREFIX="/usr"

# 交叉编译环境变量
export CC="x86_64-spiritfoxos-gcc"
export CXX="x86_64-spiritfoxos-g++"
export AR="x86_64-spiritfoxos-ar"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"
export CFLAGS="--sysroot=$SYSROOT -I$SYSROOT/usr/include"
export LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/usr/lib"

case "$2" in
    build)
        cd "$PORT_DIR"
        ./configure --host=x86_64-spiritfoxos --prefix=$PREFIX
        make -j$(nproc)
        ;;
    install)
        cd "$PORT_DIR"
        make DESTDIR="$SYSROOT" install
        ;;
    clean)
        cd "$PORT_DIR"
        make clean
        ;;
    *)
        echo "Usage: $0 {build|install|clean} port_name"
        exit 1
        ;;
esac
