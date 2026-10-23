# SpiritFoxOS

基于 x86_64 UEFI 的可启动微内核原型。保留提供的 EFI 引导入口，按 [设计说明 v0.2](SpiritFoxOS_设计说明_v0.2.md) 实现用户态 sfmod、模块装卸、文件对象、native 运行时，以及从只读 virtio 磁盘验证并运行应用。

当前推进到 M6 的受限实现：用户态 legacy virtio-blk、SFRFS1 只读区、双银行事务持久化与签名应用双槽激活。IOMMU、真实硬件断电保证、网络、图形与多核尚未实现；其余扩展保留在路线图中。

## 构建和测试

当前构建宿主为 Windows x64 + Python 3.11 或更新版本。所有工具、缓存、密钥和输出都位于仓库内，不需要 WSL、全局安装器或修改 PATH。

```powershell
python tools/bootstrap.py
python tools/test.py
```

第一步下载并校验 Zig 0.13.0、QEMU Windows 构建和 Monocypher 4.0.3。已提供的 Monocypher 源码在 `vendor/monocypher`。第二步执行基础服务测试、14 组 Python 测试（含 2000 次固定种子变异）、783 个事务故障场景、128/256/512 MiB 启动与存储失败、持久化重启测试。

只构建：`python tools/build.py`。只运行宿主测试：`python tools/test.py --host-only`。已有镜像测试：`python tools/test.py --skip-build`。

构建产物：

- `build/esp/EFI/BOOT/BOOTX64.EFI`：包含内核和已签名启动包的 UEFI 应用。
- `build/*.sfmod`、`build/*.sfk`：实际构建出的模块与应用。
- `build/storage.img`：1 MiB 的只读测试盘，含 `diskhello.sfk`、`disk-echo.sfmod` 和说明文件；磁盘应用与模块不在启动包中。
- `build/persistent.img`：2 MiB 新格式事务盘，具有相同只读包区；请复制后作为可写盘使用，重新构建会覆盖原始构建镜像。
- `build/development.pub`：当前开发公钥；私钥种子在忽略提交的 `.keys/development.seed`。
- `build/qemu-128.log`、`build/qemu-256.log`、`build/qemu-512.log`：串口验证记录。
- `build/test-results.json`：本次测试结果。

完整测试通过后，执行 `python tools/package_release.py` 生成 `build/SpiritFoxOS-M6.zip`，其中包含 EFI 镜像、两种测试盘、公钥、文档、测试结果与内容摘要。

重新构建会使用本工作区现有开发密钥。删除种子后会生成新密钥并重新构建信任锚；这里没有生产密钥、Secure Boot 配置或发布签名流程。

## 启动与交互

```powershell
python tools/qemu_test.py --interactive
```

进入系统后通过串口看到 `fox>`。除 `help`、`modules`、`echo`、RAMFS 读写和 `run hello.sfk` 外，还支持：

```text
ls /disk/bin
cat /disk/etc/readme.txt
run /disk/bin/diskhello.sfk
unmount /disk
stop disk
start disk
mount /disk
```

`/tmp` 等 RAMFS 文件关机后清空；`/disk` 只读，不接收写入。挂载时不能停止块驱动，打开的文件或目录会阻止卸载。缺盘、坏元数据和 I/O 错误会显示诊断并保留串口环境；坏签名应用被拒绝。串口输入有界，自动化驱动按字节节流。

该驱动使用明确授权的端口范围和固定 DMA 页；没有 IOMMU，属于可信驱动模式。当前只支持 QEMU 的 legacy/transitional virtio PCI（`disable-modern=on,vectors=0`），不宣称支持现代 virtio MMIO、真实硬件或 DMA 故障隔离。

持久化端到端测试：`python tools/qemu_persist_test.py`，在镜像副本上自动验证写入、包安装、重启与故障恢复。交互启动：`python tools/qemu_test.py --interactive --persistent`，首次创建 `build/user-persistent.img`，以后继续使用该副本。挂载事务盘后支持：

```text
write /persist/notes durable-note
install /disk/bin/diskhello.sfk
run installed
persist
```

成功写入与安装均等待设备 flush。任何写/flush 错误会停止卷的后续读写，需重新挂载或重启恢复；错误时不能假定新版本一定没有落盘。当前配额为四个 16 KiB 文件，只对笔记与一个应用提供入口。

## 包工具

```powershell
python tools/sfpack.py module --manifest echo.json --image echo.elf --out echo.sfmod --seed .keys/development.seed
python tools/sfpack.py app --manifest hello.json --image hello.elf --out hello.sfk --seed .keys/development.seed
python tools/sfpack.py inspect build/hello.sfk --public-key build/development.pub
python tools/sfpack.py sign build/hello.sfk --seed .keys/development.seed
```

输入 ELF 是构建中间产物，交付包使用设计说明中的 96 字节头、64 字节段描述符、确定性 CBOR 和 Ed25519 尾部。当前代码只接受静态地址 native 子集；不支持资源包、动态导入、重定位或压缩。构建脚本中列出了实际清单字段。

详见 [存储协议与磁盘格式](docs/STORAGE.md)、[事务持久化](docs/PERSISTENCE.md)、[实现边界](docs/IMPLEMENTATION.md)、[测试记录](docs/TESTING.md) 和 [后续路线](docs/ROADMAP.md)。
