"""Workspace-local tools. No installers, registry changes or global caches."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
STORE = ROOT / '.tools'
STORE.mkdir(exist_ok=True)
os.environ['TEMP'] = os.environ['TMP'] = str(ROOT / '.cache')


def fetch(url, name, digest=None, algorithm='sha256'):
    target = STORE / name
    if not target.exists():
        print('Downloading', url, flush=True)
        urllib.request.urlretrieve(url, target)
    if digest and hashlib.new(algorithm, target.read_bytes()).hexdigest() != digest:
        target.unlink()
        raise ValueError('Checksum mismatch: ' + name)
    return target


def zig():
    if (STORE / 'zig' / 'zig.exe').exists():
        return
    index = json.load(urllib.request.urlopen('https://ziglang.org/download/index.json'))
    item = index['0.13.0']['x86_64-windows']
    archive = fetch(item['tarball'], 'zig.zip', item['shasum'])
    with zipfile.ZipFile(archive) as z:
        prefix = z.namelist()[0].split('/')[0]
        z.extractall(STORE)
    (STORE / prefix).rename(STORE / 'zig')
    print('Zig ready', flush=True)


def qemu():
    if (STORE / 'qemu' / 'qemu-system-x86_64.exe').exists():
        return
    base = 'https://qemu.weilnetz.de/w64/qemu-w64-setup-20260729'
    digest = urllib.request.urlopen(base + '.sha512').read().decode().split()[0]
    archive = fetch(base + '.exe', 'qemu.exe', digest, 'sha512')
    seven = fetch('https://www.7-zip.org/a/7zr.exe', '7zr.exe', '256feca8e274e5da655e2a284fabafd9f554365eb164862089dacd4e8276d282')
    extra = fetch('https://www.7-zip.org/a/7z2301-extra.7z', '7z-extra.7z', 'db3a1cbe57a26fac81b65c6a2d23feaecdeede3e4c1fe8fb93a7b91d72d1094c')
    subprocess.run([str(seven), 'x', str(extra), '-o' + str(STORE / '7zip'), '-y'], check=True, stdout=subprocess.DEVNULL)
    full = fetch('https://www.7-zip.org/a/7z2301-x64.exe', '7z-full.exe', '26cb6e9f56333682122fafe79dbcdfd51e9f47cc7217dccd29ac6fc33b5598cd')
    subprocess.run([str(STORE / '7zip' / '7za.exe'), 'x', str(full), '-o' + str(STORE / '7zip-full'), '-y'], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([str(STORE / '7zip-full' / '7z.exe'), 'x', str(archive), '-o' + str(STORE / 'qemu'), '-y'], check=True, stdout=subprocess.DEVNULL)
    print('QEMU ready', flush=True)


def crypto():
    dest = ROOT / 'vendor' / 'monocypher'
    if (dest / 'monocypher.c').exists():
        return
    base = 'https://monocypher.org/download/monocypher-4.0.3'
    digest = urllib.request.urlopen(base + '.tar.gz.sha512').read().decode().split()[0]
    archive = fetch(base + '.tar.gz', 'monocypher.tar.gz', digest, 'sha512')
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as t:
        for member in t.getmembers():
            name = Path(member.name).name
            if name in ('monocypher.c', 'monocypher.h', 'monocypher-ed25519.c', 'monocypher-ed25519.h', 'LICENCE.md'):
                (dest / name).write_bytes(t.extractfile(member).read())
    print('Monocypher ready', flush=True)


if __name__ == '__main__':
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        for result in pool.map(lambda action: action(), [zig, qemu, crypto]):
            pass
