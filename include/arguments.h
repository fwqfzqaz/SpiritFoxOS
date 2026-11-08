#ifndef SF_ARGUMENTS_H
#define SF_ARGUMENTS_H
#include "sf.h"
/* 应用启动参数校验、打包与拆分（C 实现部分）。
 * 打包/拆分三函数已由 Rust 实现（rust/sfcore/src/argparse.rs），符号同名：
 * 见 docs/architecture/LANGUAGE.md 第 4 节 FFI 约定。 */
/* 校验 SfArguments 自身格式（数量、字节数与字符串区布局）。 */
int sf_arguments_validate(const SfArguments *);
/* 仅打包位置参数（等价于 env_count=0 的 sf_invocation_pack，Rust 实现）。 */
int sf_arguments_pack(SfArguments *,const char *const *,unsigned);
/* 打包 argv+env 为 SfArguments（Rust 实现）；偏移表以 SF_BOOT_ARGS_SIZE 为基准。 */
int sf_invocation_pack(SfArguments *,const char *const *,unsigned,const char *const *,unsigned);
/* 就地拆分命令行（Rust 实现）；支持单双引号与反斜杠转义。 */
int sf_arguments_split(char *,char **,unsigned);
/* 校验 SfBootArgs 头部与参数区布局（argc/envc 与偏移互相自洽）。 */
int sf_boot_arguments_validate(const SfBootArgs *,size_t);
/* 读取第 index 个位置参数；越界或未校验时返回 0。 */
const char *sf_argument(const SfBootArgs *,size_t,unsigned);
/* 读取第 index 个环境变量条目（NAME=VALUE 原文）；越界或未校验时返回 0。 */
const char *sf_environment(const SfBootArgs *,size_t,unsigned);
/* 按名字查找环境变量值；名字必须已通过 sf_environment_name_valid。 */
const char *sf_environment_value(const SfBootArgs *,size_t,const char *);
/* 环境变量名合法性：[A-Za-z_][A-Za-z0-9_]*，1–63 字节（Rust 侧有等价实现）。 */
int sf_environment_name_valid(const char *,size_t);
#endif
