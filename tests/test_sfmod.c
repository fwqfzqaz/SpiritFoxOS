/* Linux 宿主测试实际加载/执行模块，并检查畸形输入和回滚。 */
#define _GNU_SOURCE
#include <assert.h>
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../kernel/core/internal.h"

static size_t allocations;
static int fail_after = -1;
static int ready_messages;

void *sf_alloc(size_t size)
{
    if (!fail_after) return NULL;
    if (fail_after > 0) fail_after--;
    size_t bytes = (size + 16 + 4095) & ~(size_t)4095;
    size_t *base = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (base == MAP_FAILED) return NULL;
    base[0] = bytes;
    allocations++;
    return (uint8_t *)base + 16;
}
void sf_free(void *address)
{
    if (!address) return;
    size_t *base = (size_t *)((uint8_t *)address - 16);
    size_t bytes = base[0];
    assert(allocations);
    allocations--;
    assert(!munmap(base, bytes));
}
void sf_log(const char *text)
{
    if (!strcmp(text, "[sfmod] ready: ")) ready_messages++;
}
uint64_t sf_irq_save(void) { return 0; }
void sf_irq_restore(uint64_t value) { (void)value; }

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0 && !fseek(file, 0, SEEK_SET));
    *size = (size_t)length;
    uint8_t *data = malloc(*size);
    assert(data && fread(data, 1, *size, file) == *size);
    fclose(file);
    return data;
}

static Elf64_Shdr *section(uint8_t *elf, const char *name)
{
    Elf64_Ehdr *h = (Elf64_Ehdr *)elf;
    Elf64_Shdr *sections = (Elf64_Shdr *)(elf + h->e_shoff);
    const char *strings = (const char *)elf + sections[h->e_shstrndx].sh_offset;
    for (size_t i = 0; i < h->e_shnum; i++)
        if (!strcmp(strings + sections[i].sh_name, name)) return &sections[i];
    return NULL;
}

static void test_elf(const uint8_t *original, size_t size)
{
    sfm_image_t image;
    assert(!sfm_elf_load(original, size, &image));
    sfm_elf_free(&image);
    assert(!allocations);
    for (size_t truncated = 0; truncated < size; truncated++)
    {
        assert(sfm_elf_load(original, truncated, &image));
        assert(!allocations);
    }
    uint8_t *data = malloc(size);
    assert(data);
    memcpy(data, original, size);
    ((Elf64_Ehdr *)data)->e_shoff = UINT64_MAX;
    assert(sfm_elf_load(data, size, &image));
    memcpy(data, original, size);
    section(data, ".text")->sh_size = UINT64_MAX;
    assert(sfm_elf_load(data, size, &image));
    memcpy(data, original, size);
    section(data, ".text")->sh_addralign = 3;
    assert(sfm_elf_load(data, size, &image));
    memcpy(data, original, size);
    section(data, ".text")->sh_type = SHT_NOTE;
    assert(sfm_elf_load(data, size, &image));
    memcpy(data, original, size);
    Elf64_Shdr *note = section(data, ".note.gnu.property");
    if (note) {
        assert(note->sh_type == SHT_NOTE && !(note->sh_flags & (SHF_WRITE | SHF_EXECINSTR)));
        assert(!sfm_elf_load(data, size, &image));
        sfm_elf_free(&image);
    }
    memcpy(data, original, size);
    Elf64_Shdr *rel = section(data, ".rela.sfmod_init");
    assert(rel && rel->sh_size);
    Elf64_Rela *r = (Elf64_Rela *)(data + rel->sh_offset);
    r->r_info = ELF64_R_INFO(ELF64_R_SYM(r->r_info), 255);
    assert(sfm_elf_load(data, size, &image));
    memcpy(data, original, size);
    rel = section(data, ".rela.sfmod_init");
    r = (Elf64_Rela *)(data + rel->sh_offset);
    r->r_offset = UINT64_MAX;
    assert(sfm_elf_load(data, size, &image));
    memcpy(data, original, size);
    sfm_metadata_t *meta = (sfm_metadata_t *)(data + section(data, ".sfmod_meta")->sh_offset);
    meta->permissions = 1;
    assert(sfm_elf_load(data, size, &image));
    assert(!allocations);
    free(data);
}

typedef struct item { char name[32]; uint64_t offset, size; } item_t;

static void test_package(uint8_t *package, size_t size, uint8_t *failure, size_t fail_size)
{
    uint8_t *copy = malloc(size);
    assert(copy);
    for (size_t truncated = 0; truncated < size; truncated++)
        assert(sfm_package_load(package, truncated));
    memcpy(copy, package, size);
    item_t *items = (item_t *)(copy + 16);
    items[0].offset = 0;
    assert(sfm_package_load(copy, size) && !allocations);
    memcpy(copy, package, size);
    items[1].offset = items[0].offset;
    assert(sfm_package_load(copy, size) && !allocations);
    memcpy(copy, package, size);
    uint8_t *vfs = copy + items[0].offset;
    Elf64_Shdr *deps = section(vfs, ".sfmod_deps");
    assert(deps);
    memset(vfs + deps->sh_offset, 0, 32);
    memcpy(vfs + deps->sh_offset, "missing.tmpfs", sizeof("missing.tmpfs"));
    assert(sfm_package_load(copy, size) == SFM_ENOENT && !allocations && !ready_messages);
    memcpy(vfs + deps->sh_offset, "fs.vfs", sizeof("fs.vfs"));
    assert(sfm_package_load(copy, size) == SFM_EINVAL && !allocations && !ready_messages);

    /* 一个真实模块在分配资源后返回失败，依赖也必须按逆序退出。 */
    item_t *original = (item_t *)(package + 16);
    item_t tmp = original[2];
    size_t broken_size = 16 + 2 * sizeof(item_t) + tmp.size + fail_size;
    uint8_t *broken = calloc(1, broken_size);
    assert(broken && broken_size < UINT32_MAX);
    uint32_t header[4] = {0x4b504653u, SFM_ABI, 2, (uint32_t)broken_size};
    memcpy(broken, header, 16);
    item_t pair[2] = {{"test.fail", 16 + 2 * sizeof(item_t), fail_size},
                      {"fs.tmpfs", 16 + 2 * sizeof(item_t) + fail_size, tmp.size}};
    memcpy(broken + 16, pair, sizeof(pair));
    memcpy(broken + pair[0].offset, failure, fail_size);
    memcpy(broken + pair[1].offset, package + tmp.offset, tmp.size);
    assert(sfm_package_load(broken, broken_size) == -5 && !allocations);
    assert(!sfm_lookup("fs.tmpfs") && !sfm_lookup("test.fail"));
    free(broken);

    /* 每个分配点依次注入 OOM，验证可重复加载且没有残留端点/内存。 */
    int succeeded = 0;
    for (int budget = 0; budget < 128; budget++)
    {
        fail_after = budget;
        int result = sfm_package_load(package, size);
        fail_after = -1;
        if (!result) { succeeded = 1; break; }
        assert(!allocations && !sfm_lookup("fs.vfs") && !sfm_lookup("fs.tmpfs"));
    }
    assert(succeeded);
    assert(!sfm_selftest());
    assert(sfm_package_load(package, size) == SFM_EBUSY);
    assert(sfm_module_unload("fs.tmpfs") == SFM_EBUSY);
    assert(!sfm_module_unload("fs.vfs"));
    assert(!sfm_module_unload("fs.tmpfs"));
    assert(!sfm_module_unload("demo.hello"));
    assert(!allocations && !sfm_lookup("fs.vfs"));
    free(copy);
}

int main(int argc, char **argv)
{
    assert(argc >= 4);
    size_t size, fail_size;
    uint8_t *package = read_file(argv[1], &size);
    uint8_t *failure = read_file(argv[2], &fail_size);
    for (int i = 3; i < argc; i++)
    {
        size_t length;
        uint8_t *elf = read_file(argv[i], &length);
        test_elf(elf, length);
        free(elf);
    }
    test_package(package, size, failure, fail_size);
    free(package);
    free(failure);
    puts("sfmod tests passed: malformed ELF/package, dependencies, init rollback, OOM, IPC, file IO, unload");
    return 0;
}
