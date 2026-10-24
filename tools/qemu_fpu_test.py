"""Fresh and interleaved x87/MMX/SSE state, timer preemption and isolated faults."""
from qemu_persist_test import VM,ROOT
import json


def run():
    source=(ROOT/'build/persistent.img').read_bytes();simd={}
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('cpu-state-'+label+'.img');disk.write_bytes(source)
        # max advertises hardware AVX/XSAVE; the OS still explicitly disables AVX.
        vm=VM(disk,'cpu-state-'+label,iommu=iommu,cpu='max,+nx')
        try:
            vm.boot();vm.command('write /persist/notes CPU-PRESERVED','write: OK');before=disk.read_bytes()
            vm.command('run fpprobe.sfk --require-avx-cpu','sfexec: exit 0')
            for _ in range(12):vm.command('cpu-state-test','CPU STATE PAIR PASSED')
            start=vm.send('run fpprobe.sfk --simd-fault');vm.expect('sfexec:',start)
            pos=vm.output.index(b'sfexec:',start);vm.expect('fox> ',pos);output=bytes(vm.output[start:])
            if b'sfexec: failed code=119' in output:simd[label]='exception-delivered'
            else:
                assert b'SIMD UNMASKED STATUS PRESERVED (TCG NO TRAP)' in output and b'sfexec: exit 0' in output
                simd[label]='tcg-status-only'
            for mode,code in [('avx',106),('x87-fault',116),('pending-x87',116),('spin',9)]:
                vm.command('run fpprobe.sfk --'+mode,'sfexec: failed code='+str(code))
                start=vm.send('run fpprobe.sfk');vm.expect('CPU STATE APP PASSED',start);vm.expect('sfexec: exit 0',start)
                vm.command('echo','echo: ping')
            vm.command('cat /persist/notes','CPU-PRESERVED');vm.command('run hello.sfk','sfexec: exit 0')
            vm.command('run pieprobe.sfk','sfexec: exit 0');vm.command('runat /disk/etc readprobe.sfk','OPENAT APP PASSED')
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            assert disk.read_bytes()==before,'CPU context test changed persistent bytes'
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    (ROOT/'build/fpu-context-results.json').write_text(json.dumps({'boots':2,'simd_exception_observation':simd},indent=2)+'\n')
    print('CPU STATE QEMU PASS: 2 boots; AVX-capable CPUs with AVX disabled, fresh physical FP/XMM/pointers/controls, 12 simultaneous pairs, private x87/MMX/XMM0-15 and rounding through yield/timer, isolated pending x87/AVX faults and timeout reuse; SIMD delivery observation='+str(simd),flush=True)


if __name__=='__main__':run()
