# SpiritFoxOS

基于 x86_64 UEFI 的可启动微内核原型。保留提供的 EFI 引导入口，按 [设计说明 v0.2](SpiritFoxOS_设计说明_v0.2.md) 建立第一轮系统闭环：用户态 sfmod、模块装卸、内存文件对象、native 运行时与 `hello.sfk`。

当前是开发原型。设计文档的 M5–M7（磁盘驱动、持久化、网络、图形、多核）尚未实现；M1–M4 采用下述受限配置，也不代表设计文档全部验收条款已经完成。

## 构建和测试

当前构建宿主为 Windows x64 + Python 3.11 或更新版本。所有工具、缓存、密钥和输出都位于仓库内，不需要 WSL、全局安装器或修改 PATH。

```powershell
python tools/bootstrap.py
python tools/test.py
```

第一步下载并校验 Zig 0.13.0、QEMU Windows 构建和 Monocypher 4.0.3。已提供的 Monocypher 源码在 `vendor/monocypher`。第二步编译镜像、执行宿主测试，并在 128/256/512 MiB 的真实 QEMU 虚拟机中检查启动和串口命令。

只构建：`python tools/build.py`。只运行宿主测试：`python tools/test.py --host-only`。已有镜像测试：`python tools/test.py --skip-build`。

构建产物：

- `build/esp/EFI/BOOT/BOOTX64.EFI`：包含内核和已签名启动包的 UEFI 应用。
- `build/*.sfmod`、`build/*.sfk`：实际构建出的模块与应用。
- `build/development.pub`：当前开发公钥；私钥种子在忽略提交的 `.keys/development.seed`。
- `build/qemu-128.log`、`build/qemu-256.log`、`build/qemu-512.log`：串口验证记录。
- `build/test-results.json`：本次测试结果。

重新构建会使用本工作区现有开发密钥。删除种子后会生成新密钥并重新构建信任锚；这里没有生产密钥、Secure Boot 配置或发布签名流程。

## 启动与交互

```powershell
python tools/qemu_test.py --interactive
```

进入系统后通过串口看到 `fox>`。支持 `help`、`modules`、`echo`、`run hello.sfk`、`stop echo`、`start echo`、`ls /tmp`、`cat /tmp/notes`、`write /tmp/demo hello`、`selftest` 和 `shutdown`。

文件系统保存在 RAM，关机后清空。串口输入有界；自动化驱动按字节节流，超过输入队列会报告溢出并丢弃该行。

## 包工具

```powershell
python tools/sfpack.py module --manifest echo.json --image echo.elf --out echo.sfmod --seed .keys/development.seed
python tools/sfpack.py app --manifest hello.json --image hello.elf --out hello.sfk --seed .keys/development.seed
python tools/sfpack.py inspect build/hello.sfk --public-key build/development.pub
python tools/sfpack.py sign build/hello.sfk --seed .keys/development.seed
```

输入 ELF 是构建中间产物，交付包使用设计说明中的 96 字节头、64 字节段描述符、确定性 CBOR 和 Ed25519 尾部。当前代码只接受静态地址 native 子集；不支持资源包、动态导入、重定位或压缩。构建脚本中列出了实际清单字段。

详见 [实现边界](docs/IMPLEMENTATION.md)、[测试记录](docs/TESTING.md) 和 [后续路线](docs/ROADMAP.md)。
