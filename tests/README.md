# 测试

测试按运行方式分为三类，统一入口是 `python3 tools/test.py`。

| 目录 | 类型 | 说明 |
| --- | --- | --- |
| `host/` | C 宿主测试 | 直接编译并链接真实内核/服务实现，覆盖边界、损坏与故障注入。 |
| `integration/` | Python 集成测试 | 包格式、CBOR、签名锚点、SFRFS1 磁盘格式与变异测试。 |
| `qemu/` | QEMU 端到端测试 | 真实 UEFI/ring-3 启动，覆盖存储、持久化、IOMMU、目录能力、截断、更新、信任撤销与 legacy virtio-net 网络。 |

## 运行

```bash
python3 tools/test.py              # 构建 + 宿主 + 全部 QEMU
python3 tools/test.py --host-only  # 仅构建与宿主测试
python3 tools/test.py --skip-build # 复用已有构建镜像
```

对应的 Make 目标为 `make test`、`make test-host`、`make test-reuse`。

`python3 tests/integration/test_manifest_profile.py` 专项检查 JSON 源预算、重复键和独立作者 schema，并以真实 ELF / C 验证器测试 CLI 打包。schema/清单离线命令还在无共享库、无密钥的隔离副本中验证；完整专项的真实打包部分需要先构建。

`python3 tests/integration/test_abi_snapshot.py` 实际编译共享头文件探针，对照审阅参考和独立 wire 大小；另在临时头文件副本中验证操作码漂移、同大小字段交换和有符号性改变都能检测。正式头文件保持不变。

单独运行某个 QEMU 场景：

```bash
python3 tests/qemu/qemu_test.py --interactive
python3 tests/qemu/qemu_persist_test.py
python3 tests/qemu/qemu_net_test.py
python3 tests/qemu/qemu_trust_test.py
```

也可以用 `make run`、`make run-persist`、`make qemu-persist`、`make qemu-trust`；`make qemu-list` 列出全部单场景目标。

QEMU 场景共享 `tests/qemu/qemu_test.py` 中的固件探测、启动参数和工作区临时目录。Windows 使用工作区 `.tools/qemu`，Linux 使用系统 `qemu-system-x86_64` 与 OVMF。测试产物和串口日志写入被忽略提交的 `build/`。
