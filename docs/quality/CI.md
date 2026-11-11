# GitHub Actions 配置与排错

仓库有两个工作流，配置文件必须位于 `.github/workflows/`，使用 UTF-8、空格缩进的 YAML。以下配置与本轮修复后的文件一致；修改工作流时同步更新本页。它们是 GitHub Actions，不是本地 Codex 的额度监测任务。

2026-11-11：通用串口 VM 等待使用 `tests/qemu/poll_deadline.py` 的轮询预算。正常每 0.1 秒轮询时，原 45/60 秒预算保持；单次宿主未获调度超过 2 秒时只扣 2 秒，避免电脑暂停后尚未消费已排队输出就直接超时。这只识别调度间隔，不据此断言系统发生了休眠，也不是性能测量。持续无应答仍会耗尽有效轮询预算，客户机故障/退出/响应内容检查保留；Windows CI 的 90 分钟绝对作业时限保持。8 组确定性宿主测试覆盖正常预算、长间隔、重复间隔仍超时、坏时钟，以及真实 VM 等待方法的排队响应和静默客户机拒绝路径；并入宿主套件和发布门。


2026-11-12：6b627d6 的两项 Linux 检查在安装系统包时持续出现 Ubuntu 仓库 DNS 解析失败，耗尽 30 分钟后取消，代码测试尚未运行。runner 固定为官方支持的 [Ubuntu 24.04 标签](https://github.com/actions/runner-images#available-images)，避免 latest 迁移时基础系统变化；安装阶段单独限时、失败不继续构建，保留原测试与 30 分钟作业门禁。APT 选项见 [apt-get](https://manpages.ubuntu.com/manpages/noble/man8/apt-get.8.html)、[apt.conf](https://manpages.ubuntu.com/manpages/noble/man5/apt.conf.5.html) 与 [HTTP 传输](https://manpages.ubuntu.com/manpages/noble/man1/apt-transport-http.1.html)。这些措施限制依赖失败的影响，实际 Linux 成功仍须看新提交的托管结果；Windows 本地通过不能代替它。

## 工具链与目标

Python 3.13、Zig 0.13.0（`tools/bootstrap.py` 下载/校验）、rustup/Cargo 和对应 Rust 标准库目标都必须可用。GitHub 托管 runner 已有 Rust；自建 runner 需要先安装 rustup。`bootstrap.py` 负责 Zig/QEMU/Monocypher，不会安装 Rust。

| 构建内容 | Rust 目标 | 说明 |
| --- | --- | --- |
| 内核及用户态 ELF | `x86_64-unknown-none` | 两个平台都需要 |
| Windows 宿主库/测试 | `x86_64-pc-windows-gnu` | 不能只装默认 MSVC 目标 |
| Linux 宿主库/测试 | `x86_64-unknown-linux-gnu` | 与可执行宿主 ABI 一致 |
| UEFI/PE-COFF | `x86_64-pc-windows-gnu` | **Linux 也需要**；`--host-only` 仍先执行完整构建 |

`tools/build.py` 总是调用 Cargo 构建到 `build/rust/`。Windows GNU 静态库另生成 `libsfcore-zig.a`：根据归档符号索引定位仅定义栈探测符号的 Rust `compiler_builtins` 成员，在副本中移除，让 Zig 的 compiler-rt 提供唯一实现。原始 Cargo 归档、其他内建函数与栈探测调用保留；意外的格式或混合成员会报错，不用允许重复符号掩盖冲突。Linux ELF 归档不做此适配。

## Windows prototype tests

源文件：[windows.yml](../../.github/workflows/windows.yml)。`push` 与 `pull_request` 均触发，90 分钟上限；执行完整构建、宿主/集成/QEMU 测试和发布门，最后无论成功失败都尝试上传已有产物。本轮本地完整回归实测 3150.79 秒（约 52.5 分钟），预算为冷工具链准备、故障恢复和打包留出余量，测试清单保持。

```yaml
name: Windows prototype tests
on: [push, pull_request]
jobs:
  prototype:
    runs-on: windows-latest
    timeout-minutes: 90
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: '3.13'
      - run: python tools/bootstrap.py
      - name: Install Rust targets
        # Both native host tests and BOOTX64.EFI use Windows GNU/COFF.
        run: rustup target add x86_64-unknown-none x86_64-pc-windows-gnu
      - run: python tools/test.py
      - run: python tools/package_release.py
      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: spiritfoxos-test-results
          path: |
            build/*.log
            build/test-results.json
            build/esp/EFI/BOOT/BOOTX64.EFI
            build/storage.img
            build/persistent.img
            build/SpiritFoxOS-M6-Updates.zip
```

## Linux host tests

源文件：[linux.yml](../../.github/workflows/linux.yml)。固定 `ubuntu-24.04`，同样在 push/PR 触发，作业上限仍为 30 分钟。安装 QEMU/OVMF 单独限时 5 分钟；APT 每个传输请求设 20 秒连接/数据等待与两次重试，`--error-on=any` 让索引更新的临时错误也失败。依赖成功后执行含初始构建的宿主/集成测试，**不执行 QEMU，也不生成发布 ZIP**。

```yaml
name: Linux host tests

on: [push, pull_request]

jobs:
  host:
    runs-on: ubuntu-24.04
    timeout-minutes: 30
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: '3.13'
      - name: Cache workspace toolchain
        uses: actions/cache@v4
        with:
          path: .tools
          key: zig-0.13.0-${{ runner.os }}
      - name: Install QEMU and OVMF
        timeout-minutes: 5
        run: |
          sudo env DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=2 -o Acquire::http::Timeout=20 -o Acquire::https::Timeout=20 --error-on=any update
          sudo env DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=2 -o Acquire::http::Timeout=20 -o Acquire::https::Timeout=20 install -y --no-install-recommends qemu-system-x86 ovmf
      - name: Bootstrap workspace toolchain
        run: python3 tools/bootstrap.py
      - name: Install Rust targets
        # host-only still builds BOOTX64.EFI, so it also needs the COFF target.
        run: rustup target add x86_64-unknown-none x86_64-unknown-linux-gnu x86_64-pc-windows-gnu
      - name: Build and run host tests
        run: python3 tools/test.py --host-only
      - name: Upload host test results
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: spiritfoxos-linux-host-results
          path: |
            build/test-results.json
            build/esp/EFI/BOOT/BOOTX64.EFI
```

## 本地等价命令

从仓库根目录执行；需要联网准备工具链，Cargo 编译本身为 `--offline`、零第三方 crate。

Windows PowerShell（每步成功后再执行下一步）：

```powershell
python tools/bootstrap.py
rustup target add x86_64-unknown-none x86_64-pc-windows-gnu
python tools/test.py
python tools/package_release.py
```

Linux Bash：

```bash
sudo apt-get update
sudo apt-get install -y qemu-system-x86 ovmf
python3 tools/bootstrap.py
rustup target add x86_64-unknown-none x86_64-unknown-linux-gnu x86_64-pc-windows-gnu
python3 tools/test.py --host-only
```

Linux 如需完整 VM 验证，改用 `python3 tools/test.py`；只有完整测试通过才运行 `python3 tools/package_release.py`。`--skip-build` 仅适合已用完全相同源码/策略构建的本地复测，不能用来绕过 CI 的初始构建。两个工作流均没有 `workflow_dispatch`：通过 push/PR 自动触发，或对相同旧提交使用 Actions 的 Re-run；旧失败提交的重跑不会包含新修复。

## 判断失败发生在哪一层

1. 打开最新提交对应的 Actions run，确认提交 SHA、事件与 attempt，展开第一个失败步骤；后续跳过的打包步骤通常不是根因。
2. `error[E0463]: can't find crate for core`：核对报错中的目标及 `rustup target list --installed`。Linux UEFI 缺 `x86_64-pc-windows-gnu` 时补上该目标，不要改成 Linux ELF 目标。
3. `duplicate symbol: ___chkstk_ms`：确认使用修复后的 `tools/build.py` 与 `tools/rust_archive.py`，并通过构建入口链接 `libsfcore-zig.a`；不要删掉栈探测调用，也不要添加全局允许重复符号的链接选项。
4. 工具链构建成功后的断言失败、ABI 漂移、VM 超时要按各自日志排查，不能通过安装目标或跳过测试宣称解决。宿主绿色不代表 QEMU 或 Windows 绿色。
5. Windows 产物 `spiritfoxos-test-results` 包含已有 `build/*.log`、测试 JSON、EFI、测试盘与发布 ZIP；Linux `spiritfoxos-linux-host-results` 只包含宿主结果与 EFI。构建早期失败时这些文件可能尚不存在，需看 Actions 步骤日志。发布门会拒绝 host-only 结果和与结果摘要不符的产物。

截图中的 Windows #90 / Linux #73 对应 `49434cf`：前者在宿主 DLL 链接时遇到 Rust/Zig 栈探测重复，后者在 UEFI 构建时缺少 Windows GNU Rust 目标。它们不是 YAML 解析失败；修复是否通过以新提交的真实检查为准，不沿用旧绿色记录。更早的 #89 / #72 是不同提交的历史失败，本次没有把它们重写为成功。

本地逐层回归还修复了中文 Windows 下账户配置默认 GBK 读取失败（改为显式 UTF-8），Core 宿主夹具遗漏 `user_phys` / `space_map_fb` 链接依赖，以及线程优先级断言在 Windows LLP64 下的有符号/无符号比较。图形映射在该 Core 夹具中属于未建模路径，意外调用会立即失败；真实映射仍由 GUI VM 测试验证，不能用夹具代替设备验证。

网络测试在启动 DHCP 已由队列轮询完成时可能尚无 IRQ 等待记录；测试服务端延迟 UDP 回包后再检查中断日志和非零计数，保留真实网络流量和 IRQ 断言。GUI 两个脚本共用 `tests/qemu/qmp.py`：Python 先绑定回环地址的随机 TCP 端口，QEMU 作为客户端连接；端口保留到连接建立，避免释放再绑定的竞争，也兼容没有 `AF_UNIX` 的 Windows Python。QMP 有超时、EOF 检查和退出清理；串口命令统一用 LF 结尾，兼容 Windows stdio 后端，图形输入、校验和及真实像素断言保留。
