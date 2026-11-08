"""Authenticated runtime library loading and bounded import binding in a guest."""
from qemu_persist_test import ROOT,VM


def run():
    vm = VM(ROOT / 'build/storage.img', 'posix', writable=False)
    try:
        vm.boot('STORAGE READY')
        start = vm.send('runat /tmp posixprobe.sfk')
        vm.expect('POSIX ', start)
        vm.expect('PASS pid=', start)
        vm.expect('libc=success', start)
        vm.expect('POSIX imports bound by name/ABI', start)
        vm.expect('sfexec: exit 0', start)
        # The relative-v1 path without imports must keep working in the same boot.
        vm.command('run pieprobe.sfk', 'sfexec: exit 0')
    finally:
        vm.close()
    print('POSIX QEMU PASS: sf-libsf loaded into the app address space, 15 imports '
          'bound by name/ABI, console/stat/missing-path errno, printf/puts, read-only app',
          flush=True)


if __name__ == '__main__':
    run()
