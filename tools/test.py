"""Build, host negative tests, C service tests, and real QEMU integration."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
import build

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--skip-build', action='store_true')
parser.add_argument('--host-only', action='store_true')
args = parser.parse_args()
started = time.time()
if not args.skip_build:
    build.main()
build.run(['-target', 'x86_64-windows-gnu', '-g0', '-O2', '-Wall', '-Wextra', '-Werror', '-Iinclude', 'tests/test_services.c', 'services/manager.c', 'services/namespace.c', '-o', 'build/test_services.exe'])
subprocess.run([str(ROOT / 'build/test_services.exe')], check=True)
subprocess.run([sys.executable, 'tests/test_packages.py'], check=True)
if not args.host_only:
    for memory in [128, 256, 512]:
        subprocess.run([sys.executable, 'tools/qemu_test.py', '--memory', str(memory)], check=True)
result = {'passed': True, 'elapsed_seconds': round(time.time()-started, 2), 'service_tests': 'passed', 'package_tests': 'passed (8 groups, including 1500 seeded mutations)', 'qemu_memory_mib': [] if args.host_only else [128, 256, 512]}
(ROOT / 'build/test-results.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))
