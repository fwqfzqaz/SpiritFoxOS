# 工具

所有工具只在工作区内读写，不修改全局环境。固定版本 Zig 下载到 `.tools/`，缓存与临时文件位于 `.cache/`。

## 入口

| 脚本 | 作用 |
| --- | --- |
| `bootstrap.py` | 下载并校验固定版本 Zig、检查系统 QEMU/OVMF，准备 Monocypher。 |
| `build.py` | 构建内核、UEFI 镜像、sfmod、应用与测试盘。 |
| `test.py` | 构建并运行宿主与 QEMU 测试，写出 `build/test-results.json`。 |
| `package_release.py` | 校验测试结果与镜像摘要后生成发布包。 |
| `abi.py` | 实际编译共享头文件目录，采集固定宽度 ABI 数值，并离线检查或比较报告。 |

根目录 `Makefile` 把上述入口映射为 `make bootstrap`、`make build`、`make test`、`make release` 等目标，并透传相同参数。

`python3 tools/abi.py capture` 生成当前头文件快照，`python3 tools/abi.py check build/abi-current.json` 对照审阅参考；`diff <before> <after>` 离线报告增删改。离线比较不依赖编译器，范围见 [ABI 快照说明](../docs/architecture/ABI_SNAPSHOT.md)。

## 包工具

`package/` 保存磁盘与包格式实现，被构建、测试与发布共用：

- `sfpack.py`：SFMOD1/SFK1 打包、签名与检查。
- `manifest_profile.py`：有界无重复键 JSON 源读取与 `native-source-v1` 固定作者约定。
- `mkreadfs.py`：生成只读 SFRFS1 镜像。
- `mkpersist.py`：生成 2 MiB 双银行事务镜像。
- `crypto_host.c`：供 Python 通过 ctypes 调用的 C 加密/校验桥。

这些模块不是独立入口；从仓库根目录运行，例如：

```bash
python3 tools/package/sfpack.py inspect build/hello.sfk --public-key build/application.pub
python3 tools/package/sfpack.py manifest-schema
python3 tools/package/sfpack.py validate-manifest build/hello-source.json --kind app
```

离线 schema/源校验无需 C 共享库或签名密钥；`module` / `app` 可加 `--strict-manifest`，默认保留自由元数据。所有 JSON 打包输入都拒绝重复键、非法数字和超预算源文件。约定及独立 schema 见 [清单源文件说明](../docs/architecture/MANIFESTS.md)。

## 开发辅助

`dev/` 存放仅用于特定宿主机环境的适配脚本，不参与构建、测试或发布。
