"""Actual DMA faults against a kernel canary, plus isolation-mode regressions."""
import hashlib
import struct
from qemu_persist_test import VM, ROOT, run as persistence
import qemu_test


def run():
    for memory in (128,512):
        qemu_test.run(memory=memory,iommu=True)
    disk=ROOT/'build/storage.img'
    original=hashlib.sha256(disk.read_bytes()).digest()
    for direction in ('write','read'):
        vm=VM(disk,'iommu-dma-'+direction,iommu=True,writable=False)
        try:
            vm.boot('[IOMMU] VT-d active')
            if b'[VIRTIO] modern ACCESS_PLATFORM transport' not in vm.output:
                raise RuntimeError('Virtio bypass protection was not negotiated')
            vm.command('dma-test '+direction,'DMA PROBE PASSED: guard intact, device quarantined')
            assert b'[IOMMU] blocked DMA source=' in vm.output
            assert b'offending driver terminated and quarantined' in vm.output
            vm.command('echo','echo: ping')
            vm.command('run hello.sfk','sfexec: exit 0')
            vm.command('unmount /disk','disk: UNMOUNTED')
            vm.command('start disk','disk: start refused')
        finally:
            vm.close()
        assert hashlib.sha256(disk.read_bytes()).digest()==original
    duplicate=bytearray(64);duplicate[:4]=b'DMAR';struct.pack_into('<I',duplicate,4,64);duplicate[8]=1;duplicate[36]=38;struct.pack_into('<HHBBHQ',duplicate,48,0,16,1,0,0,0xfed90000);duplicate[9]=(-sum(duplicate))&255
    extra_table=ROOT/'build/duplicate-dmar.bin';extra_table.write_bytes(duplicate)
    for case in ('legacy-bypass','duplicate-dmar'):
        kwargs={'legacy':True} if case=='legacy-bypass' else {'extra':['-acpitable','file=build/duplicate-dmar.bin']}
        vm=VM(disk,'iommu-'+case,iommu=True,writable=False,**kwargs)
        try:
            vm.boot('STORAGE UNAVAILABLE')
            if case=='duplicate-dmar':
                assert b'[IOMMU] unsupported DMAR' in vm.output
            vm.command('echo','echo: ping')
            vm.command('run hello.sfk','sfexec: exit 0')
        finally:
            vm.close()
        assert hashlib.sha256(disk.read_bytes()).digest()==original
    persistence(iommu=True)
    print('IOMMU QEMU PASS: 128/512 MiB, actual DMA read/write denied, canary intact, driver quarantine, persistent OUT/FLUSH recovery',flush=True)


if __name__=='__main__':
    run()
