# SpiritFoxOS

基于 x86_64 UEFI 的可启动微内核原型。保留提供的 EFI 引导入口，按 [设计说明 v0.2](SpiritFoxOS_设计说明_v0.2.md) 实现用户态 sfmod、模块装卸、文件对象、native 运行时，以及从只读 virtio 磁盘验证并运行应用。

当前推进到 M6 的受限实现：legacy/modern virtio-blk、SFRFS1、双银行事务、签名应用双槽试运行/确认/失败回退，以及 QEMU q35 Intel VT-d 对 DMA RAM 访问的隔离。通用设备/IRQ 隔离、真实硬件断电保证、网络、图形与多核尚未完成；其余扩展保留在路线图中。

## 构建和测试

当前构建宿主为 Windows x64 + Python 3.11 或更新版本。所有工具、缓存、密钥和输出都位于仓库内，不需要 WSL、全局安装器或修改 PATH。

```powershell
python tools/bootstrap.py
python tools/test.py
```

第一步下载并校验 Zig 0.13.0、QEMU Windows 构建和 Monocypher 4.0.3。已提供的 Monocypher 源码在 `vendor/monocypher`。第二步执行基础服务、950 条内核 IPC/内存边界、8155 条参数与 12034 条环境检查、108 条 openat、83 条截断、229 条 IOMMU 与 4069 条版本策略检查、28 组 Python 测试（含 3500 次变异）、783 个事务、3132 个更新及 3132 个截断故障场景，以及含真实越界 DMA、试运行、版本策略、目录授权、持久化截断、密钥轮换/撤销与旧密钥迁移、驱动死亡和恢复的 117 次 QEMU 启动。

只构建：`python tools/build.py`。只运行宿主测试：`python tools/test.py --host-only`。已有镜像测试：`python tools/test.py --skip-build`。

构建产物：

- `build/esp/EFI/BOOT/BOOTX64.EFI`：包含内核和已签名启动包的 UEFI 应用。
- `build/*.sfmod`、`build/*.sfk`：实际构建出的模块与应用。
- `build/storage.img`：1 MiB 的只读测试盘，含 `diskhello.sfk`、`disk-echo.sfmod`、更新故障测试包和说明文件；diskhello 与 disk-echo 不在启动包中，pieprobe 同时在 bundle 与盘上提供。
- `build/persistent.img`：2 MiB 新格式事务盘，具有相同只读包区；请复制后作为可写盘使用，重新构建会覆盖原始构建镜像。
- `build/development.pub`、`build/application.pub`：分别用于模块与应用的开发公钥；两个私钥种子位于忽略提交的 `.keys/development.seed`、`.keys/application.seed`。
- `build/trust-policy.json`：镜像中实际生效的公开包信任策略，包含用途和撤销规则。
- `build/qemu-128.log`、`build/qemu-256.log`、`build/qemu-512.log`：串口验证记录。
- `build/test-results.json`：本次测试结果。

完整测试通过后，执行 `python tools/package_release.py` 生成 `build/SpiritFoxOS-M6-Updates.zip`，其中包含 EFI 镜像、两种测试盘、公钥、文档、测试结果与内容摘要。

重新构建保持工作区中现有的两个开发密钥。删除任一种子后会生成新的签名身份，不能自动认证旧安装。旧单密钥原型的迁移须显式启用过渡策略，步骤见 [信任策略](docs/TRUST.md)；这里没有生产密钥、Secure Boot 配置或发布签名流程。

## 启动与交互

```powershell
python tools/qemu_test.py --interactive
```

进入系统后通过串口看到 `fox>`。除 `help`、`modules`、`echo`、RAMFS 读写和 `run hello.sfk` 外，还支持：

```text
ls /disk/bin
cat /disk/etc/readme.txt
run /disk/bin/diskhello.sfk
run argprobe.sfk alpha "two words" ''
unmount /disk
stop disk
start disk
mount /disk
```

`/tmp` 等 RAMFS 文件关机后清空；`/disk` 只读，不接收写入。挂载时不能停止块驱动，打开的文件或目录会阻止卸载。缺盘、坏元数据和 I/O 错误会显示诊断并保留串口环境；坏签名应用被拒绝。串口输入有界，自动化驱动按字节节流。

默认配置使用 legacy 可信驱动模式。加上 `--iommu` 可启用受限 Intel VT-d 与 modern PCI ACCESS_PLATFORM 传输；此时 legacy 旁路设备会被拒绝，DMA 只能访问获授区域。启动日志会明确报告实际模式。尚不支持通用硬件、virtio MMIO 或 IRQ 重映射，详见 [DMA 隔离与验证边界](docs/IOMMU.md)。

持久化端到端测试：`python tools/qemu_persist_test.py`，在镜像副本上自动验证写入、包安装、重启与故障恢复。交互启动：`python tools/qemu_test.py --interactive --persistent`，首次创建 `build/user-persistent.img`，以后继续使用该副本。挂载事务盘后支持：

```text
write /persist/notes durable-note
install /disk/bin/diskhello.sfk
run installed
persist
```

成功写入与安装均等待设备 flush。`truncate /persist/notes <字节数>` 可缩短、清空或补零扩展笔记，同长度不写盘；RAMFS 普通文件也支持截断。任何写/flush 错误会停止卷的后续读写与截断，需重新挂载或重启恢复；错误时不能假定新版本一定没有落盘。当前配额为四个 16 KiB 文件，只对笔记与一个应用提供入口。

安装提交候选包，第一次运行前持久记录试运行，按时退出 0 才确认。失败或未确认时重启回退已确认包；待更新期间拒绝覆盖安装。此回退只改变应用选择器，不能撤销外部副作用；格式、旧版兼容与健康标准见 [应用更新](docs/UPDATES.md)。

已确认版本形成软件下限：拒绝降级及同版本换包，精确重装返回 `UNCHANGED` 且不写盘；`persist` 可查询实际已确认版本。下限在健康确认时才随已确认包更新，不防止离线回放旧磁盘镜像。

隔离模式交互：`python tools/qemu_test.py --interactive --persistent --iommu`；专项验证：`python tools/qemu_iommu_test.py`。诊断 `dma-test read/write` 会刻意触发设备隔离故障并使该 VM 的磁盘驱动停止至重启，请只在测试 VM 使用。

## 包工具

```powershell
python tools/sfpack.py module --manifest echo.json --image echo.elf --out echo.sfmod --seed .keys/development.seed
python tools/sfpack.py app --manifest hello.json --image hello.elf --out hello.sfk --seed .keys/application.seed
python tools/sfpack.py inspect build/hello.sfk --public-key build/application.pub
python tools/sfpack.py inspect build/hello.sfk --trust-policy build/trust-policy.json
python tools/sfpack.py trust --policy build/trust-policy.json
python tools/sfpack.py sign build/hello.sfk --seed .keys/application.seed
```

输入 ELF 是构建中间产物，交付包使用设计说明中的 96 字节头、64 字节段描述符、确定性 CBOR 和 Ed25519 尾部。当前代码只接受静态地址 native 子集；不支持资源包、动态导入、重定位或压缩。构建脚本中列出了实际清单字段。

默认策略分离模块与应用两个开发公钥；`python tools/build.py --trust-policy <工作区内JSON>` 可固化最多八个公钥，区分模块/应用用途并拒绝撤销密钥，shell 的 `trust` 可查询。应用密钥须先重叠信任、确认新密钥包，再撤销旧密钥并重建启动；在线策略更新和旧 EFI 防回放尚未实现，配置与限制见 [信任策略](docs/TRUST.md)。

目录授权示例：`runat /tmp fileprobe.sfk`、`runat /disk/etc readprobe.sfk`。只有显式选择目录且应用签名清单声明 namespace.read 时才授予只读访问；普通 run 不授予文件根。

详见 [截断与补零扩展](docs/TRUNCATE.md)、[目录能力与 openat](docs/NAMESPACE.md)、[存储协议与磁盘格式](docs/STORAGE.md)、[事务持久化](docs/PERSISTENCE.md)、[实现边界](docs/IMPLEMENTATION.md)、[安全审查与修复](docs/SECURITY.md)、[测试记录](docs/TESTING.md) 和 [后续路线](docs/ROADMAP.md)。

`run`/`runat` 可显式传递最多八个参数（含 argv[0]），支持引号、空字符串和转义。参数位于只读 BootArgs 页，解析错误不启动更新试运行；无额外参数保留空 argv 布局。SDK 和兼容边界见 [命令行参数](docs/ARGUMENTS.md)。

显式环境示例：`runenv MODE=demo "NOTE=two words" EMPTY= -- argprobe.sfk alpha`；目录委派用 `runenvat <只读目录> NAME=VALUE -- <app> [args]`。最多八条唯一合法名字，不继承宿主环境；参数与环境共同受 512 字节限制，均为私有只读调用数据。

位置无关 native 应用支持有界 `relative-v1` 相对重定位，代码/数据页保持私有且拒绝代码写修正。示例 `run pieprobe.sfk` 会在两个固定基址交替运行；这不提供安全随机化。格式、256 项上限及尚未实现的导入边界见 [native 配置](docs/NATIVE.md)。
