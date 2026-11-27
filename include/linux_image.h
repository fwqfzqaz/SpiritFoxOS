#ifndef SF_LINUX_IMAGE_H
#define SF_LINUX_IMAGE_H
#include "sf.h"
/* S1/S5 有界 Linux ELF 装载核心：只做用户态解析与计划构造，不修改内核。
 * 输入必须是已通过包签名认证的只读快照；本模块不解析路径、不访问磁盘。
 * S5 扩展为多对象装载：PT_INTERP/DT_NEEDED、符号解析、GLOB_DAT/JUMP_SLOT/
 * TPOFF64/DTPMOD64/DTPOFF64/COPY 重定位与合并 TLS；不运行 ld.so。 */
#define SF_LINUX_SEGMENT_MAX 16u
#define SF_LINUX_OBJECT_MAX 8u
#define SF_LINUX_RAW_SEGMENT_MAX 32u
#define SF_LINUX_NEEDED_MAX 8u
#define SF_LINUX_ARG_MAX 16u
#define SF_LINUX_ENV_MAX 16u
#define SF_LINUX_STRING_MAX 1024u
#define SF_LINUX_RELOCATION_MAX 8192u
#define SF_LINUX_SYMBOL_MAX 8192u
#define SF_LINUX_AUX_MAX 16u
/* 仅改写 RX 段副本中的 0F 05；S5 musl 已在编译期改用 int 0x80，无需置位。 */
#define SF_LINUX_FLAG_REWRITE_SYSCALLS 1u
/* info.flags */
#define SF_LINUX_INFO_INTERP 1u
#define SF_LINUX_INFO_DYNAMIC 2u
#define SF_LINUX_INFO_TLS 4u
typedef struct {
 uint64_t entry_rva,image_bytes,phdr_rva;
 uint32_t phnum,relocations,syscalls,flags;
 uint32_t object_count,tls_size,tls_align;
} SfLinuxInfo;
/* 对象集合：objects[0] 为主程序，其余按加载与符号作用域顺序给出。
 * name 为调用方期望名（可空）；摘要非空时必须与映像 SHA-256 一致。 */
typedef struct {
 const char *name;
 const uint8_t *image;
 size_t bytes;
 const uint8_t *digest32;
} SfLinuxObject;
typedef struct {
 const char *const *argv; uint32_t argc;
 const char *const *env; uint32_t envc;
 uint32_t uid,gid;
 const uint8_t *random16;
} SfLinuxStackRequest;
typedef struct { uint64_t sp,argc,argv,envp,auxv,bytes; } SfLinuxStack;
/* 校验单个 ET_DYN；PT_INTERP/PT_TLS 接受，缺失 DT_NEEDED 依赖返回 SF_DEPENDENCY。 */
int sf_linux_elf_validate(const uint8_t *image,size_t bytes,SfLinuxInfo *info);
/* 单对象便捷入口，等价于只含主程序的多对象装载。 */
int sf_linux_elf_load(const uint8_t *image,size_t bytes,uint64_t base,
                      void *staging,size_t capacity,SfLoadPlan *plan,
                      SfLinuxInfo *info,unsigned flags);
/* 多对象装载：解析依赖、解析符号、应用重定位、合并 TLS 并填写 SfLoadPlan。 */
int sf_linux_image_load(const SfLinuxObject *objects,unsigned count,uint64_t base,
                        void *staging,size_t capacity,SfLoadPlan *plan,
                        SfLinuxInfo *info,unsigned flags);
/* 在 buffer 中构造 SysV 初始栈；stack_top 为栈顶高地址（不含），
 * 返回的 sp 相对 stack_top 满足 16 字节对齐。buffer 可小于栈窗口。 */
int sf_linux_stack_init(void *buffer,size_t capacity,uint64_t stack_top,
                        uint64_t image_base,const SfLinuxInfo *info,
                        const SfLinuxStackRequest *request,SfLinuxStack *out);
#endif
