"""Bundle the exact locally tested boot artifacts and public documentation."""
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT/'build'
result = json.loads((BUILD/'test-results.json').read_text(encoding='utf-8'))
cases = {'missing','badheader','badhash','badsignature','writable','ioerror'}
if result.get('passed') is not True or result.get('version_policy_host') is not True or result.get('version_policy_qemu') is not True or result.get('update_health_qemu') is not True or result.get('update_fault_scenarios') != 3132 or result.get('iommu_qemu') is not True or result.get('persistence_qemu') is not True or result.get('qemu_memory_mib') != [128,256,512] or set(result.get('storage_failure_cases',[])) != cases:
    raise SystemExit('Run the full test suite before packaging')
if result.get('security_host') is not True or result.get('security_qemu') is not True:
    raise SystemExit('Run the kernel boundary and storage failure security regressions before packaging')
if result.get('openat_host_assertions') != 108 or result.get('openat_qemu') is not True:
    raise SystemExit('Run the directory capability host and VM regressions before packaging')
if result.get('truncate_host_assertions') != 83 or result.get('resize_fault_scenarios') != 3132 or result.get('truncate_qemu') is not True:
    raise SystemExit('Run truncate/zero-extension fault and VM regressions before packaging')
for name in ['esp/EFI/BOOT/BOOTX64.EFI','storage.img','persistent.img','development.pub']:
    actual = hashlib.sha256((BUILD/name).read_bytes()).hexdigest()
    if result.get('artifacts_sha256',{}).get(name) != actual:
        raise SystemExit('Artifact changed since validation: '+name)
items = [(BUILD/'esp/EFI/BOOT/BOOTX64.EFI','EFI/BOOT/BOOTX64.EFI'),(BUILD/'storage.img','storage.img'),(BUILD/'persistent.img','persistent.img'),(BUILD/'development.pub','development.pub'),(BUILD/'test-results.json','test-results.json'),(ROOT/'README.md','README.md')]
items += [(p,'docs/'+p.name) for p in sorted((ROOT/'docs').glob('*.md'))]
manifest = {'files_sha256': {name: hashlib.sha256(path.read_bytes()).hexdigest() for path,name in items}, 'validation': 'tools/test.py: host + legacy storage + VT-d/modern DMA isolation + persistence/update-health/version-policy/fault/restart + kernel-boundary/storage-failure security + directory-capability openat + truncate/zero-extension fault/recovery VMs'}
archive = BUILD/'SpiritFoxOS-M6-Updates.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
    for path,name in items:
        z.write(path,name)
    z.writestr('manifest.json',json.dumps(manifest,indent=2)+'\n')
print(str(archive),archive.stat().st_size,'bytes')
