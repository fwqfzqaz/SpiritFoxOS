#!/usr/bin/env bash
set -euo pipefail
source /etc/os-release
elevate=()
if (( EUID != 0 )); then elevate=(sudo); fi
case "$ID" in
    ubuntu|debian)
        "${elevate[@]}" apt-get update
        "${elevate[@]}" apt-get install -y --no-install-recommends \
            build-essential nasm binutils gdisk mtools dosfstools \
            qemu-system-x86 ovmf python3 python3-pil meson ninja-build \
            pkg-config libexpat1-dev libwayland-bin git curl xz-utils file gdb
        ;;
    arch)
        # Arch does not support partial upgrades: update the whole system.
        "${elevate[@]}" pacman -Syu --needed base-devel nasm binutils gptfdisk \
            mtools dosfstools qemu-system-x86 edk2-ovmf python python-pillow \
            meson ninja pkgconf expat wayland git curl xz file gdb
        ;;
    *)
        echo "Unsupported automatic installation: $ID; see docs/DEVELOPMENT.md" >&2
        exit 1
        ;;
esac
