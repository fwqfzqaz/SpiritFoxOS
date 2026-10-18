# Linux 与 Windows 协作开发

项目在 Linux x86_64 中构建。Windows 开发者使用 Linux 编译虚拟机，避免把 Windows 的路径、权限和宿主库带入目标系统。

## Linux：Arch、Ubuntu、Debian

```bash
# 安装本发行版依赖；Arch 会完整升级系统，先自行确认升级窗口。
bash tools/install-deps.sh

# 构建固定版本的 musl、libffi、Wayland，生成协议头。
bash tools/bootstrap-deps.sh

# 源码检查、完整编译、ELF 检查和限时 UEFI 启动检查。
bash tools/check-linux.sh

# 当前克隆启用推送前检查。
bash tools/enable-hooks.sh

# 交互运行。
make run
```

Ubuntu 需要 Pillow，安装包为 `python3-pil`；Arch 对应 `python-pillow`。构建脚本会优先使用可用的 `x86_64-linux-gnu-gcc`，否则使用原生 GCC。显式设置的 `CROSS` 仍然有效；原生编译需要 x86_64 Linux。

OVMF 会匹配 Ubuntu 的 `OVMF_CODE[_4M].fd` 和 Arch 的 `OVMF_CODE.4m.fd`。自定义时必须提供同一固件版本和容量的 CODE/VARS：

```bash
make run OVMF_CODE=/path/to/OVMF_CODE.fd OVMF_VARS=/path/to/OVMF_VARS.fd
```

## Windows：本机入口

本机 `.spiritfox-local.json` 配置便携工具目录和 Python 解释器路径；该文件被 Git 忽略。编译虚拟机及其密钥、镜像放在仓库之外。

在仓库根目录的 PowerShell 执行：

```powershell
.\tools\dev.ps1 start
.\tools\dev.ps1 verify
.\tools\dev.ps1 build
.\tools\dev.ps1 run
.\tools\dev.ps1 exec 'gcc --version'
.\tools\dev.ps1 stop
```

本机 `verify` 将工作区源码连同 Git 中记录的可执行权限导入干净目录，分别在 Ubuntu 和 Arch 用户空间中编译，再用便携 Windows QEMU 检查各自的启动镜像，避免双层模拟。产物与日志分别复制回 `build/linux/` 和 `build/arch/`。Arch 验证使用 Ubuntu 虚拟机内的独立 Arch 根文件系统，不等于验证 Arch 内核本身。

`run` 用便携 Windows QEMU 打开已编译的系统；不接触真实磁盘。没有启用硬件虚拟化时使用 TCG，速度会较慢。

`verify-ref <commit>` 导出指定提交，不包含工作区未提交修改。这也是 Windows 推送钩子的验证方式。

## 依赖和产物

- `tools/deps.lock` 固定第三方源码版本、URL 和 SHA256，校验失败即停止。
- 默认依赖安装到 `build/sysroot/`，不覆盖仓库内已有的 `sysroot/usr/include/`。
- `SPIRITFOX_SYSROOT` 可改为外部持久化工具链目录；`SPIRITFOX_DOWNLOAD_CACHE` 可复用已校验的源码压缩包。
- musl 的 specs 含构建前缀，移动工具链后需要重新构建，不能直接复制旧 specs。
- 项目自带 libc 与外部 musl 分别构建，目标 Wayland/libffi 链接 musl，不能误用宿主 glibc 库。

## 每次推送

1. 同步远端并处理冲突，改动集中在当前任务范围。
2. 检查 `git diff --check`，确认没有个人配置、镜像或生成库。
3. 提交需要交付的源码；新增脚本使用 `git add --chmod=+x` 保留可执行位。
4. 推送钩子对本次推送的每个分支或标签目标提交执行 Linux 编译和启动检查；失败会阻止推送。
5. GitHub Actions 对 Ubuntu 24.04 和 Arch 容器分别验证，保存日志。工作流文件需要提交并推送后才会在 GitHub 生效。

钩子仅对已安装它的克隆生效，可以被人为绕过；它不代替服务器端保护规则。不能保证任意未来改动都无问题，现有检查覆盖源码格式、依赖重建、编译链接及 UEFI 启动。实际应用行为、真机驱动和功能回归需要各自的测试。

`run_tests.py`、`run_tests.sh` 和 `demo.py` 仍包含旧 ISO 启动流程，不作为当前 UEFI 构建的验证入口；使用 `tools/check-linux.sh` 和 `tools/smoke-test.py`。

## 当前验证边界

启动检查要求模块加载和消息文件自测、`/modules` VFS 桥与用户态文件读写测试通过，
16 次 fork/exit/wait 压力测试、原 Shell/autorun 完成，Wayland 创建窗口并至少渲染 10 帧，随后继续观察 3 秒。
内核/用户异常、明确应用失败或超时都会失败。新增 core 和模块按 `-Werror` 编译，
旧代码仍有现存警告，尚未做到整个仓库零警告。

这不代表网络、真机、BIOS、全部系统调用或长时间图形负载已经验证。模块当前是
受信任的内核态插件，同步调用是内核内 RPC；独立地址空间和驱动崩溃隔离尚未实现。

模块开发和迁移边界见 [架构设计](ARCHITECTURE.md) 与 [ABI/接口](SFMOD-ABI.md)。
单独执行 `make test-modules` 可运行带 UBSan 的宿主加载器/消息测试。
交互 shell 可用 `cat /modules/README` 验证模块后端文件。
