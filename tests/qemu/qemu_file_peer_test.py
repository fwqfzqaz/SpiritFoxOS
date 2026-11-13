"""A rejected live module must not consume the manager's capability budget."""
import json
import re
from qemu_persist_test import VM,ROOT


def run():
    source=(ROOT/'build/persistent.img').read_bytes();results=[]
    for modern in (False,True):
        label='modern' if modern else 'legacy';disk=ROOT/'build'/('file-peer-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'file-peer-'+label,iommu=modern)
        try:
            vm.boot()
            for _ in range(3):
                start=vm.send('file-peer-test')
                vm.expect('FILE PEER TEST PASSED',start);vm.expect(' after=',start)
                # The initial boot prompt may still be queued. Only accept a
                # prompt after this diagnostic's complete metric line.
                vm.expect('fox> ',bytes(vm.output).index(b' after=',start))
                match=re.search(rb'FILE PEER TEST PASSED code=(\d+) rounds=(\d+) before=(\d+) during=(\d+) after=(\d+)',bytes(vm.output[start:]))
                assert match,bytes(vm.output[start:]);code,rounds,before,during,after=map(int,match.groups())
                assert code==0 and rounds==64 and during==before+1 and after==before,(code,rounds,before,during,after)
                results.append({'transport':label,'rounds':rounds,'before':before,'during_control_endpoint':during,'after':after})
                vm.command('run hello.sfk','sfexec: exit 0');vm.command('echo','echo: ping')
            vm.command('login alice alice','alice@fox> ')
            vm.command('file-peer-test','denied: root session required')
            vm.command('run foreigncap.sfmod','sfexec: failed code=2')
            vm.command('run hello.sfk','sfexec: exit 0')
            vm.command('logout','logout: session 2 closed')
            vm.command('cat /disk/etc/readme.txt','SpiritFoxOS')
            assert disk.read_bytes()==source,'Foreign-peer cleanup or ordinary apps wrote disk'
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    (ROOT/'build/file-peer-results.json').write_text(json.dumps({'passed':True,'guest_boots':2,'transferred_caps':384,'batches':results,'disk_unchanged':True},indent=2)+'\n',encoding='utf-8')
    print('FILE PEER QEMU PASS: 2 boots, 384 rejected live-peer transfers; Root cap budget stable, cleanup/ordinary app/echo/disk and admin/module-kind gates preserved',flush=True)


if __name__=='__main__':run()
