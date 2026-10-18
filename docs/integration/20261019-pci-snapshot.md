# 2026-10-19 对接：PCI 只读快照与显示调用方

## 基线

开发前重新 fetch upstream/origin。作者 main 仍为 414fd39，PR #2 未合并；
本次沿用 codex/devfs-services，从 e15af9b 追加，按 NEXT.md 的 PCI 任务推进。
不重复创建包含同一基础的 PR，不直接推送作者代码分支。

## 接入与协议

模块 ABI 升为 **1.3 / 0x00010003**，内核、打包器、全部模块一起重编译。
sfm_api_t 末尾增加 pci_read(index, record*)，只复制原枚举缓存；返回 1=设备、
0=末尾、负 errno=错误。这是可信 Ring 0 内的复制接口，指针不进入消息。
原 pci_init/存储初始化顺序不变。dev.pci 启动时复制最多 64 个 domain-0 设备，
再提供查询；显示初始化在其后。没有配置写、映射、IRQ、DMA 或任意端口接口。

sfmod_pci.h 定义独立协议 version=1：record=176、query=16、BAR=24 字节，
均有静态断言。记录包括版本/大小、snapshot、BDF、vendor/device、class/subclass/
prog-if/revision/header、IRQ line/pin、六个 BAR；保留字段为零。
消息头仍 64 字节，文件 route 沿用 1.2；PCI 请求 route=0、flags=PCI_VERSION。

| 消息 | 输入 | 响应 |
| --- | --- | --- |
| PCI_ENUM=0x3000 | offset=索引，handle=0，无载荷 | result=1、record、offset=索引、handle=snapshot；索引等于总数时 result=0、空载荷 |
| PCI_FIND=0x3001 | offset=起始索引，handle=0，query | 首个匹配；无匹配返回 0。下次 offset=响应索引+1，可取全部匹配 |
| PCI_GET=0x3002 | handle=snapshot，offset=索引，无载荷 | 同 ENUM；旧快照或 offset=总数返回 ENOENT，越过总数返回 EINVAL |

query.match 组合 vendor/device/class/subclass/prog-if（1/2/4/8/16），0=不筛选。
未知位、保留字段非零、错误长度、越过末尾索引或非零 route 返回 EINVAL；
版本不符 EPROTONOSUPPORT，未知操作 ENOSYS。
snapshot 是带代数的服务端点句柄，设备引用为 (snapshot,index)，不授予资源所有权。
端点销毁后旧端点调用失败；新服务也拒绝旧 snapshot。服务生命期内快照不更新，
未提供热插拔/热替换管理接口。

## BAR 与真实消费者

core/pci.c 解码原 type-0 缓存中的 IO、32/64 位 memory、prefetch、64 位上半槽。
全 1 或最后槽不完整的 64 位 BAR 标为 INVALID，非缓存头部/未支持类型为 UNKNOWN。
没有通过写全 1 探测大小；size=0 且 SIZE_KNOWN 清零表示未知，不是零长度资源。
物理地址不是可直接解引用的内核指针，也不是映射权限。

Bochs DRM 用 dev.pci FIND(display,VGA) 选择可用 BAR0，原逐总线配置读取已移除。
成功须记录 `[bochs_drm] VGA selected via dev.pci`，启动检查要求此标记和后续
Wayland 渲染都通过。无可用结果保留原固定地址回退并写不同日志，回退不能通过
正常 QEMU 验收。仍沿用原扫描器的范围和 64 设备上限；未新增 ECAM、多 domain、
桥资源或 BAR 大小探测。AHCI、网卡及显示 MMIO/VBE 数据面未迁出内核。

职责参考 [Linux PCI sysfs](https://docs.kernel.org/PCI/sysfs-pci.html) 对只读描述与
可修改资源的区分，不实现其 sysfs ABI。统一注册见
[DEVICE-REGISTRY 提案](../DEVICE-REGISTRY.md)，该提案尚未实施。

## 验证

Windows tools/dev.ps1 verify；最终提交由 pre-push 执行 verify-ref。
Linux bash tools/check-linux.sh。保留原设备、tmpfs、fork/wait、Wayland 回归。
新增宿主测试验证 BAR 解码、空/多设备/64 设备清单、旧 ABI 拒绝、旧端点，以及
原依赖/OOM 回滚。共同 PCI 自测逐字节比较每条消息记录和平台缓存，检查版本、
长度/索引、无匹配、筛选、多匹配和旧快照；内核启动运行同一协议自测。
真实 Bochs 消费者及后续图形输出另由启动检查验收。

2026-10-19 实测：初版工作区分别通过 Ubuntu GCC 13.3/NASM 2.16、独立 Arch
用户空间 GCC 16.2/NASM 3.02 的全量构建及各自产物的 Windows QEMU TCG UEFI
启动。均出现 PCI 清单对照、Bochs 通过 dev.pci 选择 BAR0、原设备/文件/fork 与
Wayland 10 帧标记，收据 build_exit_code=0、exit_code=0。随后补充的空/64 设备、
筛选及旧包边界测试在干净 Linux 导出源码上 make test-modules 通过（UBSan）。
最终提交由 pre-push 再完整构建/启动；结果与远端 CI 以对应运行记录为准。
Arch 为 Ubuntu VM 内独立 chroot，不是原生 Arch 内核；真机和长期负载未验证。
