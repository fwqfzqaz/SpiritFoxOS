#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
if [[ $# -gt 1 || ( $# -eq 1 && "$1" != --build-only ) ]]; then
    echo 'Usage: bash tools/check-linux.sh [--build-only]' >&2
    exit 2
fi
python3 tools/check-source.py
python3 tools/test-pe-reloc.py
for script in tools/*.sh .githooks/* run_tests.sh; do
    [[ -f "$script" ]] && bash -n "$script"
done
bash tools/bootstrap-deps.sh
make test-modules test-ramdisk test-ata test-fat32 test-vfs-bounds
make -j"${JOBS:-$(getconf _NPROCESSORS_ONLN)}" all libc test-elf musl-test wayland-all build/fat32.img build/disk.img build/ovmf_vars.fd
file build/kernel/kernel.elf build/efi_stub/kernel.efi
readelf -h build/kernel/kernel.elf | grep -q 'Advanced Micro Devices X86-64'
readelf -l build/test_musl_dyn | grep -q '/lib/ld-musl-x86_64.so.1'
if [[ "${1:-}" != --build-only ]]; then
    python3 tools/smoke-test.py
else
    echo 'Linux build checks passed; the caller must run the boot smoke test.'
fi
