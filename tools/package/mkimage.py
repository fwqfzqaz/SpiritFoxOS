"""把 build/esp 目录打包成可启动的 MBR + FAT32 系统镜像。

UEFI 固件按 MBR 分区表中的 0xEF（EFI System）类型查找 FAT 卷，然后加载
EFI/BOOT/BOOTX64.EFI。镜像只包含启动分区，不含 SFRFS 存储盘；存储仍作为
独立 virtio-blk 镜像使用。镜像内容为固定时间戳，重复构建结果一致。
"""
import argparse
import struct
from pathlib import Path

SECTOR = 512
PART_START = 2048
RESERVED_SECTORS = 32
FAT_COUNT = 2
SECTORS_PER_CLUSTER = 1
CLUSTER_BYTES = SECTOR * SECTORS_PER_CLUSTER
EOC = 0x0FFFFFFF
# 固定写入时间，保证相同输入产生相同镜像。
FIXED_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1
FIXED_TIME = 0
VOLUME_ID = 0x53464F53


def _chs(lba):
    """把 LBA 换算成 MBR 使用的 CHS 三元组。"""
    cylinders = lba // (255 * 63)
    remainder = lba % (255 * 63)
    heads = remainder // 63
    sectors = remainder % 63 + 1
    return cylinders, heads, sectors


def _pack_chs(lba):
    cylinders, heads, sectors = _chs(lba)
    return bytes([heads, (sectors & 0x3F) | ((cylinders >> 2) & 0xC0), cylinders & 0xFF])


def _short_name(name):
    """把 8.3 名称拆成 FAT 目录项使用的大写 8+3 字节。"""
    upper = name.upper()
    base, _, ext = upper.rpartition('.')
    if not _:
        base, ext = upper, ''
    allowed = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-$~!#%&()@^`{}')
    if (not base or len(base) > 8 or len(ext) > 3
            or any(c not in allowed for c in base + ext) or '.' in ext):
        raise ValueError('系统镜像只支持 8.3 名称，无法写入: ' + name)
    return base.ljust(8).encode('ascii'), ext.ljust(3).encode('ascii')


def _geometry(total_sectors):
    """计算 FAT32 的 FAT 扇区数与数据簇数。"""
    fat_sectors = 1
    while True:
        data_sectors = total_sectors - RESERVED_SECTORS - FAT_COUNT * fat_sectors
        clusters = data_sectors // SECTORS_PER_CLUSTER
        needed = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        # 对齐到 8 扇区，让 FAT 起始位置对所有固件都保持常见布局。
        aligned = (needed + 7) // 8 * 8
        if aligned <= fat_sectors:
            if clusters < 65525:
                raise ValueError('镜像容量过小，FAT32 至少需要约 33 MiB')
            return fat_sectors, clusters
        fat_sectors = aligned


class FatImage:
    def __init__(self, total_sectors):
        self.total_sectors = total_sectors
        self.fat_sectors, self.clusters = _geometry(total_sectors)
        self.data_sectors = total_sectors - RESERVED_SECTORS - FAT_COUNT * self.fat_sectors
        self.image = bytearray((PART_START + total_sectors) * SECTOR)
        self.fat = bytearray(self.fat_sectors * SECTOR)
        struct.pack_into('<I', self.fat, 0, 0x0FFFFFF8)
        struct.pack_into('<I', self.fat, 4, 0x0FFFFFFF)
        self.next_cluster = 3

    def _set_entry(self, cluster, value):
        struct.pack_into('<I', self.fat, cluster * 4, value)

    def _alloc(self, count, first=None):
        if first is None:
            first = self.next_cluster
        if first + count - 1 >= self.clusters + 2:
            raise ValueError('ESP 内容超出系统镜像容量')
        self.next_cluster = max(self.next_cluster, first + count)
        for index in range(count - 1):
            self._set_entry(first + index, first + index + 1)
        self._set_entry(first + count - 1, EOC)
        return first

    def _cluster_offset(self, cluster):
        sector = RESERVED_SECTORS + FAT_COUNT * self.fat_sectors + (cluster - 2) * SECTORS_PER_CLUSTER
        return (PART_START + sector) * SECTOR

    def _write_file(self, data):
        if not data:
            return 0
        clusters = (len(data) + CLUSTER_BYTES - 1) // CLUSTER_BYTES
        first = self._alloc(clusters)
        offset = self._cluster_offset(first)
        self.image[offset:offset + len(data)] = data
        return first

    @staticmethod
    def _entry(name8, attr, cluster, size, name3=b'   '):
        return struct.pack('<8s3sBBBHHHHHHHI', name8, name3, attr, 0, 0,
                           FIXED_TIME, FIXED_DATE, FIXED_DATE,
                           cluster >> 16, FIXED_TIME, FIXED_DATE,
                           cluster & 0xFFFF, size)

    def _write_directory(self, source, parent_cluster, root=False):
        children = sorted(source.iterdir(), key=lambda item: (item.is_file(), item.name))
        entry_count = len(children) + (0 if root else 2)
        clusters = max(1, (entry_count * 32 + CLUSTER_BYTES - 1) // CLUSTER_BYTES)
        first = self._alloc(clusters, 2 if root else None)
        entries = []
        if not root:
            entries.append(self._entry(b'.', 0x10, first, 0, b'\0\0\0'))
            entries.append(self._entry(b'..', 0x10, parent_cluster, 0, b'\0\0\0'))
        for child in children:
            name8, name3 = _short_name(child.name)
            if child.is_dir():
                cluster = self._write_directory(child, first)
                entries.append(self._entry(name8, 0x10, cluster, 0, name3))
            else:
                data = child.read_bytes()
                cluster = self._write_file(data)
                entries.append(self._entry(name8, 0x20, cluster, len(data), name3))
        offset = self._cluster_offset(first)
        self.image[offset:offset + len(entries) * 32] = b''.join(entries)
        return first

    def _boot_sector(self):
        boot = bytearray(SECTOR)
        boot[0:3] = b'\xEB\x58\x90'
        boot[3:11] = b'MSWIN4.1'
        struct.pack_into('<H', boot, 11, SECTOR)
        boot[13] = SECTORS_PER_CLUSTER
        struct.pack_into('<H', boot, 14, RESERVED_SECTORS)
        boot[16] = FAT_COUNT
        struct.pack_into('<H', boot, 17, 0)
        struct.pack_into('<H', boot, 19, 0)
        boot[21] = 0xF8
        struct.pack_into('<H', boot, 22, 0)
        struct.pack_into('<H', boot, 24, 63)
        struct.pack_into('<H', boot, 26, 255)
        struct.pack_into('<I', boot, 28, PART_START)
        struct.pack_into('<I', boot, 32, self.total_sectors)
        struct.pack_into('<I', boot, 36, self.fat_sectors)
        struct.pack_into('<H', boot, 40, 0)
        struct.pack_into('<H', boot, 42, 0)
        struct.pack_into('<I', boot, 44, 2)
        struct.pack_into('<H', boot, 48, 1)
        struct.pack_into('<H', boot, 50, 6)
        boot[64] = 0x80
        boot[66] = 0x29
        struct.pack_into('<I', boot, 67, VOLUME_ID)
        boot[71:82] = b'SPIRITFOXOS'
        boot[82:90] = b'FAT32   '
        boot[510:512] = b'\x55\xAA'
        return boot

    def _fsinfo(self):
        info = bytearray(SECTOR)
        struct.pack_into('<I', info, 0, 0x41615252)
        struct.pack_into('<I', info, 484, 0x61417272)
        struct.pack_into('<I', info, 488, max(0, self.clusters - (self.next_cluster - 2)))
        struct.pack_into('<I', info, 492, self.next_cluster)
        struct.pack_into('<I', info, 508, 0xAA550000)
        return info

    def build(self, source):
        self._write_directory(source, 0, root=True)
        partition = PART_START * SECTOR
        boot = self._boot_sector()
        info = self._fsinfo()
        self.image[partition:partition + len(boot)] = boot
        self.image[partition + SECTOR:partition + 2 * SECTOR] = info
        self.image[partition + 6 * SECTOR:partition + 7 * SECTOR] = boot
        self.image[partition + 7 * SECTOR:partition + 8 * SECTOR] = info
        fat_offset = partition + RESERVED_SECTORS * SECTOR
        for index in range(FAT_COUNT):
            start = fat_offset + index * self.fat_sectors * SECTOR
            self.image[start:start + len(self.fat)] = self.fat
        mbr = bytearray(SECTOR)
        struct.pack_into('<I', mbr, 440, VOLUME_ID)
        end = PART_START + self.total_sectors - 1
        mbr[446:462] = struct.pack('<B3sB3sII', 0x80, _pack_chs(PART_START), 0xEF,
                                     _pack_chs(end), PART_START, self.total_sectors)
        mbr[510:512] = b'\x55\xAA'
        self.image[0:SECTOR] = mbr
        return bytes(self.image)


def main():
    parser = argparse.ArgumentParser(description='生成 SpiritFoxOS 可启动系统镜像')
    parser.add_argument('--source', type=Path, default=Path('build/esp'), help='ESP 目录（默认 build/esp）')
    parser.add_argument('--out', type=Path, default=Path('build/spiritfoxos.img'), help='输出镜像路径')
    parser.add_argument('--size-mib', type=int, default=64, help='启动分区容量，单位 MiB（默认 64）')
    args = parser.parse_args()
    if not args.source.is_dir():
        parser.error('ESP 目录不存在: ' + str(args.source))
    if args.size_mib <= 0:
        parser.error('--size-mib 必须为正整数')
    image = FatImage(args.size_mib * 1024 * 1024 // SECTOR).build(args.source)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(image)
    print('System image:', args.out.resolve(), len(image), 'bytes')


if __name__ == '__main__':
    main()
