"""Rotate external app keys, then revoke an old key across real guest reboots."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from qemu_persist_test import VM, ROOT, replace_base, generation

FIXTURES = ROOT/'.cache/trust-fixtures'
SEEDS = {name: hashlib.sha256(('SpiritFoxOS TRUST TEST ONLY / '+name).encode()).digest() for name in ('old','next','revoked','module','unknown')}
PATHS = dict(zip(('old','next','oldnext','revoked','module','unknown'),('diskupdate','disknext','diskfault','diskexit','diskspin','diskother')))


def package_path(name):
    return '/disk/bin/'+PATHS[name]+'.sfk'


def make_fixtures():
    # This runs in a short-lived child so Windows can unload the crypto DLL
    # before a later build replaces it. These fixed keys are test-only.
    from sfpack import Crypto, pack, seal
    import trust_policy
    crypto = Crypto()
    FIXTURES.mkdir(exist_ok=True)
    keys = trust_policy.development((ROOT/'build/development.pub').read_bytes(),(ROOT/'build/application.pub').read_bytes())['keys']
    SEEDS['module'] = (ROOT/'.keys/development.seed').read_bytes()
    application_seed = (ROOT/'.keys/application.seed').read_bytes()
    legacy = {'version':1,'keys':[dict(k) for k in keys]}
    legacy['keys'][0]['usage'] = ['module','app']
    (FIXTURES/'legacy.json').write_text(json.dumps(legacy),encoding='utf-8')
    (FIXTURES/'legacy.sfk').write_bytes(seal((ROOT/'build/diskhello.sfk').read_bytes(),SEEDS['module'],crypto))
    for name in ('old','next','revoked'):
        usage = ['module','app'] if name=='revoked' else ['app']
        keys.append({'public_key':crypto.key(SEEDS[name])[1].hex(), 'usage':usage, 'revoked':name=='revoked'})
    for phase in ('overlap','revoked'):
        policy = {'version':1,'keys':[dict(k) for k in keys]}
        if phase=='revoked':
            policy['keys'][2]['revoked'] = True
        (FIXTURES/(phase+'.json')).write_text(json.dumps(policy),encoding='utf-8')
    manifest = {'app_id':'org.spiritfox.diskhello','version':'0.4.0','runtime_id':'org.spiritfox.native','app_abi':1,'requested_caps':['stdio']}
    image = (ROOT/'build/diskhello.elf').read_bytes()
    for name,version,signer in [('old','0.4.0','old'),('next','0.5.0','next'),('oldnext','0.6.0','old'),('revoked','0.6.0','revoked'),('module','0.6.0','module'),('unknown','0.6.0','unknown')]:
        (FIXTURES/(name+'.sfk')).write_bytes(pack(dict(manifest,version=version),image,2,SEEDS[signer]))
    for name in ('next','revoked'):
        signer = application_seed if name=='next' else SEEDS[name]
        (FIXTURES/('bootstrap-'+name+'.sfmod')).write_bytes(seal((ROOT/'build/sf-init.sfmod').read_bytes(),signer,crypto))
    (FIXTURES/'app-signed-module.sfmod').write_bytes(seal((ROOT/'build/disk-echo.sfmod').read_bytes(),application_seed,crypto))


def child(args, label):
    result = subprocess.run([sys.executable,*args],cwd=ROOT,capture_output=True,text=True)
    (ROOT/'build'/('trust-'+label+'.txt')).write_text(result.stdout+result.stderr,encoding='utf-8')
    if result.returncode:
        raise RuntimeError(label+': '+(result.stdout+result.stderr)[-3000:])


def boot_rejection(label, replacement):
    efi = ROOT/'build/esp/EFI/BOOT/BOOTX64.EFI'
    original = efi.read_bytes()
    package = (ROOT/'build/sf-init.sfmod').read_bytes()
    assert len(package)==len(replacement) and original.count(package)==1
    vm = None
    try:
        efi.write_bytes(original.replace(package,replacement))
        vm = VM(ROOT/'build/storage.img','trust-bootstrap-'+label,writable=False)
        # A firmware rejection is expected here, so use the raw serial queue
        # rather than VM.expect(), which treats any EFISTUB rejection as fatal.
        import queue
        import time
        until = time.monotonic()+30
        while b'[EFISTUB] rejected bootstrap package' not in vm.output:
            if time.monotonic()>until:
                raise RuntimeError('Bootstrap trust rejection missing: '+label)
            try:
                vm.output.extend(vm.queue.get(timeout=.1))
            except queue.Empty:
                if vm.process.poll() is not None:
                    raise RuntimeError('Guest exited before expected rejection')
        assert b'[INIT]' not in vm.output
    finally:
        if vm:
            vm.close(False)
        efi.write_bytes(original)


def run():
    names = ['esp/EFI/BOOT/BOOTX64.EFI','storage.img','persistent.img','development.pub','application.pub','trust-policy.json']
    baseline = {n:hashlib.sha256((ROOT/'build'/n).read_bytes()).digest() for n in names}
    FIXTURES.mkdir(exist_ok=True)
    restore_policy = FIXTURES/'restore.json'
    restore_policy.write_bytes((ROOT/'build/trust-policy.json').read_bytes())
    saved = {}
    try:
        child(['tools/qemu_trust_test.py','--fixtures'],'fixtures-default')
        child(['tools/build.py','--trust-policy',str(FIXTURES/'legacy.json')],'build-legacy')
        legacy_base = replace_base((ROOT/'build/persistent.img').read_bytes(),{'/bin/diskhello.sfk':(FIXTURES/'legacy.sfk').read_bytes()})
        migrated = {}
        for iommu in (False,True):
            label = 'modern' if iommu else 'legacy'
            disk = ROOT/'build'/('trust-legacy-key-'+label+'.img')
            disk.write_bytes(legacy_base)
            vm = VM(disk,'trust-key-migration-'+label,iommu=iommu)
            try:
                vm.boot()
                for name in ('diskhello','diskupdate'):
                    vm.command('install /disk/bin/'+name+'.sfk','install: COMMITTED')
                    vm.command('run installed','sfexec: exit 0')
                vm.command('persist','confirmed-version=0.2.0 policy=monotonic')
            finally:
                vm.close()
            migrated[iommu] = disk.read_bytes()
        child(['tools/build.py','--trust-policy',str(FIXTURES/'overlap.json')],'build-overlap')
        child(['tools/qemu_trust_test.py','--fixtures'],'fixtures-overlap')
        for iommu,data in migrated.items():
            label = 'modern' if iommu else 'legacy'
            disk = ROOT/'build'/('trust-legacy-key-'+label+'.img')
            disk.write_bytes(data)
            vm = VM(disk,'trust-key-migrated-strict-'+label,iommu=iommu)
            try:
                vm.boot()
                vm.expect('DISK PACKAGE REJECTED app code=2')
                vm.command('run installed','sfexec: exit 0')
                vm.command('run /disk/bin/diskhello.sfk','sfexec: failed code=2')
                vm.command('install /disk/bin/diskhello.sfk','install: failed code=2')
                vm.command('persist','confirmed-version=0.2.0 policy=monotonic')
            finally:
                vm.close()
            assert disk.read_bytes()==data, 'Migrated app or rejected legacy signer changed disk'
        for name in ('next','revoked'):
            boot_rejection(name,(FIXTURES/('bootstrap-'+name+'.sfmod')).read_bytes())
        files = {'/bin/'+PATHS[name]+'.sfk':(FIXTURES/(name+'.sfk')).read_bytes() for name in PATHS}
        files['/modules/disk-echo.sfmod'] = (FIXTURES/'app-signed-module.sfmod').read_bytes()
        original = replace_base((ROOT/'build/persistent.img').read_bytes(),files)
        for iommu in (False,True):
            label = 'modern' if iommu else 'legacy'
            disk = ROOT/'build'/('trust-'+label+'.img')
            disk.write_bytes(original)
            vm = VM(disk,'trust-overlap-'+label,iommu=iommu)
            try:
                vm.boot()
                vm.expect('DISK PACKAGE REJECTED module code=2')
                vm.command('trust','trust: build-time keys=5')
                for name,code in [('revoked',13),('module',2),('unknown',4)]:
                    before = disk.read_bytes()
                    vm.command('run '+package_path(name),'sfexec: failed code='+str(code))
                    vm.command('install '+package_path(name),'install: failed code='+str(code))
                    assert disk.read_bytes()==before, 'Rejected signer changed transaction disk'
                for name in ('old','next'):
                    vm.command('install '+package_path(name),'install: COMMITTED')
                    vm.command('run installed','sfexec: exit 0')
                    if name=='old':
                        old_confirmed = disk.read_bytes()
                vm.command('persist','confirmed-version=0.5.0 policy=monotonic')
                stable = disk.read_bytes()
                vm.command('install '+package_path('oldnext'),'install: COMMITTED')
            finally:
                vm.close()
            saved[iommu] = (old_confirmed,stable,disk.read_bytes())
        child(['tools/build.py','--trust-policy',str(FIXTURES/'revoked.json')],'build-revoked')
        for iommu,(old_confirmed,stable,pending) in saved.items():
            label = 'modern' if iommu else 'legacy'
            disk = ROOT/'build'/('trust-'+label+'.img')
            for state,data in [('old-confirmed',old_confirmed),('stable',stable),('pending',pending)]:
                disk.write_bytes(data)
                before_gen = generation(disk)
                vm = VM(disk,'trust-revoked-'+state+'-'+label,iommu=iommu)
                try:
                    vm.boot()
                    vm.command('trust',' REVOKED app')
                    vm.command('run '+package_path('old'),'sfexec: failed code=13')
                    vm.command('run '+package_path('next'),'sfexec: exit 0')
                    if state=='old-confirmed':
                        vm.command('run installed','sfexec: failed code=13')
                        vm.command('install '+package_path('next'),'install: failed code=13')
                        assert disk.read_bytes()==data, 'Revoked version floor must fail closed without writes'
                        continue
                    if state=='pending':
                        vm.command('run installed','sfexec: failed code=13')
                        vm.command('persist','confirmed-version=0.5.0 policy=monotonic')
                    vm.command('run installed','sfexec: exit 0')
                    before = disk.read_bytes()
                    vm.command('install '+package_path('oldnext'),'install: failed code=13')
                    assert disk.read_bytes()==before
                finally:
                    vm.close()
                assert generation(disk)==before_gen+(2 if state=='pending' else 0)
    finally:
        # Restore the normal developer policy and regenerate exact deliverables,
        # even when a negative test fails; never ship the public test keys.
        child(['tools/build.py','--trust-policy',str(restore_policy)],'restore-default')
        assert all(hashlib.sha256((ROOT/'build'/n).read_bytes()).digest()==h for n,h in baseline.items()), 'Default deliverables changed after trust fixture restore'
    print('TRUST QEMU PASS: 14 boots; separate default keys and explicit legacy signer migration, bootstrap purpose/revocation rejection, cross-key install/confirm, revoked/unknown/wrong-purpose rejection without writes, revoked confirmed floor fails closed, pending revoked trial rollback, legacy and modern, original artifacts restored',flush=True)


if __name__ == '__main__':
    if '--fixtures' in sys.argv:
        make_fixtures()
    else:
        run()
