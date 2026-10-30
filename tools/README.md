# 工具

所有工具只在工作区内读写，不修改全局环境。固定版本 Zig 下载到 `.tools/`，缓存与临时文件位于 `.cache/`。

## 入口

| 脚本 | 作用 |
| --- | --- |
| `bootstrap.py` | 下载并校验固定版本 Zig、检查系统 QEMU/OVMF，准备 Monocypher。 |
| `build.py` | 构建内核、UEFI 镜像、sfmod、应用与测试盘。 |
| `test.py` | 构建并运行宿主与 QEMU 测试，写出 `build/test-results.json`。 |
| `package_release.py` | 校验测试结果与镜像摘要后生成发布包。 |

根目录 `Makefile` 把上述入口映射为 `make bootstrap`、`make build`、`make test`、`make release` 等目标，并透传相同参数。

## 包工具

`package/` 保存磁盘与包格式实现，被构建、测试与发布共用：

- `sfpack.py`：SFMOD1/SFK1 打包、签名与检查。
- `mkreadfs.py`：生成只读 SFRFS1 镜像。
- `mkpersist.py`：生成 2 MiB 双银行事务镜像。
- `crypto_host.c`：供 Python 通过 ctypes 调用的 C 加密/校验桥。

这些模块不是独立入口；从仓库根目录运行，例如：

```bash
python3 tools/package/sfpack.py inspect build/hello.sfk --public-key build/application.pub
```

## 开发辅助

`dev/` 存放仅用于特定宿主机环境的适配脚本，不参与构建、测试或发布。
