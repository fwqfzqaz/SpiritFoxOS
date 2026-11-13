"""Linux 兼容 S2：静态 PIE hello 的真实上行调用与负例边界。"""
from qemu_persist_test import VM,ROOT


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    disk=ROOT/'build'/'linux-compat.img';disk.write_bytes(source)
    vm=VM(disk,'linux-compat')
    try:
        # 兼容测试在 boot_splash 之前运行；失败会触发 sf_assert 而无法到达 BOOT TESTS。
        vm.boot()
        vm.expect('LINUX COMPAT HELLO')
        vm.expect('LINUX COMPAT TESTS PASSED')
        # 原生路径不受 0x81/0x80 新语义影响。
        vm.command('run hello.sfk','sfexec: exit 0')
    finally:
        vm.close()
    assert disk.read_bytes()==source,'Linux compat path changed persistent bytes'
    print('LINUX COMPAT QEMU PASS: hello output, exit 0, registration bounds, denied 0x81, nested upcall termination, native regression',flush=True)


if __name__=='__main__':
    run()
