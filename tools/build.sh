#!/bin/bash
# build.sh - SpiritFoxOS main build script
# 用法: ./build.sh [all|kernel|libc|ports|sysroot|clean]
#
# Phase 0: 构建系统基础设施
# 负责设置交叉编译环境、构建内核和 libc、
# 构建第三方库 ports，以及创建 sysroot 目录结构。

set -e

# =========================================
# 脚本所在目录与项目根目录
# =========================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# =========================================
# 加载交叉编译环境变量
# =========================================
source "$SCRIPT_DIR/env.sh"

# =========================================
# 颜色输出
# =========================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
success() { echo -e "${GREEN}[OK]${NC} $1"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
error()   { echo -e "${RED}[ERROR]${NC} $1"; }

# =========================================
# 构建 sysroot 目录结构
# =========================================
create_sysroot() {
    info "Creating sysroot directory structure..."

    # 基础 FHS 目录
    mkdir -p sysroot/bin
    mkdir -p sysroot/sbin
    mkdir -p sysroot/usr/bin
    mkdir -p sysroot/usr/sbin
    mkdir -p sysroot/usr/lib
    mkdir -p sysroot/usr/include
    mkdir -p sysroot/usr/libexec

    # share 目录 (字体、协议、图标、主题、本地化)
    mkdir -p sysroot/usr/share/fonts
    mkdir -p sysroot/usr/share/wayland
    mkdir -p sysroot/usr/share/icons
    mkdir -p sysroot/usr/share/themes
    mkdir -p sysroot/usr/share/locale

    # 配置目录
    mkdir -p sysroot/etc/wayland
    mkdir -p sysroot/etc/fonts

    # 设备目录 (devfs 挂载点)
    mkdir -p sysroot/dev/dri
    mkdir -p sysroot/dev/input

    # 虚拟文件系统挂载点
    mkdir -p sysroot/proc
    mkdir -p sysroot/sys
    mkdir -p sysroot/run
    mkdir -p sysroot/tmp

    # 可变数据目录
    mkdir -p sysroot/var/log
    mkdir -p sysroot/var/lib

    # 用户目录
    mkdir -p sysroot/home
    mkdir -p sysroot/root

    # SpiritFoxOS 特有头文件目录
    mkdir -p sysroot/usr/include/spiritfoxos
    mkdir -p sysroot/usr/include/sys
    mkdir -p sysroot/usr/include/linux

    success "sysroot directory structure created at $SYSROOT"
}

# =========================================
# 构建内核
# =========================================
build_kernel() {
    info "Building kernel..."
    make kernel
    success "Kernel build complete."
}

# =========================================
# 构建 libc
# =========================================
build_libc() {
    info "Building libc..."
    make libc
    success "libc build complete."
}

# =========================================
# 构建第三方库 port
# 用法: build_port <port_name>
# =========================================
build_port() {
    local port_name="$1"
    if [ -z "$port_name" ]; then
        error "No port name specified."
        echo "Usage: $0 ports <port_name> [build|install|clean]"
        exit 1
    fi

    local action="${2:-build}"
    info "Building port: $port_name (action: $action)"
    "$SCRIPT_DIR/port.sh" "$port_name" "$action"
    success "Port '$port_name' $action complete."
}

# =========================================
# 构建所有 ports
# 遍历 userland/ports/ 下的所有 port 目录
# =========================================
build_all_ports() {
    info "Building all ports..."
    local ports_dir="$PROJECT_ROOT/userland/ports"
    if [ ! -d "$ports_dir" ]; then
        warn "Ports directory not found: $ports_dir"
        return 0
    fi

    for port_dir in "$ports_dir"/*/; do
        [ -d "$port_dir" ] || continue
        local port_name="$(basename "$port_dir")"
        # 跳过隐藏目录和 .gitkeep
        [[ "$port_name" == .* ]] && continue
        info "Building port: $port_name"
        build_port "$port_name" build
        build_port "$port_name" install
    done
    success "All ports built."
}

# =========================================
# 清理构建产物
# =========================================
clean_all() {
    info "Cleaning build artifacts..."
    make clean
    success "Clean complete."
}

# =========================================
# 构建全部 (sysroot + kernel + libc)
# =========================================
build_all() {
    create_sysroot
    build_kernel
    build_libc
    success "Full build complete."
}

# =========================================
# 用法说明
# =========================================
usage() {
    echo "SpiritFoxOS Build Script"
    echo ""
    echo "Usage: $0 <command> [options]"
    echo ""
    echo "Commands:"
    echo "  all                 Build everything (sysroot + kernel + libc)"
    echo "  kernel              Build the kernel only"
    echo "  libc                Build libc only"
    echo "  sysroot             Create the sysroot directory structure"
    echo "  ports <name> [cmd]  Build a single port (cmd: build|install|clean)"
    echo "  ports-all           Build and install all ports"
    echo "  clean               Clean build artifacts"
    echo "  help                Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 all"
    echo "  $0 kernel"
    echo "  $0 ports libdrm build"
    echo "  $0 ports libdrm install"
}

# =========================================
# 主入口
# =========================================
case "${1:-}" in
    all)
        build_all
        ;;
    kernel)
        build_kernel
        ;;
    libc)
        build_libc
        ;;
    sysroot)
        create_sysroot
        ;;
    ports)
        build_port "${2:-}" "${3:-build}"
        ;;
    ports-all)
        build_all_ports
        ;;
    clean)
        clean_all
        ;;
    help|--help|-h|"")
        usage
        ;;
    *)
        error "Unknown command: $1"
        usage
        exit 1
        ;;
esac
