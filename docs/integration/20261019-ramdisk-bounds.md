# RAMDISK 连续后端与边界修复对接

日期：2026-10-19，Asia/Shanghai。基于 da2ae58 的块设备清单继续。
作者 main=414fd39；沿用个人 Fork 和 PR #2，无作者分支写入或合并。

## 问题与修复

审计实际 ramdisk.c 确认：旧实现逐页 alloc_page，却从第一页按连续地址访问；
只清零第一页；分配失败可能改小 sectors 后仍使用原字节范围；注册失败不回收已分配页。
旧 lba+count 检查还存在无符号回绕路径。以上是代码核查结果，不等于已在真机复现。

现改为一次 alloc_pages 请求实际连续区间，完整清零所有分配页；非零容量、字节乘积
和页舍入必须先通过 size_t 边界检查。OOM 原子失败，不缩小已请求容量，不登记半成品。
块设备表满导致登记失败时逐页释放该连续区间，并清空私有槽，可重新创建。

RAMDISK 读写使用 lba<=sectors 且 count<=sectors-lba，避免加法回绕；非零读写要求
非空缓冲区，零长度在合法 EOF 可以返回成功，不调用 memcpy。错误不访问或改变数据。
借用 initrd 缓冲区须非空、至少一个完整扇区、地址范围不回绕；只暴露完整扇区，
尾部不足 512 字节不作为扇区。该缓冲仍由调用者保证有效/生命周期，登记失败不释放它。

没有改动页分配算法、AHCI DMA/IRQ、块清单协议、模块 API 或包版本；ABI 仍为 1.4。
RAMDISK 创建/初始化仍是串行启动路径，没有新增热销毁或运行期并发管理接口。
AHCI 的容量探测来源与默认值问题仍由 GEOMETRY_UNVERIFIED 表示，未在本轮补全。

## 验证

新增 make test-ramdisk，将实际生产 ramdisk.c 和 blkdev.c 链接到带毒化分配器的
宿主程序，启用 AddressSanitizer+UBSan。分配器提供连续区间并跟踪每页回收；
测试完整清零三页（含末页填充）、跨页与最后扇区读写、最大 LBA 回绕、越界/空指针/
零长、容量舍入溢出、OOM、表满后全部回收及重新创建、借用缓冲区不释放和尾字节保护。
不是对真实物理碎片或硬件 DMA 故障的注入测试。

tools/check-linux.sh 同时执行原模块测试与新 RAMDISK 测试，之后完整构建/UEFI；
Windows tools/dev.ps1 verify，最终提交由 pre-push 的 verify-ref 再检查。
保留实际两 SATA 盘/FAT32、早期 PCI/AHCI 顺序、冻结块清单、统一 /dev、null/zero、
tmpfs、PID、fork/wait、Wayland 检查。

2026-10-19 工作区 RAMDISK 主体已通过 Ubuntu GCC 13.3/NASM 2.16、独立 Arch chroot
GCC 16.2/NASM 3.02 全量构建及各自产物 Windows QEMU TCG UEFI 启动。
两套 ASan/UBSan RAMDISK 宿主测试通过，verify 回执 build_exit_code=0、exit_code=0。
追加默认 ram0 成功创建的必需标记；最终提交由 pre-push 全量检查上述变化及下述标记修复。

## CI 发现的启动标记问题

da2ae58 的远端 Ubuntu 成功、Arch 在 120 秒超时：
https://github.com/fwqfzqaz/SpiritFoxOS/actions/runs/39215552876 。
下载串口日志逐项比对，除 `[autorun] done,` 外所有既有必需标记齐全，包含 Wayland
frame 10；无已知异常标记。实际文本为 `[autoru[sched] ...`，切换回来才打印
`n] done, executed 2 commands`，说明计时器/抢占输出插断了这条标记。

autorun_execute 现保存中断状态、关中断输出完整串口里程碑，再恢复原状态。
这不提供跨 CPU 串口互斥；CI/QEMU 的单 CPU 启动标记不再被本 CPU 时钟抢占插断。
没有放宽 required/bad 条件或延长超时。最终快照与远端结果见 PR/最终交付记录。
下一步见 NEXT.md，尚未开放扇区消息 READ。
