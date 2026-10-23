# SpiritFoxOS

基于 x86_64 UEFI 的可启动微内核原型。保留提供的 EFI 引导入口，按 [设计说明 v0.2](docs/design/DESIGN-v0.2.md) 实现用户态 sfmod、模块装卸、文件对象、native 运行时，以及从只读 virtio 磁盘验证并运行应用。

当前推进到 M6 的受限实现：legacy/modern virtio-blk、SFRFS1、双银行事务、签名应用双槽试运行/确认/失败回退，以及 QEMU q35 Intel VT-d 对 DMA RAM 访问的隔离。通用设备/IRQ 隔离、真实硬件断电保证、网络、图形与多核尚未完成；其余扩展保留在路线图中。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `boot/` | UEFI 引导桩、ELF 重定位与 payload 链接。 |
| `kernel/` | Core 0：入口、内存、IPC、设备、ACPI 与 VT-d。 |
| `services/` | ring-3 sfmod 与服务：init、namespace、package、txn、storage、trust、virtio。 |
| `include/` | 内核与服务共享的 ABI、结构与常量。 |
| `tests/` | [测试目录](tests/README.md)：`host/` C 测试、`integration/` Python 测试、`qemu/` 端到端测试。 |
| `tools/` | [工具目录](tools/README.md)：构建/测试/打包入口与 `package/` 格式实现。 |
| `docs/` | [文档索引](docs/README.md)：设计、架构、质量与项目文档。 |
| `vendor/` | 固定版本 Monocypher 4.0.3。 |
| `build/` | 忽略提交的构建产物、镜像与测试结果。 |

## 构建和测试

构建宿主为 Linux x64（qemu-system-x86_64 与 OVMF 固件来自系统包）或 Windows x64 + Python 3.11 或更新版本。所有工具、缓存、密钥和输出都位于仓库内，不需要全局安装器或修改 PATH。

```bash
python3 tools/bootstrap.py
python3 tools/test.py
```

第一步下载并校验 Zig 0.13.0 与 Monocypher 4.0.3；Linux 使用系统 QEMU/OVMF，Windows 下载 QEMU 构建。已提供的 Monocypher 源码在 `vendor/monocypher`。第二步执行基础服务、950 条内核 IPC/内存边界、108 条 openat、83 条截断、229 条 IOMMU、4069 条版本策略与 21 条信任撤销检查、16 组 Python 测试（含 2000 次变异）、783 个事务、3132 个更新及 3132 个截断故障场景，以及含真实越界 DMA、试运行、版本策略、目录授权、持久化截断、密钥撤销、驱动死亡和恢复的 QEMU 启动套件。

只构建：`python3 tools/build.py`。只运行宿主测试：`python3 tools/test.py --host-only`。已有镜像测试：`python3 tools/test.py --skip-build`。

构建产物：

- `build/esp/EFI/BOOT/BOOTX64.EFI`：包含内核和已签名启动包的 UEFI 应用。
- `build/*.sfmod`、`build/*.sfk`：实际构建出的模块与应用。
- `build/storage.img`：1 MiB 的只读测试盘，含 `diskhello.sfk`、`disk-echo.sfmod`、更新故障测试包和说明文件；磁盘应用与模块不在启动包中。
- `build/persistent.img`：2 MiB 新格式事务盘，具有相同只读包区；请复制后作为可写盘使用，重新构建会覆盖原始构建镜像。
- `build/development.pub`：当前开发公钥；私钥种子在忽略提交的 `.keys/development.seed`。
- `build/qemu-128.log`、`build/qemu-256.log`、`build/qemu-512.log`：串口验证记录。
- `build/test-results.json`：本次测试结果。

完整测试通过后，执行 `python3 tools/package_release.py` 生成 `build/SpiritFoxOS-M6-Updates.zip`，其中包含 EFI 镜像、两种测试盘、公钥、文档、测试结果与内容摘要。

重新构建会使用本工作区现有开发密钥。删除种子后会生成新密钥并重新构建信任锚；这里没有生产密钥、Secure Boot 配置或发布签名流程。

## 启动与交互

```bash
python3 tests/qemu/qemu_test.py --interactive
```

进入系统后通过串口看到 `fox>`。除 `help`、`modules`、`echo`、RAMFS 读写和 `run hello.sfk` 外，还支持：

```text
ls /disk/bin
cat /disk/etc/readme.txt
run /disk/bin/diskhello.sfk
trust
revoke-key /disk/bin/diskexit.sfk
unmount /disk
stop disk
start disk
mount /disk
```

`/tmp` 等 RAMFS 文件关机后清空；`/disk` 只读，不接收写入。挂载时不能停止块驱动，打开的文件或目录会阻止卸载。缺盘、坏元数据和 I/O 错误会显示诊断并保留串口环境；坏签名应用被拒绝。串口输入有界，自动化驱动按字节节流。

默认配置使用 legacy 可信驱动模式。加上 `--iommu` 可启用受限 Intel VT-d 与 modern PCI ACCESS_PLATFORM 传输；此时 legacy 旁路设备会被拒绝，DMA 只能访问获授区域。启动日志会明确报告实际模式。尚不支持通用硬件、virtio MMIO 或 IRQ 重映射，详见 [DMA 隔离与验证边界](docs/architecture/IOMMU.md)。

持久化端到端测试：`python3 tests/qemu/qemu_persist_test.py`，在镜像副本上自动验证写入、包安装、重启与故障恢复。交互启动：`python3 tests/qemu/qemu_test.py --interactive --persistent`，首次创建 `build/user-persistent.img`，以后继续使用该副本。挂载事务盘后支持：

```text
write /persist/notes durable-note
install /disk/bin/diskhello.sfk
run installed
persist
trust
revoke-key /disk/bin/diskexit.sfk
```

成功写入与安装均等待设备 flush。`truncate /persist/notes <字节数>` 可缩短、清空或补零扩展笔记，同长度不写盘；RAMFS 普通文件也支持截断。任何写/flush 错误会停止卷的后续读写与截断，需重新挂载或重启恢复；错误时不能假定新版本一定没有落盘。当前配额为五个 16 KiB 文件，只对笔记与一个应用提供入口。

安装提交候选包，第一次运行前持久记录试运行，按时退出 0 才确认。失败或未确认时重启回退已确认包；待更新期间拒绝覆盖安装。此回退只改变应用选择器，不能撤销外部副作用；格式、旧版兼容与健康标准见 [应用更新](docs/architecture/UPDATES.md)。

签名信任采用构建时双锚点（开发密钥与备用密钥）；`revoke-key /disk/bin/pkg` 撤销指定磁盘包的签名者并通过事务持久化，重启后仍生效，被撤销签名者的包按 `SF_REVOKED` 拒绝执行。`trust` 查询锚点与撤销状态；语义与限制见 [事务持久化](docs/architecture/PERSISTENCE.md)。

已确认版本形成软件下限：拒绝降级及同版本换包，精确重装返回 `UNCHANGED` 且不写盘；`persist` 可查询实际已确认版本。下限在健康确认时才随已确认包更新，不防止离线回放旧磁盘镜像。

隔离模式交互：`python3 tests/qemu/qemu_test.py --interactive --persistent --iommu`；专项验证：`python3 tests/qemu/qemu_iommu_test.py`。诊断 `dma-test read/write` 会刻意触发设备隔离故障并使该 VM 的磁盘驱动停止至重启，请只在测试 VM 使用。

## 包工具

```bash
python3 tools/package/sfpack.py module --manifest echo.json --image echo.elf --out echo.sfmod --seed .keys/development.seed
python3 tools/package/sfpack.py app --manifest hello.json --image hello.elf --out hello.sfk --seed .keys/development.seed
python3 tools/package/sfpack.py inspect build/hello.sfk --public-key build/development.pub
python3 tools/package/sfpack.py sign build/hello.sfk --seed .keys/development.seed
```

输入 ELF 是构建中间产物，交付包使用设计说明中的 96 字节头、64 字节段描述符、确定性 CBOR 和 Ed25519 尾部。当前代码只接受静态地址 native 子集；不支持资源包、动态导入、重定位或压缩。构建脚本中列出了实际清单字段。

目录授权示例：`runat /tmp fileprobe.sfk`、`runat /disk/etc readprobe.sfk`。只有显式选择目录且应用签名清单声明 namespace.read 时才授予只读访问；普通 run 不授予文件根。

详见 [截断与补零扩展](docs/architecture/TRUNCATE.md)、[目录能力与 openat](docs/architecture/NAMESPACE.md)、[存储协议与磁盘格式](docs/architecture/STORAGE.md)、[事务持久化](docs/architecture/PERSISTENCE.md)、[实现边界](docs/architecture/IMPLEMENTATION.md)、[安全审查与修复](docs/quality/SECURITY.md)、[测试记录](docs/quality/TESTING.md) 和 [后续路线](docs/project/ROADMAP.md)。
