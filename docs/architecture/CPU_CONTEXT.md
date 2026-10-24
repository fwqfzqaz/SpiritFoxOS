# CPU 上下文边界

Core 0 的单核任务现在拥有私有、16 字节对齐的 512 字节 FXSAVE64 状态。该状态是内核内存，不接受用户提供的保存/恢复指针；进程仍为单线程。

## 已复现的问题

在 c565c40 的原镜像中，两个正确签名的普通应用先后运行：第一个向 XMM0 写入哨兵并 yield/退出，第二个读出相同值。legacy 和 VT-d/modern 两个实际 VM 均复现，见 build/cpu-state-baseline.json 及对应串口日志。六项原发布摘要和磁盘字节保持，测试没有修改生产镜像或增加特权接口。此为进程间寄存器数据泄漏，不是仅根据缺少代码作出的推断。

## 实现

初始化要求 CPUID 的 x87、FXSR、SSE 和 SSE2；CR0 保留 WP，设置 MP/NE，清除 EM/TS。CR4 设置 OSFXSR/OSXMMEXCPT，明确清除 OSXSAVE；EFER 清除 FFXSR，确保 CPL0 不省略 XMM 的保存/恢复。只支持 x87、MMX、XMM0–15 和 MXCSR；AVX/AVX-512、XSAVE 扩展、AMX 和完整 TLS 不在当前配置内。CPU 宣告 AVX 硬件不代表本操作系统启用了 AVX。

每次从用户态进入中断/系统调用后，在可能结束或切换任务前以 FXSAVE64 保存当前任务。调度选中下一任务、更新 CR3 后，先以无等待 FNINIT 和固定零指针 FLDENV 环境清理旧状态，再以 FXRSTOR64 恢复其私有状态；初次进入同样恢复。内核 C 继续禁用编译器 SSE/MMX，内核 IRQ 路径不使用浮点，保存操作不等待未屏蔽的 x87 异常。用户的待处理异常由其后续用户指令触发，不能在其他任务中继承。

创建或复用任务槽时清空整个状态，显式写入 x87 控制字 0x037f 和 MXCSR 0x1f80。物理 x87 数据、XMM、状态/标签、指令/数据指针均从零开始。仅执行 FNINIT 不会清除 x87 数据寄存器，也不会初始化 XMM/MXCSR，因此不能单独用来隔离新进程。上下文从用户硬件状态保存，恢复时不存在用户可写的内核状态映像。

## 验证范围

最终产品完整回归通过（1074.85秒、143VM），日志build/kernel-cpu-isolation-final-test.txt，全部既有回归与六项摘要一致。tests/host/test_task_pid.c 在实际 task_new 路径增加一千次带脏状态的 DEAD 槽复用，检查完整初始映像及对齐，新增 3000 项断言。services/fpprobe.c 检查初始物理寄存器、指针与控制值；按 PID 生成不同的 x87/MMX/XMM0–15 和舍入控制，经过 yield、系统调用与不主动让出的 TSC 循环后检查保持。

cpu-state-test 诊断会启动两个实际同时存活的应用。专项 VM 使用宣告 AVX/XSAVE 的 max CPU，同时要求 OSXSAVE 未启用，验证 AVX、x87 和跨 yield 的待处理 x87 异常仅终止原应用，以及超时终止后槽复用的清零。该工具用于当前 QEMU 平台回归，不代表所有物理 CPU、微架构侧信道或多核上下文已经验证。

部分 CPU/模拟器对 ES=0 的异常指针采用省略行为，单独 FXRSTOR 不能作为新任务指针清理手段。零 FLDENV 不执行会记录内核操作数地址的浮点运算；探针以 FNSTENV 再检查新任务的实际指令/数据指针。当前 TCG 专项不能证明非零 FOP/FIP/FDP 的完整恢复，不能据此宣称覆盖所有物理 CPU 的异常指针语义。

机制参考：[Intel SDM Volume 2A，FINIT/FNINIT、FXSAVE、FXRSTOR](https://cdrdv2.intel.com/v1/dl/getContent/671199)；FINIT/FNINIT 说明物理数据不变，FXSAVE64 表规定 512 字节布局和 16 字节对齐，FXRSTOR64 恢复 64 位指针与全部 XMM。更广的硬件支持须继续独立验证。

实际专项迭代中，随仓库提供的 QEMU 11.0.92 TCG 对未屏蔽的 SSE 0/0 设置 MXCSR.IE，未交付 #XM。测试区分真实 vector 19 与 status-only 两条结果，将各传输观察写入 fpu-context-results.json 和总结果；status-only 路径额外检查 0x1f01 跨 yield 保持，不将其记为已通过 SIMD 异常交付。Core 已启用 OSXMMEXCPT 并沿用一般用户异常隔离入口，真实硬件 #XM 仍待独立验证。

FFXSR 依据 [AMD64 System Programming，EFER bit 14](https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/24593.pdf)，其启用时 CPL0 的 FXSAVE/FXRSTOR 会省略 XMM。当前明确关闭这项优化。
