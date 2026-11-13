#ifndef SF_LINUX_IMAGE_H
#define SF_LINUX_IMAGE_H
#include "sf.h"
/* S1 有界 Linux ELF 装载核心：只做用户态解析与计划构造，不修改内核。
 * 输入必须是已通过包签名认证的只读快照；本模块不解析路径、不访问磁盘。 */
#define SF_LINUX_SEGMENT_MAX 8u
#define SF_LINUX_RAW_SEGMENT_MAX 32u
#define SF_LINUX_ARG_MAX 16u
#define SF_LINUX_ENV_MAX 16u
#define SF_LINUX_STRING_MAX 1024u
#define SF_LINUX_RELOCATION_MAX 4096u
#define SF_LINUX_AUX_MAX 16u
/* 仅改写 RX 段副本中的 0F 05；供 S2 兼容入口使用。 */
#define SF_LINUX_FLAG_REWRITE_SYSCALLS 1u
typedef struct {
 uint64_t entry_rva,image_bytes,phdr_rva;
 uint32_t phnum,relocations,syscalls,flags;
} SfLinuxInfo;
typedef struct {
 const char *const *argv; uint32_t argc;
 const char *const *env; uint32_t envc;
 uint32_t uid,gid;
 const uint8_t *random16;
} SfLinuxStackRequest;
typedef struct { uint64_t sp,argc,argv,envp,auxv,bytes; } SfLinuxStack;
/* 校验 ET_DYN 静态子集并填写只读元数据；不写任何输出缓冲区。 */
int sf_linux_elf_validate(const uint8_t *image,size_t bytes,SfLinuxInfo *info);
/* 把合法映像复制到 staging（按合并段顺序页对齐、零填充），应用相对重定位，
 * 可选改写系统调用指令，最终填写既有 SfLoadPlan（入口为 RVA）。 */
int sf_linux_elf_load(const uint8_t *image,size_t bytes,uint64_t base,
                      void *staging,size_t capacity,SfLoadPlan *plan,
                      SfLinuxInfo *info,unsigned flags);
/* 在 buffer 中构造 SysV 初始栈；stack_top 为栈顶高地址（不含），
 * 返回的 sp 相对 stack_top 满足 16 字节对齐。buffer 可小于栈窗口。 */
int sf_linux_stack_init(void *buffer,size_t capacity,uint64_t stack_top,
                        uint64_t image_base,const SfLinuxInfo *info,
                        const SfLinuxStackRequest *request,SfLinuxStack *out);
#endif
