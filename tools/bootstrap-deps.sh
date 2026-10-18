#!/usr/bin/env bash
# Build target libraries against musl, never against the build host's glibc.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export SPIRITFOX_SYSROOT=${SPIRITFOX_SYSROOT:-"$root/build/sysroot"}
cache=${SPIRITFOX_DOWNLOAD_CACHE:-"$root/build/downloads"}
work="$root/build/deps"
host_tools="$(dirname -- "$SPIRITFOX_SYSROOT")/host-tools"
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
mkdir -p "$cache" "$work" "$SPIRITFOX_SYSROOT/usr/include"
for command in gcc make curl tar sha256sum meson ninja pkg-config wayland-scanner; do
    command -v "$command" >/dev/null || { echo "Missing $command; run bash tools/install-deps.sh" >&2; exit 1; }
done
case $(gcc -dumpmachine) in x86_64*linux*) ;; *) echo 'An x86_64 Linux compiler is required' >&2; exit 1 ;; esac

while read -r name version digest url; do
    [[ -z "$name" || "$name" == \#* ]] && continue
    archive="$cache/${url##*/}"
    if ! [[ -f "$archive" ]] || ! printf '%s  %s\n' "$digest" "$archive" | sha256sum --check --status; then
        curl --fail --location --retry 3 --connect-timeout 20 --output "$archive.part" "$url"
        printf '%s  %s\n' "$digest" "$archive.part" | sha256sum --check
        mv -- "$archive.part" "$archive"
    fi
    stamp="$SPIRITFOX_SYSROOT/.$name-$version.stamp"
    # Include the prefix: musl-gcc.specs contains absolute toolchain paths.
    identity="$digest $SPIRITFOX_SYSROOT $(gcc -dumpmachine) $(gcc -dumpversion)"
    [[ "$name" != libffi ]] || identity+=" gnu11 disable-exec-static-tramp"
    [[ "$name" != wayland ]] || identity+=" native-scanner-$version"
    if [[ -f "$stamp" && "$(cat "$stamp")" == "$identity" ]]; then
        echo "[DEPS] $name $version already built"
        continue
    fi
    tar -xf "$archive" -C "$work"
    source_dir="$work/$name-$version"
    case "$name" in
        musl)
            mkdir -p "$work/musl-build"
            (cd "$work/musl-build"
             "$source_dir/configure" --prefix="$SPIRITFOX_SYSROOT/usr" \
                 --syslibdir="$SPIRITFOX_SYSROOT/lib"
             make -s -j"$jobs"
             make -s install)
            ;;
        libffi)
            mkdir -p "$work/libffi-build"
            (cd "$work/libffi-build"
             CC="$root/tools/x86_64-spiritfoxos-gcc" CFLAGS='-O2 -std=gnu11' "$source_dir/configure" \
                 --host=x86_64-linux-musl --prefix="$SPIRITFOX_SYSROOT/usr" \
                 --libdir="$SPIRITFOX_SYSROOT/usr/lib" --disable-shared \
                 --enable-static --disable-docs --disable-multi-os-directory \
                 --disable-exec-static-tramp
             make -s -j"$jobs"
             make -s install)
            ;;
        wayland)
            # Wayland requires the build-machine scanner to match its version.
            meson setup --wipe "$work/scanner-build" "$source_dir" \
                --prefix="$host_tools" --libdir=lib -Dlibraries=false \
                -Dtests=false -Ddocumentation=false -Ddtd_validation=false
            meson compile -C "$work/scanner-build" -j "$jobs"
            meson install -C "$work/scanner-build"
            export PKG_CONFIG_PATH="$host_tools/lib/pkgconfig:$host_tools/share/pkgconfig"
            cross="$work/wayland-cross.ini"
            cat > "$cross" <<EOF
[binaries]
c = '$root/tools/x86_64-spiritfoxos-gcc'
ar = 'ar'
strip = 'strip'
pkgconfig = 'pkg-config'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$SPIRITFOX_SYSROOT/usr/lib/pkgconfig'
[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
            meson setup --wipe "$work/wayland-build" "$source_dir" \
                --cross-file "$cross" --prefix="$SPIRITFOX_SYSROOT/usr" \
                -Dbuild.pkg_config_path="$host_tools/lib/pkgconfig" \
                --libdir=lib --default-library=static -Dscanner=false \
                -Dtests=false -Ddocumentation=false -Ddtd_validation=false
            meson compile -C "$work/wayland-build" -j "$jobs"
            meson install -C "$work/wayland-build"
            ;;
    esac
    printf '%s\n' "$identity" > "$stamp"
done < "$root/tools/deps.lock"

# These are project device ABI headers, not distribution kernel headers.
mkdir -p "$SPIRITFOX_SYSROOT/usr/include/linux" "$root/wayland/generated"
cp "$root/sysroot/usr/include/linux/"*.h "$SPIRITFOX_SYSROOT/usr/include/linux/"
for protocol in wayland xdg-shell; do
    for side in client server; do
        "$host_tools/bin/wayland-scanner" "$side-header" "$root/wayland/protocol/$protocol.xml" \
            "$root/wayland/generated/$protocol-$side-protocol.h"
    done
done
echo "[DEPS] musl, libffi, Wayland and protocol headers are ready"
