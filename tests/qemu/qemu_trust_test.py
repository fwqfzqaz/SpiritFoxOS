"""Key revocation over real QEMU: shell workflow, restart durability, fail-closed tamper."""
import hashlib
from pathlib import Path
import qemu_test
from qemu_persist_test import VM, generation, rewrite, replace_base
from sfpack import seal

ROOT = qemu_test.ROOT


def run():
    disk = ROOT/'build/trust-test.img'
    pristine = (ROOT/'build/persistent.img').read_bytes()
    # Reuse an existing entry: the prototype namespace has a fixed node quota.
    standby = seal((ROOT/'build/diskhello.sfk').read_bytes(),(ROOT/'.keys/backup.seed').read_bytes())
    disk.write_bytes(replace_base(pristine,{'/bin/disknext.sfk':standby}))
    machine = lambda name,**kw: VM(disk,name,**kw)
    fingerprint = hashlib.sha256((ROOT/'build/application.pub').read_bytes()).digest()[:8].hex()
    vm = machine('revoke')
    try:
        vm.boot('PERSIST READY')
        vm.command('trust','trust: anchors=3 revoked=0')
        vm.command('run /disk/bin/disknext.sfk','sfexec: exit 0')
        vm.command('write /persist/notes durable-note','write: OK')
        # Stage a candidate signed by the active key before revoking that key.
        vm.command('install /disk/bin/diskhello.sfk','install: COMMITTED')
        vm.command('revoke-key /disk/nope','revoke: failed code=16')
        vm.command('revoke-key /disk/etc/readme.txt','revoke: failed code=3')
        vm.command('revoke-key /disk/bin/diskexit.sfk','revoke: OK')
        vm.command('trust','trust: anchors=3 revoked=1')
        vm.command('trust','trust: revoked key='+fingerprint)
        vm.command('revoke-key /disk/bin/diskexit.sfk','revoke: UNCHANGED')
        # The standby anchor is unaffected; the revoked signer fails closed.
        vm.command('run /disk/bin/disknext.sfk','sfexec: exit 0')
        vm.command('run hello.sfk','sfexec: failed code=13')
        # The staged candidate is re-verified before execution and rolls back.
        vm.command('run installed','sfexec: failed code=13')
        assert b'update: ROLLED BACK' in vm.output, 'revoked candidate must roll back'
        vm.command('install /disk/bin/diskfault.sfk','install: failed code=13')
        vm.command('cat /persist/notes','durable-note')
        vm.command('persist','update: STABLE good=- candidate=-')
    finally:
        vm.close()
    gen = generation(disk)
    committed = disk.read_bytes()
    # The revocation survives restart and applies before any package runs.
    vm = machine('reboot')
    try:
        vm.boot('PERSIST READY generation='+str(gen))
        assert b'[PASS] signed sfmod loaded from disk without bundle fallback' in vm.output
        assert b'DISK PACKAGE REJECTED app code=13' in vm.output
        vm.command('trust','trust: anchors=3 revoked=1')
        vm.command('trust','trust: revoked key='+fingerprint)
        vm.command('run hello.sfk','sfexec: failed code=13')
        vm.command('run installed','sfexec: failed code=16')
        vm.command('run /disk/bin/disknext.sfk','sfexec: exit 0')
        vm.command('cat /persist/notes','durable-note')
    finally:
        vm.close()
    assert disk.read_bytes()==committed, 'read-only reboot changed disk'
    # A torn revocation record must fail the mount closed, not drop policy.
    disk.write_bytes(rewrite(committed,{'/boot/revoked':bytes(31)}))
    vm = machine('tampered')
    try:
        vm.boot('PERSIST UNAVAILABLE code=3')
        vm.command('run installed','sfexec: failed code=10')
        vm.command('persist','persist: UNAVAILABLE')
    finally:
        vm.close()
    disk.write_bytes(committed)
    vm = machine('recovered')
    try:
        vm.boot('PERSIST READY generation='+str(gen))
        vm.command('trust','trust: anchors=3 revoked=1')
        vm.command('run /disk/bin/disknext.sfk','sfexec: exit 0')
        vm.command('cat /persist/notes','durable-note')
    finally:
        vm.close()
    assert disk.read_bytes()==committed
    assert (ROOT/'build/persistent.img').read_bytes()==pristine
    print('TRUST QEMU PASS: purpose-separated anchors, revocation workflow, restart durability, standby app key unaffected, fail-closed tamper',flush=True)


if __name__=='__main__':
    run()
