# 共享头文件 ABI 快照

`tools/abi.py` 采集和比较当前 x86_64 共享头文件的接口编号及结构布局，参考为 `abi/x86_64-current.json`。首个目录有 549 项数值，来源是实际编译的 `tools/abi_snapshot.c`，不是从 C 文本猜测布局。

句柄复制新增 `OP_DUP=25` 后当前目录为 550 项。该次已审阅差异只有这一项新增，原 549 项无删改；记录见对应代码提交和 TESTING。它展示了增加操作码时显式维护目录及参考的流程。

后续定位读取新增 `OP_PREAD=26`，当前为 551 项；该轮差异仅新增此常量，前 550 项保持。共享消息/BootArgs 大小和原有编号不变。

## 采集与离线比较

在源码仓库根目录执行：

```bash
python3 tools/abi.py capture
python3 tools/abi.py check build/abi-current.json
python3 tools/abi.py diff abi/x86_64-current.json build/abi-current.json
```

`capture` 使用仓库内固定 Zig，经 `tools/build.py` 设置的工作区缓存/临时目录编译宿主探针，只执行打印元数据的程序。输出默认为 `build/abi-current.json`，可用 `--out` 指定已有父目录中的文件。它不会更新审阅参考，也不运行内核或服务。

`check` 默认与仓库参考比较；`--reference` 可选择另一份参考。`diff` 比较任意两份合法报告，输出按名称排序的 `added`、`removed`、`changed`；`changed` 含 before/after。相同布局返回 0，任何差异（包括新增项目）返回非零，留待审阅；它不自动判定新增项目向后兼容。

离线 `check` / `diff` 只需要 Python 和两份报告，不需要编译器、构建目录或签名密钥。发布 ZIP 同时提供脚本、审阅参考 `abi/x86_64-current.json` 与本次真实采集 `abi/x86_64-captured.json`。发布包中的离线脚本用于比较；重新采集需要源码和工具链。

发布 ZIP 解压后可直接执行 `python3 tools/abi.py check abi/x86_64-captured.json`。

## 采集内容

- 已登记的系统调用、操作码、状态、能力权限、地址/容量约束、模块/应用信任用途，以及作者现有网络操作码。
- 15 种固定宽度布局，包括 IPC header/message、文件元数据、DMA 状态、BootArgs/BootInfo、加载计划、参数环境、native 计划、网络消息和公钥策略记录。
- 每个结构的 size/align，每个已登记成员的 offset/width，以及标量种类。

标量种类为 1:uint8、2:int8、3:uint16、4:int16、5:uint32、6:int32、7:uint64、8:int64；0 表示数组或复合成员，需结合其总字节宽度、子结构及目录审查。它不包含密钥内容或实例内存。C11 `_Generic` 按固定宽度 typedef 分类，不把宿主 `long` 的 Windows/Linux 差异当作共享结构变化。

报告格式是 `spiritfoxos-x86_64-abi-snapshot-v1`，根只有 `format` 和 `values`。输入最多 256 KiB，1–2048 个有界名称/整数项目；重复字段、布尔/小数、非法名称、未知格式和越界整数都拒绝。工具比较时不修改输入或审阅参考。

## 维护与边界

目录是显式登记的；新增枚举或结构需扩展探针。修改字段顺序、固定宽度类型或编号时先查看差异，再审查协议版本和调用者，最后通过单独可审阅的改动更新参考。不能为让检查通过而无条件覆盖参考。

这份快照包含当前仍在演进的内部装载结构，提供漂移检测，不宣告整个 ABI 冻结。数组成员的完整元素语义、系统调用寄存器/调用约定、权限与错误语义、超时取消、包/磁盘格式和行为兼容仍按各自协议验证。布局匹配只回答所登记的数值是否相同。

## 回归与发布门

`tests/integration/test_abi_snapshot.py` 先实际采集并对照参考，再用独立 little-endian struct 格式检查关键 wire 大小。它在隔离副本中验证无编译器的离线操作，并覆盖报告边界、重复键及增删改。

三项负例在工作区临时目录内编译经过修改的真实共享头文件：系统调用中间插入新编号、BootArgs 在保持总大小下交换 pid/manager、pid 在保持大小和偏移下改为有符号类型。三者都产生真实探针输出，并被离线检查定位；正式头文件保持原状。

统一测试入口记录参考和本次采集的摘要；发布门核对测试标记与两份摘要后才把报告和离线脚本纳入 ZIP。Windows/Linux 的固定宽度布局分别由相应 CI 核对，完整托管结果以当前提交为准。
