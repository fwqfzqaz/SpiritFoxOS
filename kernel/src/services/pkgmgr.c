/*
 * pkgmgr.c - SpiritFoxOS Package Manager Implementation
 *
 * Handles installation, removal, and querying of both DEB and SFK packages.
 * DEB packages follow a standard dpkg-like flow; SFK packages go through
 * a permission-based sandbox authorization flow.
 */

#include "pkgmgr.h"
#include "registry.h"
#include "sandbox.h"
#include "syscall.h"
#include "process.h"
#include "kmalloc.h"
#include "vfs.h"
#include "string.h"
#include "vga.h"
#include "gunzip.h"

/* ========================================================================
 * Constants
 * ======================================================================== */

#define DEB_AR_MAGIC      "!<arch>\n"
#define DEB_AR_MAGIC_LEN  8

/* ========================================================================
 * Helpers
 * ======================================================================== */

static int snprintf_local(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    char *out = buf;
    size_t remaining = size;
    const char *p = fmt;
    while (*p && remaining > 1) {
        if (*p == '%' && *(p+1)) {
            p++;
            if (*p == 's') {
                const char *s = __builtin_va_arg(ap, const char *);
                while (*s && remaining > 1) { *out++ = *s++; remaining--; }
            } else if (*p == 'u' || *p == 'd') {
                int v = __builtin_va_arg(ap, int);
                if (v < 0 && *p == 'd' && remaining > 1) { *out++ = '-'; remaining--; v = -v; }
                char tmp[20]; int pos = 0;
                if (v == 0) tmp[pos++] = '0';
                else while (v > 0) { tmp[pos++] = '0' + (v % 10); v /= 10; }
                for (int i = pos - 1; i >= 0 && remaining > 1; i--) { *out++ = tmp[i]; remaining--; }
            } else if (*p == 'x') {
                unsigned int v = __builtin_va_arg(ap, unsigned int);
                const char *hex = "0123456789abcdef";
                char tmp[16]; int pos = 0;
                if (v == 0) tmp[pos++] = '0';
                else while (v > 0) { tmp[pos++] = hex[v & 0xf]; v >>= 4; }
                for (int i = pos - 1; i >= 0 && remaining > 1; i--) { *out++ = tmp[i]; remaining--; }
            } else { if (remaining > 1) { *out++ = '%'; remaining--; } if (remaining > 1) { *out++ = *p; remaining--; } }
            p++;
        } else {
            *out++ = *p++;
            remaining--;
        }
    }
    *out = '\0';
    __builtin_va_end(ap);
    return (int)(out - buf);
}

/* ========================================================================
 * Streaming AR archive helpers
 * ======================================================================== */

#define AR_HDR_SIZE  60
#define AR_MAGIC     "`\n"

typedef struct {
    char   name[17];   /* null-terminated member name (trimmed) */
    size_t offset;     /* offset of member DATA in the file */
    size_t size;       /* size of member data */
} ar_member_info_t;

/* Walk AR archive headers using VFS seek/read.
 * Returns number of members found, or negative on error. */
static int stream_ar_find_members(int fd, int64_t file_size,
                                  ar_member_info_t *members, int max_members)
{
    uint8_t magic_buf[8];
    int count = 0;
    size_t pos;

    /* Read and verify AR magic */
    vfs_seek(fd, 0, VFS_SEEK_SET);
    if (vfs_read(fd, magic_buf, DEB_AR_MAGIC_LEN) != DEB_AR_MAGIC_LEN)
        return -1;
    if (memcmp(magic_buf, DEB_AR_MAGIC, DEB_AR_MAGIC_LEN) != 0)
        return -1;

    pos = DEB_AR_MAGIC_LEN;

    while (pos + AR_HDR_SIZE <= (size_t)file_size && count < max_members) {
        uint8_t hdr[AR_HDR_SIZE];

        /* Read the 60-byte AR header */
        vfs_seek(fd, (int64_t)pos, VFS_SEEK_SET);
        if (vfs_read(fd, hdr, AR_HDR_SIZE) != AR_HDR_SIZE)
            break;

        /* Validate header magic */
        if (memcmp(hdr + 58, AR_MAGIC, 2) != 0)
            break;

        /* Parse name (16 bytes, space-padded, may end with '/') */
        char name[17];
        memcpy(name, hdr, 16);
        name[16] = '\0';

        int name_len = 16;
        while (name_len > 0 && name[name_len - 1] == ' ')
            name_len--;
        if (name_len > 0 && name[name_len - 1] == '/')
            name_len--;
        name[name_len] = '\0';

        /* Parse size (10 bytes, decimal, space-padded) */
        char size_str[11];
        memcpy(size_str, hdr + 48, 10);
        size_str[10] = '\0';
        size_t member_size = 0;
        for (int i = 0; i < 10; i++) {
            if (size_str[i] >= '0' && size_str[i] <= '9')
                member_size = member_size * 10 + (size_str[i] - '0');
        }

        /* Store member info */
        memcpy(members[count].name, name, name_len + 1);
        members[count].offset = pos + AR_HDR_SIZE;
        members[count].size   = member_size;
        count++;

        /* Advance past header + data, padded to 2-byte boundary */
        pos += AR_HDR_SIZE + member_size;
        if (member_size & 1)
            pos++;
    }

    return count;
}

/* Find a specific AR member by name prefix from the members array */
static int ar_find_member(ar_member_info_t *members, int count,
                          const char *name_prefix,
                          ar_member_info_t *out)
{
    size_t prefix_len = strlen(name_prefix);
    for (int i = 0; i < count; i++) {
        size_t nlen = strlen(members[i].name);
        if (prefix_len <= nlen &&
            memcmp(members[i].name, name_prefix, prefix_len) == 0) {
            if (out) *out = members[i];
            return 0;
        }
    }
    return -1;
}

/* ========================================================================
 * ustar tar helpers
 * ======================================================================== */

#define TAR_BLOCK_SIZE 512

/* Parse an octal field from a tar header. Handles null/space-terminated
 * octal strings, as well as the GNU base-256 encoding (high bit set). */
static size_t parse_octal(const char *field, int width)
{
    size_t val = 0;

    /* GNU base-256 encoding: if high bit of first byte is set */
    if ((unsigned char)field[0] & 0x80) {
        for (int i = 0; i < width; i++) {
            val = (val << 8) | (unsigned char)field[i];
        }
        val &= ~((size_t)1 << (width * 8 - 1));
        return val;
    }

    /* Standard octal */
    for (int i = 0; i < width; i++) {
        if (field[i] >= '0' && field[i] <= '7') {
            val = val * 8 + (field[i] - '0');
        }
    }
    return val;
}

/* Recursively create all directories along `path`. */
static int mkdir_recursive(const char *path, uint32_t mode)
{
    char tmp[VFS_MAX_PATH];
    size_t len = strlen(path);

    if (len >= VFS_MAX_PATH)
        return -1;

    memcpy(tmp, path, len + 1);

    /* Iterate and create each component */
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            vfs_mkdir(tmp, mode);
            tmp[i] = '/';
        }
    }
    /* Create the final directory */
    vfs_mkdir(tmp, mode);
    return 0;
}

/* ========================================================================
 * Streaming control info parser
 * ======================================================================== */

/* Parse DEB control info by reading control.tar from VFS into a small
 * buffer (control.tar is typically <4KB).  Extracts the ./control file
 * from the tar and scans for standard fields. */
static int stream_deb_parse_control(int fd, size_t ctrl_offset, size_t ctrl_size,
                                    deb_control_t *ctrl)
{
    if (ctrl_size > 65536) {
        printf("[pkgmgr] control.tar unexpectedly large (%lu bytes)\n",
               (unsigned long)ctrl_size);
        return -1;
    }

    uint8_t *buf = (uint8_t *)kmalloc(ctrl_size + 1);
    if (!buf) return -1;

    vfs_seek(fd, (int64_t)ctrl_offset, VFS_SEEK_SET);
    if ((size_t)vfs_read(fd, buf, ctrl_size) != ctrl_size) {
        kfree(buf);
        return -1;
    }

    /* Find the control file inside control.tar (it's a tar archive) */
    size_t tar_pos = 0;
    const char *control_data = NULL;
    size_t control_len = 0;

    while (tar_pos + TAR_BLOCK_SIZE <= ctrl_size) {
        const char *hdr = (const char *)(buf + tar_pos);

        /* Check for end-of-archive */
        int all_zero = 1;
        for (int i = 0; i < TAR_BLOCK_SIZE; i++) {
            if (hdr[i] != '\0') { all_zero = 0; break; }
        }
        if (all_zero) break;

        char name[101];
        memcpy(name, hdr, 100);
        name[100] = '\0';
        /* Trim */
        int nlen = strlen(name);
        while (nlen > 0 && (name[nlen-1] == '\0' || name[nlen-1] == ' ' || name[nlen-1] == '/'))
            nlen--;
        name[nlen] = '\0';

        size_t entry_size = parse_octal(hdr + 124, 12);
        char typeflag = hdr[156];

        /* Look for the control file (name is "control" or "./control") */
        const char *base = name;
        while (*base == '/') base++;
        if (base[0] == '.' && base[1] == '/') base += 2;

        if ((typeflag == '0' || typeflag == '\0') &&
            strcmp(base, "control") == 0) {
            control_data = (const char *)(buf + tar_pos + TAR_BLOCK_SIZE);
            control_len = entry_size;
            break;
        }

        /* Advance past header + data */
        tar_pos += TAR_BLOCK_SIZE;
        tar_pos += ((entry_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE;
    }

    if (!control_data) {
        printf("[pkgmgr] control file not found in control.tar\n");
        kfree(buf);
        return -1;
    }

    /* Parse control fields from the control file content */
    memset(ctrl, 0, sizeof(*ctrl));
    const char *p = control_data;
    const char *end = control_data + control_len;

    while (p < end - 8) {
        if (memcmp(p, "Package:", 8) == 0) {
            const char *vs = p + 8;
            while (vs < end && (*vs == ' ' || *vs == '\t')) vs++;
            const char *ve = vs;
            while (ve < end && *ve != '\n') ve++;
            size_t len = ve - vs;
            if (len >= sizeof(ctrl->package)) len = sizeof(ctrl->package) - 1;
            memcpy(ctrl->package, vs, len);
            ctrl->package[len] = '\0';
        } else if (memcmp(p, "Version:", 8) == 0) {
            const char *vs = p + 8;
            while (vs < end && (*vs == ' ' || *vs == '\t')) vs++;
            const char *ve = vs;
            while (ve < end && *ve != '\n') ve++;
            size_t len = ve - vs;
            if (len >= sizeof(ctrl->version)) len = sizeof(ctrl->version) - 1;
            memcpy(ctrl->version, vs, len);
            ctrl->version[len] = '\0';
        } else if (memcmp(p, "Architecture:", 13) == 0) {
            const char *vs = p + 13;
            while (vs < end && (*vs == ' ' || *vs == '\t')) vs++;
            const char *ve = vs;
            while (ve < end && *ve != '\n') ve++;
            size_t len = ve - vs;
            if (len >= sizeof(ctrl->architecture)) len = sizeof(ctrl->architecture) - 1;
            memcpy(ctrl->architecture, vs, len);
            ctrl->architecture[len] = '\0';
        } else if (memcmp(p, "Maintainer:", 11) == 0) {
            const char *vs = p + 11;
            while (vs < end && (*vs == ' ' || *vs == '\t')) vs++;
            const char *ve = vs;
            while (ve < end && *ve != '\n') ve++;
            size_t len = ve - vs;
            if (len >= sizeof(ctrl->maintainer)) len = sizeof(ctrl->maintainer) - 1;
            memcpy(ctrl->maintainer, vs, len);
            ctrl->maintainer[len] = '\0';
        } else if (memcmp(p, "Description:", 12) == 0) {
            const char *vs = p + 12;
            while (vs < end && (*vs == ' ' || *vs == '\t')) vs++;
            const char *ve = vs;
            while (ve < end && *ve != '\n') ve++;
            size_t len = ve - vs;
            if (len >= sizeof(ctrl->description)) len = sizeof(ctrl->description) - 1;
            memcpy(ctrl->description, vs, len);
            ctrl->description[len] = '\0';
        } else if (memcmp(p, "Depends:", 8) == 0) {
            const char *vs = p + 8;
            while (vs < end && (*vs == ' ' || *vs == '\t')) vs++;
            const char *ve = vs;
            while (ve < end && *ve != '\n') ve++;
            size_t len = ve - vs;
            if (len >= sizeof(ctrl->depends)) len = sizeof(ctrl->depends) - 1;
            memcpy(ctrl->depends, vs, len);
            ctrl->depends[len] = '\0';
        }
        p++;
    }

    kfree(buf);

    if (ctrl->package[0] == '\0') {
        printf("[pkgmgr] Package field not found in control\n");
        return -1;
    }

    return 0;
}

/* ========================================================================
 * Incremental tar stream processor
 * ======================================================================== */

#define TAR_STREAM_READ_BUF  (64 * 1024)  /* 64KB read buffer for tar */

typedef enum {
    TAR_STATE_HEADER,    /* Reading 512-byte header */
    TAR_STATE_FILE_DATA, /* Writing file data to VFS */
    TAR_STATE_FILE_PAD,  /* Skipping padding after file data */
    TAR_STATE_SKIP_DATA, /* Skipping unhandled entry data */
    TAR_STATE_SKIP_PAD,  /* Skipping padding after skipped data */
    TAR_STATE_DONE
} tar_state_t;

typedef struct {
    char        install_prefix[VFS_MAX_PATH];
    size_t      prefix_len;
    int         extracted;

    /* State machine */
    tar_state_t state;

    /* Partial header accumulation */
    size_t      header_bytes;
    char        header[TAR_BLOCK_SIZE];

    /* Current entry */
    char        target[VFS_MAX_PATH];
    size_t      entry_data_size;   /* file size from header */
    size_t      data_remaining;    /* bytes of data still to process */
    size_t      pad_remaining;     /* bytes of padding still to skip */
    int         write_fd;          /* VFS fd for current output file */
} tar_stream_ctx_t;

/* Build the target path from the tar header's name/prefix fields and
 * the install_prefix.  Returns 0 on success, -1 if the name is empty. */
static int tar_build_target(const char *header, const char *install_prefix,
                            size_t prefix_len, char *target, size_t target_cap)
{
    char name[101], pf[156];

    memcpy(name, header, 100);
    name[100] = '\0';
    memcpy(pf, header + 345, 155);
    pf[155] = '\0';

    char full_name[VFS_MAX_PATH];
    if (pf[0] != '\0') {
        int plen = 155;
        while (plen > 0 && (pf[plen-1] == '\0' || pf[plen-1] == ' '))
            plen--;
        pf[plen] = '\0';

        int nstart = 0;
        while (nstart < 100 && (name[nstart] == '\0' || name[nstart] == ' '))
            nstart++;

        snprintf_local(full_name, sizeof(full_name), "%s/%s", pf, name + nstart);
    } else {
        int nlen = strlen(name);
        while (nlen > 0 && (name[nlen-1] == '\0' || name[nlen-1] == ' '))
            nlen--;
        name[nlen] = '\0';
        memcpy(full_name, name, nlen + 1);
    }

    if (full_name[0] == '\0')
        return -1;

    /* Strip leading slashes from full_name */
    const char *fname = full_name;
    while (*fname == '/') fname++;

    if (prefix_len > 0) {
        if (install_prefix[prefix_len - 1] == '/')
            snprintf_local(target, target_cap, "%s%s", install_prefix, fname);
        else
            snprintf_local(target, target_cap, "%s/%s", install_prefix, fname);
    } else {
        size_t flen = strlen(fname);
        if (flen >= target_cap) flen = target_cap - 1;
        memcpy(target, fname, flen + 1);
    }

    /* Remove trailing slash */
    int tlen = strlen(target);
    while (tlen > 1 && target[tlen - 1] == '/') {
        target[tlen - 1] = '\0';
        tlen--;
    }

    return 0;
}

/* Ensure parent directory exists for a given file path */
static void ensure_parent_dir(const char *path)
{
    char parent[VFS_MAX_PATH];
    size_t len = strlen(path);
    if (len >= VFS_MAX_PATH) return;
    memcpy(parent, path, len + 1);

    char *last_slash = NULL;
    for (int i = (int)len - 1; i >= 0; i--) {
        if (parent[i] == '/') { last_slash = &parent[i]; break; }
    }
    if (last_slash) {
        *last_slash = '\0';
        if (parent[0] != '\0')
            mkdir_recursive(parent, 0755);
    }
}

/* Compute the padding needed after file_size bytes of tar data */
static size_t tar_padding(size_t file_size)
{
    size_t mod = file_size % TAR_BLOCK_SIZE;
    return mod ? (TAR_BLOCK_SIZE - mod) : 0;
}

/* Process a chunk of tar data incrementally.
 * Returns 0 to continue, negative on error. */
static int tar_process_chunk(const void *data, size_t len,
                             tar_stream_ctx_t *ctx)
{
    const uint8_t *buf = (const uint8_t *)data;
    size_t pos = 0;

    while (pos < len) {
        size_t avail = len - pos;

        switch (ctx->state) {
        case TAR_STATE_HEADER: {
            size_t need = TAR_BLOCK_SIZE - ctx->header_bytes;
            size_t copy = need < avail ? need : avail;
            memcpy(ctx->header + ctx->header_bytes, buf + pos, copy);
            ctx->header_bytes += copy;
            pos += copy;

            if (ctx->header_bytes < TAR_BLOCK_SIZE)
                return 0;  /* Need more data for this header */

            /* Full header received */
            ctx->header_bytes = 0;

            /* Check for end-of-archive (two zero blocks) */
            int all_zero = 1;
            for (int i = 0; i < TAR_BLOCK_SIZE; i++) {
                if (ctx->header[i] != '\0') { all_zero = 0; break; }
            }
            if (all_zero) {
                ctx->state = TAR_STATE_DONE;
                return 0;
            }

            /* Parse header */
            size_t file_size = parse_octal(ctx->header + 124, 12);
            char typeflag = ctx->header[156];

            if (tar_build_target(ctx->header,
                                 ctx->install_prefix, ctx->prefix_len,
                                 ctx->target, sizeof(ctx->target)) < 0) {
                /* Empty name – skip */
                ctx->data_remaining = file_size;
                ctx->pad_remaining  = tar_padding(file_size);
                ctx->state = (file_size > 0) ? TAR_STATE_SKIP_DATA
                            : TAR_STATE_HEADER;
                if (ctx->pad_remaining > 0 && file_size == 0)
                    ctx->state = TAR_STATE_SKIP_PAD;
                break;
            }

            if (typeflag == '5') {
                /* Directory */
                mkdir_recursive(ctx->target, 0755);
                ctx->state = TAR_STATE_HEADER;
            } else if (typeflag == '0' || typeflag == '\0') {
                /* Regular file */
                ensure_parent_dir(ctx->target);
                ctx->entry_data_size = file_size;
                ctx->data_remaining  = file_size;
                ctx->pad_remaining   = tar_padding(file_size);

                if (file_size == 0) {
                    /* Empty file */
                    int fd = vfs_open(ctx->target,
                                      VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644);
                    if (fd >= 0) vfs_close(fd);
                    ctx->extracted++;
                    ctx->state = ctx->pad_remaining > 0
                                 ? TAR_STATE_FILE_PAD : TAR_STATE_HEADER;
                } else {
                    ctx->write_fd = vfs_open(ctx->target,
                                             VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC,
                                             0644);
                    if (ctx->write_fd >= 0) {
                        ctx->state = TAR_STATE_FILE_DATA;
                    } else {
                        printf("[pkgmgr] Failed to create: %s\n", ctx->target);
                        ctx->state = TAR_STATE_SKIP_DATA;
                    }
                }
            } else {
                /* Skip other entry types */
                ctx->data_remaining = file_size;
                ctx->pad_remaining  = tar_padding(file_size);
                ctx->state = (file_size > 0) ? TAR_STATE_SKIP_DATA
                            : (ctx->pad_remaining > 0 ? TAR_STATE_SKIP_PAD
                               : TAR_STATE_HEADER);
            }
            break;
        }

        case TAR_STATE_FILE_DATA: {
            size_t to_write = ctx->data_remaining < avail
                            ? ctx->data_remaining : avail;
            if (to_write > 0 && ctx->write_fd >= 0)
                vfs_write(ctx->write_fd, buf + pos, to_write);
            pos += to_write;
            ctx->data_remaining -= to_write;

            if (ctx->data_remaining == 0) {
                if (ctx->write_fd >= 0) {
                    vfs_close(ctx->write_fd);
                    ctx->write_fd = -1;
                    ctx->extracted++;
                    if ((ctx->extracted % 50) == 0)
                        printf("[pkgmgr] Extracted %d files...\n", ctx->extracted);
                }
                ctx->state = ctx->pad_remaining > 0
                             ? TAR_STATE_FILE_PAD : TAR_STATE_HEADER;
            }
            break;
        }

        case TAR_STATE_FILE_PAD: {
            size_t to_skip = ctx->pad_remaining < avail
                           ? ctx->pad_remaining : avail;
            pos += to_skip;
            ctx->pad_remaining -= to_skip;
            if (ctx->pad_remaining == 0)
                ctx->state = TAR_STATE_HEADER;
            break;
        }

        case TAR_STATE_SKIP_DATA: {
            size_t to_skip = ctx->data_remaining < avail
                           ? ctx->data_remaining : avail;
            pos += to_skip;
            ctx->data_remaining -= to_skip;
            if (ctx->data_remaining == 0)
                ctx->state = ctx->pad_remaining > 0
                             ? TAR_STATE_SKIP_PAD : TAR_STATE_HEADER;
            break;
        }

        case TAR_STATE_SKIP_PAD: {
            size_t to_skip = ctx->pad_remaining < avail
                           ? ctx->pad_remaining : avail;
            pos += to_skip;
            ctx->pad_remaining -= to_skip;
            if (ctx->pad_remaining == 0)
                ctx->state = TAR_STATE_HEADER;
            break;
        }

        case TAR_STATE_DONE:
            return 0;
        }
    }

    return 0;
}

/* ========================================================================
 * Streaming tar extraction from VFS (uncompressed data.tar)
 * ======================================================================== */

static int stream_extract_tar(int fd, size_t tar_offset, size_t tar_size,
                              const char *install_prefix)
{
    uint8_t *buf = (uint8_t *)kmalloc(TAR_STREAM_READ_BUF);
    if (!buf) return -1;

    tar_stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    strncpy(ctx.install_prefix, install_prefix, sizeof(ctx.install_prefix) - 1);
    ctx.prefix_len = strlen(install_prefix);
    ctx.state      = TAR_STATE_HEADER;
    ctx.write_fd   = -1;

    size_t total_read = 0;
    vfs_seek(fd, (int64_t)tar_offset, VFS_SEEK_SET);

    while (total_read < tar_size) {
        size_t to_read = tar_size - total_read;
        if (to_read > TAR_STREAM_READ_BUF)
            to_read = TAR_STREAM_READ_BUF;

        int n = vfs_read(fd, buf, to_read);
        if (n <= 0) break;

        int rc = tar_process_chunk(buf, n, &ctx);
        if (rc < 0) {
            if (ctx.write_fd >= 0) vfs_close(ctx.write_fd);
            kfree(buf);
            return rc;
        }

        total_read += n;

        if (ctx.state == TAR_STATE_DONE)
            break;
    }

    /* Clean up any open file */
    if (ctx.write_fd >= 0) {
        vfs_close(ctx.write_fd);
        ctx.write_fd = -1;
    }

    kfree(buf);
    printf("[pkgmgr] Stream-extracted %d files from data.tar\n", ctx.extracted);
    return ctx.extracted;
}

/* ========================================================================
 * Streaming data.tar.gz extraction via gunzip_stream_vfs
 * ======================================================================== */

/* Callback context for gunzip_stream_vfs → tar processing */
typedef struct {
    tar_stream_ctx_t *tar_ctx;
} tar_gz_cb_ctx_t;

static int tar_gz_output_cb(const void *data, size_t len, void *ctx)
{
    tar_gz_cb_ctx_t *cb_ctx = (tar_gz_cb_ctx_t *)ctx;
    return tar_process_chunk(data, len, cb_ctx->tar_ctx);
}

static int stream_extract_tar_gz(int fd, size_t gz_offset, size_t gz_size,
                                 const char *install_prefix)
{
    tar_stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    strncpy(ctx.install_prefix, install_prefix, sizeof(ctx.install_prefix) - 1);
    ctx.prefix_len = strlen(install_prefix);
    ctx.state      = TAR_STATE_HEADER;
    ctx.write_fd   = -1;

    tar_gz_cb_ctx_t cb_ctx;
    cb_ctx.tar_ctx = &ctx;

    int rc = gunzip_stream_vfs(fd, gz_offset, gz_size,
                               tar_gz_output_cb, &cb_ctx);

    /* Clean up any open file */
    if (ctx.write_fd >= 0) {
        vfs_close(ctx.write_fd);
        ctx.write_fd = -1;
    }

    if (rc < 0) {
        printf("[pkgmgr] Failed to decompress data.tar.gz\n");
        return rc;
    }

    printf("[pkgmgr] Stream-extracted %d files from data.tar.gz\n", ctx.extracted);
    return ctx.extracted;
}

/* ========================================================================
 * pkgmgr_init - Initialize the package manager
 * ======================================================================== */

void pkgmgr_init(void)
{
    /* Load cached package info from the registry if available */
    registry_open_key(REG_ROOT_SYSTEM "/Software", 1);
    printf("[pkgmgr] Package manager initialized\n");
}

/* ========================================================================
 * pkgmgr_install - Auto-detect format and dispatch
 * ======================================================================== */

int pkgmgr_install(const char *path, uint32_t flags)
{
    int fd;
    uint8_t buf[8];
    int n;

    fd = vfs_open(path, VFS_O_RDONLY, 0);
    if (fd < 0) {
        printf("[pkgmgr] Cannot open package file: %s\n", path);
        return -1;
    }

    n = vfs_read(fd, buf, sizeof(buf));
    vfs_close(fd);

    if (n < 4) {
        printf("[pkgmgr] Package file too small: %s\n", path);
        return -1;
    }

    /* Check for DEB: ar archive starts with "!<arch>\n" */
    if (n >= DEB_AR_MAGIC_LEN && memcmp(buf, DEB_AR_MAGIC, DEB_AR_MAGIC_LEN) == 0) {
        return pkgmgr_install_deb(path, flags);
    }

    /* Check for SFK: magic 0x53464B21 at offset 0 (little-endian: 0x21,0x4B,0x46,0x53) */
    if (n >= 4) {
        uint32_t magic = (uint32_t)buf[0]       |
                         ((uint32_t)buf[1] << 8)  |
                         ((uint32_t)buf[2] << 16) |
                         ((uint32_t)buf[3] << 24);
        if (magic == SFK_MAGIC) {
            return pkgmgr_install_sfk(path, flags, NULL);
        }
    }

    printf("[pkgmgr] Unknown package format: %s\n", path);
    return -1;
}

/* ========================================================================
 * pkgmgr_install_deb - Install a DEB package
 * ======================================================================== */

int pkgmgr_install_deb(const char *path, uint32_t flags)
{
    int fd;
    int64_t file_size;
    deb_control_t ctrl;
    reg_software_record_t rec;
    char install_dir[256];
    ar_member_info_t members[16];
    int nmembers;
    ar_member_info_t ctrl_member, data_member;

    fd = vfs_open(path, VFS_O_RDONLY, 0);
    if (fd < 0) {
        printf("[pkgmgr] Cannot open DEB file: %s\n", path);
        return -1;
    }

    /* Determine file size */
    vfs_inode_t stat;
    if (vfs_fstat(fd, &stat) < 0) {
        printf("[pkgmgr] Cannot stat DEB file: %s\n", path);
        vfs_close(fd);
        return -1;
    }
    file_size = stat.size;

    /* ---- 1. Find AR members using streaming header scan ---- */
    nmembers = stream_ar_find_members(fd, file_size, members, 16);
    if (nmembers < 0) {
        printf("[pkgmgr] Failed to parse AR archive: %s\n", path);
        vfs_close(fd);
        return -1;
    }

    /* Locate control.tar and data.tar members */
    if (ar_find_member(members, nmembers, "control.tar", &ctrl_member) < 0) {
        printf("[pkgmgr] control.tar not found in DEB archive\n");
        vfs_close(fd);
        return -1;
    }

    if (ar_find_member(members, nmembers, "data.tar", &data_member) < 0) {
        printf("[pkgmgr] data.tar not found in DEB archive\n");
        vfs_close(fd);
        return -1;
    }

    /* ---- 2. Parse control info (small buffer, safe to read entirely) ---- */
    memset(&ctrl, 0, sizeof(ctrl));
    if (stream_deb_parse_control(fd, ctrl_member.offset, ctrl_member.size,
                                 &ctrl) < 0) {
        printf("[pkgmgr] Failed to parse DEB control\n");
        vfs_close(fd);
        return -1;
    }

    /* Skip dependency resolution if requested or if NODEPS flag set */
    if (!(flags & PKG_INSTALL_NODEPS)) {
        /* Stub: dependency resolution not yet implemented */
    }

    if (flags & PKG_INSTALL_DRYRUN) {
        printf("[pkgmgr] DRYRUN: would install DEB package %s %s\n",
               ctrl.package, ctrl.version);
        vfs_close(fd);
        return 0;
    }

    /* Create install directory */
    snprintf_local(install_dir, sizeof(install_dir),
                   "/usr/local/lib/%s", ctrl.package);
    vfs_mkdir(install_dir, 0755);

    /* ---- 3. Extract files using streaming (never loads entire file) ---- */
    size_t namelen = strlen(data_member.name);
    int extract_rc;

    if (namelen >= 3 &&
        memcmp(data_member.name + namelen - 3, ".gz", 3) == 0) {
        /* data.tar.gz – streaming gunzip + tar extraction */
        printf("[pkgmgr] Streaming decompression of %s (%lu bytes)\n",
               data_member.name, (unsigned long)data_member.size);
        extract_rc = stream_extract_tar_gz(fd, data_member.offset,
                                           data_member.size, "/usr/local");
    } else {
        /* Uncompressed data.tar – streaming tar extraction */
        extract_rc = stream_extract_tar(fd, data_member.offset,
                                        data_member.size, "/usr/local");
    }

    if (extract_rc < 0) {
        printf("[pkgmgr] Warning: failed to extract some files from DEB package: %s\n",
               ctrl.package);
        /* Continue anyway - partial install is still registered */
    }

    /* ---- 4. Register in the registry ---- */
    memset(&rec, 0, sizeof(rec));
    strncpy(rec.name, ctrl.package, sizeof(rec.name) - 1);
    strncpy(rec.version, ctrl.version, sizeof(rec.version) - 1);
    strncpy(rec.vendor, ctrl.maintainer, sizeof(rec.vendor) - 1);
    strncpy(rec.install_path, install_dir, sizeof(rec.install_path) - 1);
    strncpy(rec.pkg_format, "deb", sizeof(rec.pkg_format) - 1);
    strncpy(rec.pkg_id, ctrl.package, sizeof(rec.pkg_id) - 1);
    strncpy(rec.dependencies, ctrl.depends, sizeof(rec.dependencies) - 1);
    rec.install_size = (uint64_t)file_size;

    if (registry_register_software(&rec) < 0) {
        printf("[pkgmgr] Failed to register DEB package: %s\n", ctrl.package);
        vfs_close(fd);
        return -1;
    }

    printf("[pkgmgr] Installed DEB package: %s %s -> %s\n",
           ctrl.package, ctrl.version, install_dir);

    vfs_close(fd);
    return 0;
}

/* ========================================================================
 * pkgmgr_install_sfk - Install an SFK package
 * ======================================================================== */

int pkgmgr_install_sfk(const char *path, uint32_t flags, uint32_t *granted_perms)
{
    int fd;
    int64_t file_size;
    void *data = NULL;
    sfk_header_t hdr;
    reg_software_record_t rec;
    char install_dir[256];
    uint32_t actual_perms;

    fd = vfs_open(path, VFS_O_RDONLY, 0);
    if (fd < 0) {
        printf("[pkgmgr] Cannot open SFK file: %s\n", path);
        return -1;
    }

    /* Determine file size */
    vfs_inode_t stat;
    if (vfs_fstat(fd, &stat) < 0) {
        printf("[pkgmgr] Cannot stat SFK file: %s\n", path);
        vfs_close(fd);
        return -1;
    }
    file_size = stat.size;

    data = kmalloc(file_size);
    if (!data) {
        printf("[pkgmgr] Out of memory reading SFK file\n");
        vfs_close(fd);
        return -1;
    }

    if (vfs_read(fd, data, file_size) != file_size) {
        printf("[pkgmgr] Failed to read SFK file\n");
        kfree(data);
        vfs_close(fd);
        return -1;
    }
    vfs_close(fd);

    /* Parse SFK header */
    if (sfk_parse_header(data, file_size, &hdr) < 0) {
        printf("[pkgmgr] Failed to parse SFK header\n");
        kfree(data);
        return -1;
    }

    /* Determine granted permissions */
    if (granted_perms != NULL) {
        actual_perms = *granted_perms;
    } else {
        /* Use the package's requested permissions as granted */
        actual_perms = 0;
        sfk_perm_request_t perms[32];
        int nperms = sfk_get_perm_requests(data, file_size, perms, 32);
        for (int i = 0; i < nperms; i++) {
            actual_perms |= perms[i].perm_flag;
        }
    }

    if (flags & PKG_INSTALL_DRYRUN) {
        printf("[pkgmgr] DRYRUN: would install SFK package %s %s\n",
               hdr.pkg_id, hdr.pkg_version);
        kfree(data);
        return 0;
    }

    /* Create install directory: /opt/sfk/<pkg_id>/ */
    snprintf_local(install_dir, sizeof(install_dir),
                   "/opt/sfk/%s", hdr.pkg_id);
    vfs_mkdir("/opt/sfk", 0755);
    vfs_mkdir(install_dir, 0755);

    /* TODO: Extract files from data payload to install_dir */

    /* Register in the registry */
    memset(&rec, 0, sizeof(rec));
    strncpy(rec.name, hdr.pkg_name, sizeof(rec.name) - 1);
    strncpy(rec.version, hdr.pkg_version, sizeof(rec.version) - 1);
    strncpy(rec.vendor, hdr.vendor, sizeof(rec.vendor) - 1);
    strncpy(rec.install_path, install_dir, sizeof(rec.install_path) - 1);
    strncpy(rec.pkg_format, "sfk", sizeof(rec.pkg_format) - 1);
    strncpy(rec.pkg_id, hdr.pkg_id, sizeof(rec.pkg_id) - 1);
    rec.sfk_perms = actual_perms;
    rec.install_size = hdr.data_size;

    if (registry_register_software(&rec) < 0) {
        printf("[pkgmgr] Failed to register SFK package: %s\n", hdr.pkg_id);
        kfree(data);
        return -1;
    }

    /* Register declared registry entries */
    {
        sfk_reg_entry_t *entries = NULL;
        int entry_count = 0;
        if (sfk_get_reg_entries(data, file_size, &entries, &entry_count) == 0
            && entries != NULL && entry_count > 0) {
            for (int i = 0; i < entry_count; i++) {
                /* Create the registry key and write the value */
                registry_open_key(entries[i].key_path, 1);
                /* value_data is packed right after the sfk_reg_entry_t struct */
                const void *vdata = (const uint8_t *)&entries[i] + sizeof(sfk_reg_entry_t);
                registry_write_value(entries[i].key_path,
                                     entries[i].value_name,
                                     entries[i].value_type,
                                     vdata,
                                     entries[i].value_size);
            }
        }
    }

    /* Register declared file associations */
    {
        reg_fileassoc_t assocs[16];
        int nassocs = sfk_get_file_assocs(data, file_size, assocs, 16);
        for (int i = 0; i < nassocs; i++) {
            registry_register_fileassoc(&assocs[i]);
        }
    }

    /* Register declared services */
    if (hdr.nservices > 0 && hdr.meta_offset > 0 && hdr.meta_size > 0) {
        /* Parse service entries from metadata.
         * Layout after file_assocs: array of reg_service_t */
        const uint8_t *meta = (const uint8_t *)data + hdr.meta_offset;
        uint64_t offset = 0;

        /* Skip past perm requests, reg entries, and file assocs to reach services */
        offset += hdr.nperms * sizeof(sfk_perm_request_t);
        offset += hdr.nreg_entries * sizeof(sfk_reg_entry_t);
        /* Account for value data after each reg entry - approximate */
        for (uint32_t i = 0; i < hdr.nreg_entries; i++) {
            sfk_reg_entry_t *re = (sfk_reg_entry_t *)(meta + offset);
            offset += sizeof(sfk_reg_entry_t) + re->value_size;
        }
        offset += hdr.nfile_assocs * sizeof(reg_fileassoc_t);

        for (uint32_t i = 0; i < hdr.nservices && offset + sizeof(reg_service_t) <= hdr.meta_size; i++) {
            reg_service_t *svc = (reg_service_t *)(meta + offset);
            registry_register_service(svc);
            offset += sizeof(reg_service_t);
        }
    }

    printf("[pkgmgr] Installed SFK package: %s %s -> %s (perms=0x%x)\n",
           hdr.pkg_id, hdr.pkg_version, install_dir, actual_perms);

    kfree(data);
    return 0;
}

/* ========================================================================
 * pkgmgr_remove - Remove a package by ID
 * ======================================================================== */

int pkgmgr_remove(const char *pkg_id, uint32_t flags)
{
    reg_software_record_t rec;

    if (registry_get_software(pkg_id, &rec) < 0) {
        printf("[pkgmgr] Package not found: %s\n", pkg_id);
        return -1;
    }

    /* Remove sandbox policies if this was an SFK package */
    if (strcmp(rec.pkg_format, "sfk") == 0 && rec.sfk_perms != 0) {
        /* Walk through all processes and remove sandbox for those belonging
         * to this package. Stub: just log the intent. */
        printf("[pkgmgr] Removing sandbox policies for SFK package: %s\n", pkg_id);
    }

    /* Remove the registry entry */
    if (registry_unregister_software(pkg_id) < 0) {
        printf("[pkgmgr] Failed to unregister package: %s\n", pkg_id);
        return -1;
    }

    /* TODO: Actually delete files from install_path */

    printf("[pkgmgr] Removed package: %s\n", pkg_id);
    return 0;
}

/* ========================================================================
 * pkgmgr_query - Query package info
 * ======================================================================== */

int pkgmgr_query(const char *pkg_id, pkg_info_t *info)
{
    reg_software_record_t rec;

    if (registry_get_software(pkg_id, &rec) < 0) {
        return -1;
    }

    memset(info, 0, sizeof(*info));

    if (strcmp(rec.pkg_format, "deb") == 0) {
        info->format = PKG_FORMAT_DEB;
    } else if (strcmp(rec.pkg_format, "sfk") == 0) {
        info->format = PKG_FORMAT_SFK;
    }

    info->status = PKG_STATUS_INSTALLED;
    strncpy(info->pkg_id, rec.pkg_id, sizeof(info->pkg_id) - 1);
    strncpy(info->pkg_name, rec.name, sizeof(info->pkg_name) - 1);
    strncpy(info->version, rec.version, sizeof(info->version) - 1);
    strncpy(info->vendor, rec.vendor, sizeof(info->vendor) - 1);
    strncpy(info->description, "", sizeof(info->description) - 1);
    strncpy(info->install_path, rec.install_path, sizeof(info->install_path) - 1);
    info->install_size = rec.install_size;
    info->sfk_perms = rec.sfk_perms;
    strncpy(info->dependencies, rec.dependencies, sizeof(info->dependencies) - 1);

    return 0;
}

/* ========================================================================
 * pkgmgr_list - List all installed packages
 * ======================================================================== */

int pkgmgr_list(pkg_info_t *packages, int max_entries)
{
    char pkg_ids[128][64];
    int count;
    int i;

    count = registry_list_software(pkg_ids, max_entries > 128 ? 128 : max_entries);
    if (count < 0) {
        return 0;
    }

    for (i = 0; i < count && i < max_entries; i++) {
        if (pkgmgr_query(pkg_ids[i], &packages[i]) < 0) {
            /* Skip packages that fail to query */
            continue;
        }
    }

    return i;
}

/* ========================================================================
 * pkgmgr_is_installed - Check if a package is installed
 * ======================================================================== */

int pkgmgr_is_installed(const char *pkg_id)
{
    char key_path[256];
    snprintf_local(key_path, sizeof(key_path),
                   REG_ROOT_SYSTEM "/Software/%s", pkg_id);
    return registry_key_exists(key_path);
}

/* ========================================================================
 * pkgmgr_resolve_deps - Stub
 * ======================================================================== */

int pkgmgr_resolve_deps(const char *pkg_id, char missing[][64], int max_missing)
{
    /* Stub: dependency resolution not yet implemented */
    return 0;
}

/* ========================================================================
 * pkgmgr_verify - Stub
 * ======================================================================== */

int pkgmgr_verify(const char *pkg_id)
{
    /* Stub: package verification not yet implemented */
    return 0;
}

/* ========================================================================
 * sfk_parse_header - Parse SFK header from a .sfk file
 * ======================================================================== */

int sfk_parse_header(const void *data, size_t size, sfk_header_t *header)
{
    if (size < sizeof(sfk_header_t)) {
        return -1;
    }

    memcpy(header, data, sizeof(sfk_header_t));

    if (header->magic != SFK_MAGIC) {
        printf("[pkgmgr] Invalid SFK magic: 0x%x\n", header->magic);
        return -1;
    }

    if (header->version > SFK_VERSION) {
        printf("[pkgmgr] Unsupported SFK version: %u\n", header->version);
        return -1;
    }

    return 0;
}

/* ========================================================================
 * sfk_get_perm_requests - Parse permission requests from metadata section
 *
 * Metadata layout (starting at meta_offset):
 *   [0 .. nperms * sizeof(sfk_perm_request_t)]
 * ======================================================================== */

int sfk_get_perm_requests(const void *data, size_t size,
                          sfk_perm_request_t *perms, int max_perms)
{
    sfk_header_t hdr;
    int i;

    if (sfk_parse_header(data, size, &hdr) < 0) {
        return -1;
    }

    if (hdr.nperms == 0 || hdr.meta_offset == 0) {
        return 0;
    }

    int count = (int)hdr.nperms;
    if (count > max_perms) {
        count = max_perms;
    }

    const uint8_t *meta = (const uint8_t *)data + hdr.meta_offset;
    size_t avail = size - hdr.meta_offset;
    size_t needed = (size_t)count * sizeof(sfk_perm_request_t);

    if (needed > avail) {
        count = (int)(avail / sizeof(sfk_perm_request_t));
    }

    for (i = 0; i < count; i++) {
        memcpy(&perms[i], meta + i * sizeof(sfk_perm_request_t),
               sizeof(sfk_perm_request_t));
    }

    return count;
}

/* ========================================================================
 * sfk_get_reg_entries - Parse registry entries from metadata section
 *
 * After the permission requests, the registry entries follow.
 * Each sfk_reg_entry_t has value_data immediately following it.
 * ======================================================================== */

int sfk_get_reg_entries(const void *data, size_t size,
                        sfk_reg_entry_t **entries, int *count)
{
    sfk_header_t hdr;
    const uint8_t *meta;
    uint64_t offset;
    int i;

    if (sfk_parse_header(data, size, &hdr) < 0) {
        return -1;
    }

    if (hdr.nreg_entries == 0 || hdr.meta_offset == 0) {
        *count = 0;
        *entries = NULL;
        return 0;
    }

    meta = (const uint8_t *)data + hdr.meta_offset;

    /* Skip past permission requests */
    offset = hdr.nperms * sizeof(sfk_perm_request_t);

    /* Allocate buffer for entries */
    *count = (int)hdr.nreg_entries;
    *entries = (sfk_reg_entry_t *)kcalloc(*count, sizeof(sfk_reg_entry_t));
    if (!*entries) {
        return -1;
    }

    for (i = 0; i < *count; i++) {
        if (offset + sizeof(sfk_reg_entry_t) > hdr.meta_size) {
            break;
        }
        memcpy(&(*entries)[i], meta + offset, sizeof(sfk_reg_entry_t));
        offset += sizeof(sfk_reg_entry_t) + (*entries)[i].value_size;
    }

    *count = i;
    return 0;
}

/* ========================================================================
 * sfk_get_file_assocs - Parse file associations from metadata section
 *
 * After the registry entries (and their value data), file associations follow.
 * ======================================================================== */

int sfk_get_file_assocs(const void *data, size_t size,
                        reg_fileassoc_t *assocs, int max_assocs)
{
    sfk_header_t hdr;
    const uint8_t *meta;
    uint64_t offset;
    int i;

    if (sfk_parse_header(data, size, &hdr) < 0) {
        return -1;
    }

    if (hdr.nfile_assocs == 0 || hdr.meta_offset == 0) {
        return 0;
    }

    meta = (const uint8_t *)data + hdr.meta_offset;

    /* Skip past permission requests */
    offset = hdr.nperms * sizeof(sfk_perm_request_t);

    /* Skip past registry entries (including their value data) */
    for (uint32_t j = 0; j < hdr.nreg_entries; j++) {
        if (offset + sizeof(sfk_reg_entry_t) > hdr.meta_size) {
            return 0;
        }
        sfk_reg_entry_t re;
        memcpy(&re, meta + offset, sizeof(sfk_reg_entry_t));
        offset += sizeof(sfk_reg_entry_t) + re.value_size;
    }

    /* Read file associations */
    int count = (int)hdr.nfile_assocs;
    if (count > max_assocs) {
        count = max_assocs;
    }

    for (i = 0; i < count; i++) {
        if (offset + sizeof(reg_fileassoc_t) > hdr.meta_size) {
            break;
        }
        memcpy(&assocs[i], meta + offset, sizeof(reg_fileassoc_t));
        offset += sizeof(reg_fileassoc_t);
    }

    return i;
}
