/* 受信任 ELF64 ET_REL 加载器。所有文件区间先验证，重定位不越过目标 section。 */
#include "../internal.h"

#define MAX_SECTIONS 128u
#define MAX_IMAGE (2u * 1024u * 1024u)
#define ALLOC 2u
#define EXEC 4u
#define NOBITS 8u
#define SYMTAB 2u
#define STRTAB 3u
#define RELA 4u

typedef struct elf_header
{
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_header_t;

typedef struct section
{
    uint32_t name, type;
    uint64_t flags, address, offset, size;
    uint32_t link, info;
    uint64_t align, entsize;
} section_t;

typedef struct symbol
{
    uint32_t name;
    uint8_t info, other;
    uint16_t section;
    uint64_t value, size;
} symbol_t;

typedef struct relocation
{
    uint64_t offset, info;
    int64_t addend;
} relocation_t;

typedef struct elf_context
{
    const uint8_t *data;
    size_t size;
    elf_header_t header;
    section_t *sections;
    uintptr_t addresses[MAX_SECTIONS];
} elf_context_t;

static int range(size_t total, uint64_t offset, uint64_t size)
{
    return offset <= total && size <= total - offset;
}

static int text_equal(const uint8_t *table, size_t size, uint32_t offset,
                      const char *expected)
{
    if (offset >= size) return 0;
    for (size_t i = offset, j = 0; i < size; i++, j++)
    {
        if (table[i] != (uint8_t)expected[j]) return 0;
        if (!table[i]) return 1;
    }
    return 0;
}

static int symbol_address(elf_context_t *ctx, const section_t *table,
                          uint64_t index, uint64_t *address)
{
    if (index >= table->size / sizeof(symbol_t)) return SFM_ENOEXEC;
    symbol_t sym;
    sfm_copy(&sym, ctx->data + table->offset + index * sizeof(sym), sizeof(sym));
    const section_t *strings = &ctx->sections[table->link];
    if (sym.name >= strings->size) return SFM_ENOEXEC;
    int terminated = 0;
    for (size_t i = sym.name; i < strings->size; i++)
        if (!ctx->data[strings->offset + i]) { terminated = 1; break; }
    if (!terminated) return SFM_ENOEXEC;
    if (!sym.section)
    {
        /* 未定义强符号不从任意内核地址解析；弱符号可为 NULL。 */
        if (index && (sym.info >> 4) != 2) return SFM_ENOEXEC;
        *address = 0;
        return 0;
    }
    if (sym.section == 0xfff1) { *address = sym.value; return 0; }
    if (sym.section >= ctx->header.shnum || !ctx->addresses[sym.section])
        return SFM_ENOEXEC;
    const section_t *target = &ctx->sections[sym.section];
    if (!range(target->size, sym.value, sym.size)) return SFM_ENOEXEC;
    *address = ctx->addresses[sym.section] + sym.value;
    return 0;
}

static int relocate(elf_context_t *ctx, const section_t *rel)
{
    if (rel->link >= ctx->header.shnum || rel->info >= ctx->header.shnum ||
        rel->entsize != sizeof(relocation_t) || rel->size % sizeof(relocation_t))
        return SFM_ENOEXEC;
    const section_t *symbols = &ctx->sections[rel->link];
    const section_t *target = &ctx->sections[rel->info];
    if (symbols->type != SYMTAB || symbols->entsize != sizeof(symbol_t) ||
        symbols->size % sizeof(symbol_t) || symbols->link >= ctx->header.shnum ||
        ctx->sections[symbols->link].type != STRTAB) return SFM_ENOEXEC;
    if (!(target->flags & ALLOC)) return 0;
    for (size_t i = 0; i < rel->size; i += sizeof(relocation_t))
    {
        relocation_t r;
        sfm_copy(&r, ctx->data + rel->offset + i, sizeof(r));
        uint32_t type = (uint32_t)r.info;
        if (!type) continue;
        size_t width = type == 1 ? 8 : 4;
        if ((type != 1 && type != 2 && type != 4 && type != 10 && type != 11) ||
            !range(target->size, r.offset, width)) return SFM_ENOEXEC;
        uint64_t symbol;
        if (symbol_address(ctx, symbols, r.info >> 32, &symbol)) return SFM_ENOEXEC;
        uintptr_t place = ctx->addresses[rel->info] + r.offset;
        __int128 value = (__int128)symbol + r.addend;
        if (type == 2 || type == 4) value -= place;
        if (type == 1)
        {
            if (value < 0 || value > UINT64_MAX) return SFM_ENOEXEC;
            uint64_t v = (uint64_t)value;
            sfm_copy((void *)place, &v, 8);
        }
        else
        {
            if (type == 10 ? (value < 0 || value > UINT32_MAX) :
                (value < INT32_MIN || value > INT32_MAX)) return SFM_ENOEXEC;
            uint32_t v = (uint32_t)value;
            sfm_copy((void *)place, &v, 4);
        }
    }
    return 0;
}

static int executable(elf_context_t *ctx, uintptr_t address)
{
    for (size_t i = 1; i < ctx->header.shnum; i++)
        if ((ctx->sections[i].flags & (ALLOC | EXEC)) == (ALLOC | EXEC) &&
            address >= ctx->addresses[i] &&
            address - ctx->addresses[i] < ctx->sections[i].size) return 1;
    return 0;
}

static int load_sections(elf_context_t *ctx, sfm_image_t *image)
{
    size_t total = 0;
    const section_t *names = &ctx->sections[ctx->header.shstrndx];
    if (names->type != STRTAB) return SFM_ENOEXEC;
    size_t meta_index = 0, entry_index = 0, deps_index = 0;
    for (size_t i = 1; i < ctx->header.shnum; i++)
    {
        const section_t *s = &ctx->sections[i];
        if (s->type != NOBITS && !range(ctx->size, s->offset, s->size))
            return SFM_ENOEXEC;
        if (s->type == 9 || (s->flags & (0x400u | 0x800u))) return SFM_ENOEXEC;
        const uint8_t *strings = ctx->data + names->offset;
        size_t *found = text_equal(strings, names->size, s->name, ".sfmod_meta") ?
            &meta_index : text_equal(strings, names->size, s->name, ".sfmod_init") ?
            &entry_index : text_equal(strings, names->size, s->name, ".sfmod_deps") ?
            &deps_index : NULL;
        if (found)
        {
            if (*found || !(s->flags & ALLOC) || s->type != 1) return SFM_ENOEXEC;
            *found = i;
        }
        if (!(s->flags & ALLOC)) continue;
        /* Ubuntu GCC 可产生只读 .note.gnu.property；它是元数据，不是代码。 */
        if (s->type != 1 && s->type != NOBITS &&
            !(s->type == 7 && !(s->flags & (1u | EXEC)))) return SFM_ENOEXEC;
        uint64_t alignment = s->align ? s->align : 1;
        if (alignment > 4096 || (alignment & (alignment - 1)) || s->size > MAX_IMAGE)
            return SFM_ENOEXEC;
        total = (total + alignment - 1) & ~(size_t)(alignment - 1);
        if (total > MAX_IMAGE - s->size) return SFM_ENOEXEC;
        ctx->addresses[i] = total;
        total += s->size;
    }
    if (!meta_index || !entry_index ||
        ctx->sections[meta_index].size != sizeof(sfm_metadata_t) ||
        ctx->sections[entry_index].size != sizeof(sfm_entry_t)) return SFM_ENOEXEC;
    image->allocation = sf_alloc(total + 4095);
    if (!image->allocation) return SFM_ENOMEM;
    image->base = (uint8_t *)(((uintptr_t)image->allocation + 4095) & ~(uintptr_t)4095);
    image->size = total;
    sfm_clear(image->base, total);
    for (size_t i = 1; i < ctx->header.shnum; i++)
    {
        const section_t *s = &ctx->sections[i];
        if (!(s->flags & ALLOC)) continue;
        ctx->addresses[i] += (uintptr_t)image->base;
        if (s->type != NOBITS)
            sfm_copy((void *)ctx->addresses[i], ctx->data + s->offset, s->size);
    }
    for (size_t i = 1; i < ctx->header.shnum; i++)
        if (ctx->sections[i].type == RELA && relocate(ctx, &ctx->sections[i]))
            return SFM_ENOEXEC;
    sfm_copy(&image->meta, (void *)ctx->addresses[meta_index], sizeof(image->meta));
    sfm_copy(&image->entry, (void *)ctx->addresses[entry_index], sizeof(image->entry));
    const sfm_metadata_t *m = &image->meta;
    if (m->magic != SFM_MAGIC || m->abi != SFM_ABI || m->header_size != sizeof(*m) ||
        m->flags != SFM_TRUSTED || m->permissions || m->reserved || m->reserved2 ||
        !sfm_name_valid(m->name, sizeof(m->name)) ||
        !sfm_name_valid(m->version, sizeof(m->version)) ||
        !executable(ctx, (uintptr_t)image->entry.init) ||
        !executable(ctx, (uintptr_t)image->entry.exit)) return SFM_ENOEXEC;
    if (deps_index)
    {
        size_t size = ctx->sections[deps_index].size;
        if (size % 32 || size / 32 > SFM_MAX_DEPS) return SFM_ENOEXEC;
        image->dep_count = size / 32;
        sfm_copy(image->deps, (void *)ctx->addresses[deps_index], size);
        for (size_t i = 0; i < image->dep_count; i++)
            if (!sfm_name_valid(image->deps[i], 32)) return SFM_ENOEXEC;
    }
    return 0;
}

int sfm_elf_load(const void *data, size_t size, sfm_image_t *image)
{
    if (!image) return SFM_EINVAL;
    sfm_clear(image, sizeof(*image));
    if (!data || size < sizeof(elf_header_t)) return SFM_ENOEXEC;
    elf_context_t ctx;
    sfm_clear(&ctx, sizeof(ctx));
    ctx.data = data;
    ctx.size = size;
    sfm_copy(&ctx.header, data, sizeof(ctx.header));
    elf_header_t *h = &ctx.header;
    if (h->ident[0] != 0x7f || h->ident[1] != 'E' || h->ident[2] != 'L' ||
        h->ident[3] != 'F' || h->ident[4] != 2 || h->ident[5] != 1 ||
        h->ident[6] != 1 || h->type != 1 || h->machine != 62 || h->version != 1 ||
        h->ehsize != sizeof(*h) || h->shentsize != sizeof(section_t) ||
        h->shnum < 2 || h->shnum > MAX_SECTIONS || !h->shstrndx ||
        h->shstrndx >= h->shnum || h->phnum ||
        !range(size, h->shoff, (uint64_t)h->shnum * sizeof(section_t)))
        return SFM_ENOEXEC;
    size_t bytes = h->shnum * sizeof(section_t);
    ctx.sections = sf_alloc(bytes);
    if (!ctx.sections) return SFM_ENOMEM;
    sfm_copy(ctx.sections, ctx.data + h->shoff, bytes);
    int result = SFM_ENOEXEC;
    const section_t *names = &ctx.sections[h->shstrndx];
    if (range(size, names->offset, names->size)) result = load_sections(&ctx, image);
    sf_free(ctx.sections);
    if (result) sfm_elf_free(image);
    return result;
}

void sfm_elf_free(sfm_image_t *image)
{
    if (!image) return;
    sf_free(image->allocation);
    sfm_clear(image, sizeof(*image));
}
