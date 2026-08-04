#!/bin/bash
# SpiritFoxOS cross-compilation environment
export SYSROOT="$PWD/sysroot"
export TARGET=x86_64-spiritfoxos
export PREFIX=/usr
export CC="${TARGET}-gcc"
export CXX="${TARGET}-g++"
export AR="${TARGET}-ar"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"
export CFLAGS="--sysroot=$SYSROOT -I$SYSROOT/usr/include"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/usr/lib"
export PATH="/usr/local/${TARGET}/bin:$PATH"
