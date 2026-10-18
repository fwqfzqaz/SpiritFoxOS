/* Linux 宿主测试实际加载/执行模块，并检查畸形输入和回滚。 */
#define _GNU_SOURCE
#include <assert.h>
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../kernel/core/internal.h"
#include "../kernel/core/pci.h"

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
static uint32_t pci_fixture_count = 3;
int sf_pci_read(uint32_t index, sfm_pci_record_t *out)
{
    if (!out || index > SFM_PCI_MAX) return SFM_EINVAL;
    if (index >= pci_fixture_count) return 0;
    pci_device_t device = {.bus = (uint8_t)(index / 32), .device = (uint8_t)(index % 32),
        .vendor_id = 0x1234, .device_id = (uint16_t)(0x1111 + index),
        .class_code = 3, .bars = {0x80000008, 0xc001, 0x2000000c, 1}};
    int result = sfm_pci_describe(&device, out);
    return result ? result : 1;
}

static void test_pci_decode(void)
{
    pci_device_t device = {.vendor_id = 0x1234,
        .bars = {0x80000008, 0xc001, 0x2000000c, 1, 0xffffffff, 4}};
    sfm_pci_record_t record;
    assert(!sfm_pci_describe(&device, &record));
    assert(record.bars[0].address == 0x80000000 && record.bars[0].flags ==
           (SFM_BAR_PRESENT | SFM_BAR_MEM32 | SFM_BAR_PREFETCH));
    assert(record.bars[1].address == 0xc000 && record.bars[1].flags == (SFM_BAR_PRESENT | SFM_BAR_IO));
    assert(record.bars[2].address == 0x120000000ull && record.bars[3].flags == SFM_BAR_UPPER);
    assert(record.bars[4].flags == SFM_BAR_INVALID && record.bars[5].flags == SFM_BAR_INVALID);
    for (unsigned i = 0; i < 6; i++) assert(!record.bars[i].size && !(record.bars[i].flags & SFM_BAR_SIZE_KNOWN));
    device.header_type = 1;
    assert(!sfm_pci_describe(&device, &record) && record.bars[0].flags == SFM_BAR_UNKNOWN);
    device.function = 8;
    assert(sfm_pci_describe(&device, &record) == SFM_EINVAL);
}

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
    memcpy(data, original, size);
    meta = (sfm_metadata_t *)(data + section(data, ".sfmod_meta")->sh_offset);
    meta->abi = 0x00010002u;
    assert(sfm_elf_load(data, size, &image) == SFM_ENOEXEC);
    assert(!allocations);
    free(data);
}

typedef struct item { char name[32]; uint64_t offset, size; } item_t;

static void test_tmpfs_limits(void)
{
    uint64_t backend = sfm_lookup("fs.tmpfs"), handles[SFM_FILE_NODES];
    sfm_message_t *req = calloc(1, sizeof(*req)), *resp = calloc(1, sizeof(*resp));
    assert(req && resp && backend);
    req->type = SFM_FS_ROOT;
    assert(!sfm_call(backend, req, resp));
    uint64_t root = resp->handle;
    req->type = SFM_FILE_OPEN;
    req->handle = root;
    req->flags = SFM_CREATE;
    req->size = sizeof("oom");
    memcpy(req->data, "oom", req->size);
    assert(!sfm_call(backend, req, resp));
    uint64_t file = resp->handle;
    req->handle = file;
    req->type = SFM_FILE_WRITE;
    req->size = 1;
    req->data[0] = 'x';
    size_t before = allocations;
    fail_after = 0;
    assert(sfm_call(backend, req, resp) == SFM_ENOMEM);
    fail_after = -1;
    assert(allocations == before);
    req->type = SFM_FILE_STAT;
    assert(!sfm_call(backend, req, resp) && !resp->result);
    req->type = SFM_FILE_WRITE;
    assert(!sfm_call(backend, req, resp) && resp->result == 1);
    req->offset = 5000;
    fail_after = 0;
    assert(sfm_call(backend, req, resp) == SFM_ENOMEM);
    fail_after = -1;
    req->type = SFM_FILE_STAT;
    assert(!sfm_call(backend, req, resp) && resp->result == 1);
    req->type = SFM_FILE_READ;
    req->offset = 0;
    req->size = sizeof(uint32_t);
    uint32_t read_length = 1;
    memcpy(req->data, &read_length, sizeof(read_length));
    assert(!sfm_call(backend, req, resp) && resp->result == 1 && resp->data[0] == 'x');
    req->type = SFM_FILE_REMOVE;
    assert(!sfm_call(backend, req, resp) && allocations == before);
    size_t count = 0;
    for (; count < SFM_FILE_NODES; count++)
    {
        req->type = SFM_FILE_OPEN;
        req->handle = root;
        req->size = (uint32_t)snprintf((char *)req->data, SFM_PAYLOAD, "quota-%zu", count) + 1;
        int result = sfm_call(backend, req, resp);
        if (result) { assert(result == SFM_ENOSPC); break; }
        handles[count] = resp->handle;
    }
    assert(count == SFM_FILE_NODES - SFM_FS_INSTANCES);
    for (size_t i = 0; i < count; i++)
    {
        req->type = SFM_FILE_REMOVE;
        req->handle = handles[i];
        assert(!sfm_call(backend, req, resp));
    }
    free(req);
    free(resp);
}

static void test_package(uint8_t *package, size_t size, uint8_t *failure, size_t fail_size)
{
    uint8_t *copy = malloc(size);
    assert(copy);
    memcpy(copy, package, size);
    ((uint32_t *)copy)[1] = 0x00010002u;
    assert(sfm_package_load(copy, size) == SFM_ENOEXEC && !allocations);
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
        assert(!allocations && !sfm_lookup("fs.vfs") && !sfm_lookup("fs.tmpfs") &&
               !sfm_lookup("dev.tmpfs") && !sfm_lookup("dev.pci"));
    }
    assert(succeeded);
    assert(!sfm_selftest());
    assert(!sfm_pci_selftest());
    test_tmpfs_limits();
    assert(sfm_package_load(package, size) == SFM_EBUSY);
    assert(sfm_module_unload("fs.tmpfs") == SFM_EBUSY);
    assert(sfm_module_unload("dev.tmpfs") == SFM_EBUSY);
    assert(!sfm_module_unload("fs.vfs"));
    assert(!sfm_module_unload("fs.tmpfs"));
    assert(!sfm_module_unload("dev.tmpfs"));
    assert(!sfm_module_unload("demo.hello"));
    uint64_t stale_pci = sfm_lookup("dev.pci");
    assert(!sfm_module_unload("dev.pci"));
    sfm_message_t *req = calloc(1, sizeof(*req)), *resp = calloc(1, sizeof(*resp));
    assert(req && resp && sfm_call(stale_pci, req, resp) == SFM_ENOENT);
    free(req); free(resp);
    assert(!allocations && !sfm_lookup("fs.vfs"));
    free(copy);
}

int main(int argc, char **argv)
{
    test_pci_decode();
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
    for (unsigned pass = 0; pass < 2; pass++) {
        pci_fixture_count = pass ? SFM_PCI_MAX : 0;
        assert(!sfm_package_load(package, size) && !sfm_pci_selftest());
        assert(!sfm_module_unload("fs.vfs"));
        assert(!sfm_module_unload("fs.tmpfs"));
        assert(!sfm_module_unload("dev.tmpfs"));
        assert(!sfm_module_unload("demo.hello"));
        assert(!sfm_module_unload("dev.pci") && !allocations);
    }
    free(package);
    free(failure);
    puts("sfmod tests passed: malformed ELF/package, dependencies, init rollback, OOM, IPC, file IO, unload");
    return 0;
}
