#include "drm.h"
#include "drm_gem.h"
#include "chardev.h"
#include "memory.h"
#include "string.h"
#include "vga.h"
#include "timer.h"
#include "process.h"
#include "mmu.h"
#include "serial.h"

/* mmap protection flags (与 syscall_mem.c 保持一致) */
#define PROT_READ     0x1
#define PROT_WRITE    0x2

/* ========================================================================
 * DRM Core - Direct Rendering Manager 核心层
 *
 * 管理 DRM 设备、ioctl 调度、事件读取和 chardev 注册。
 * ======================================================================== */

/* ---------- ioctl 命令号提取宏 (标准 Linux 编码) ---------- */

#define DRM_IOC_NR(cmd)    (((cmd) >> _IOC_NRSHIFT)  & _IOC_NRMASK)
#define DRM_IOC_TYPE(cmd)  (((cmd) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)

/* ---------- 全局 DRM 设备实例 ---------- */

drm_device_t g_drm_dev;

/* ---------- Poll 事件常量 ---------- */
#define POLLIN  0x001

/* ========================================================================
 * DRM 字符设备操作
 * ======================================================================== */

static int drm_open(vfs_file_t *file)
{
    (void)file;
    return 0;
}

static int drm_close(vfs_file_t *file)
{
    (void)file;
    return 0;
}

static int drm_read(vfs_file_t *file, void *buf, size_t count)
{
    (void)file;

    /* 处理 pending vblank（完成页面翻转） */
    drm_kms_handle_vblank();

    if (g_drm_dev.event_count == 0)
        return 0;  /* 无事件可读 */

    /* 读取一个事件 */
    drm_event_vblank_t *evt = &g_drm_dev.events[g_drm_dev.event_head];
    size_t evt_size = sizeof(*evt);
    size_t to_copy = count < evt_size ? count : evt_size;
    memcpy(buf, evt, to_copy);

    g_drm_dev.event_head = (g_drm_dev.event_head + 1) % DRM_EVENT_QUEUE_SIZE;
    g_drm_dev.event_count--;

    return (int)to_copy;
}

static int drm_write(vfs_file_t *file, const void *buf, size_t count)
{
    (void)file;
    (void)buf;
    (void)count;
    return -1;  /* DRM 设备不可写 */
}

static int64_t drm_ioctl(vfs_file_t *file, uint64_t cmd, uint64_t arg)
{
    (void)file;

    /* 用户空间 ioctl 命令可能被符号扩展到 64 位（如 0xc03864a0 → 0xffffffffc03864a0），
     * 截断为 32 位以确保正确解码。 */
    cmd &= 0xFFFFFFFFU;

    /* 验证 ioctl 类型 */
    if (DRM_IOC_TYPE(cmd) != DRM_IOCTL_BASE)
        return -1;

    uint8_t nr = (uint8_t)DRM_IOC_NR(cmd);

    switch (nr) {
    /* ---- 核心 ioctl ---- */
    case DRM_IOCTL_VERSION: {
        drm_version_t *v = (drm_version_t *)arg;
        v->version_major  = 1;
        v->version_minor  = 0;
        v->version_patchlevel = 0;

        const char *name = "bochs";
        size_t name_len = strlen(name);
        if (v->name && v->name_len > 0) {
            size_t cpy = name_len < (size_t)v->name_len ? name_len : (size_t)v->name_len;
            memcpy(v->name, name, cpy);
        }
        v->name_len = (size_t)name_len;

        const char *date = "20260803";
        size_t date_len = strlen(date);
        if (v->date && v->date_len > 0) {
            size_t cpy = date_len < (size_t)v->date_len ? date_len : (size_t)v->date_len;
            memcpy(v->date, date, cpy);
        }
        v->date_len = (size_t)date_len;

        const char *desc = "SpiritFoxOS Bochs DRM";
        size_t desc_len = strlen(desc);
        if (v->desc && v->desc_len > 0) {
            size_t cpy = desc_len < (size_t)v->desc_len ? desc_len : (size_t)v->desc_len;
            memcpy(v->desc, desc, cpy);
        }
        v->desc_len = (size_t)desc_len;

        return 0;
    }

    case DRM_IOCTL_GET_CAP: {
        drm_get_cap_t *cap = (drm_get_cap_t *)arg;
        switch (cap->capability) {
        case DRM_CAP_DUMB_BUFFER:
            cap->value = 1;
            return 0;
        case DRM_CAP_DUMB_PREFERRED_DEPTH:
            cap->value = 32;
            return 0;
        case DRM_CAP_DUMB_PREFER_SHADOW:
            cap->value = 0;
            return 0;
        case DRM_CAP_VBLANK_HIGH_CRTC:
            cap->value = 1;
            return 0;
        case DRM_CAP_PRIME:
            cap->value = 0;
            return 0;
        default:
            cap->value = 0;
            return -1;
        }
    }

    case DRM_IOCTL_SET_CLIENT_CAP: {
        /* 接受但不做额外处理 */
        return 0;
    }

    case DRM_IOCTL_MODESET_CTL: {
        /* Modeset 控制 - 接受但不做额外处理 */
        return 0;
    }

    /* ---- GEM ioctl ---- */
    case DRM_IOCTL_GEM_CLOSE: {
        drm_gem_close_t *gc = (drm_gem_close_t *)arg;
        drm_gem_close(&g_drm_dev.gem_manager, gc->handle);
        return 0;
    }

    case DRM_IOCTL_GEM_FLINK: {
        drm_gem_flink_t *gf = (drm_gem_flink_t *)arg;
        drm_gem_object_t *obj = drm_gem_lookup(&g_drm_dev.gem_manager, gf->handle);
        if (!obj) return -1;
        if (obj->name == 0) {
            obj->name = g_drm_dev.gem_manager.next_gem_name++;
        }
        gf->name = obj->name;
        return 0;
    }

    case DRM_IOCTL_GEM_OPEN: {
        drm_gem_open_t *go = (drm_gem_open_t *)arg;
        /* 简化实现：线性扫描查找名称匹配的对象 */
        for (int i = 0; i < 64; i++) {
            drm_gem_object_t *obj = &g_drm_dev.gem_manager.objects[i];
            if (obj->refcount > 0 && obj->name == go->name) {
                obj->refcount++;
                go->handle = obj->handle;
                go->size   = obj->size;
                return 0;
            }
        }
        return -1;
    }

    /* ---- Mode Setting ioctl ---- */
    case DRM_IOCTL_MODE_GETRESOURCES:
        return drm_kms_get_resources(arg);

    case DRM_IOCTL_MODE_GETCRTC:
        return drm_kms_get_crtc(arg);

    case DRM_IOCTL_MODE_SETCRTC:
        return drm_kms_set_crtc(arg);

    case DRM_IOCTL_MODE_GETENCODER:
        return drm_kms_get_encoder(arg);

    case DRM_IOCTL_MODE_GETCONNECTOR:
        return drm_kms_get_connector(arg);

    case DRM_IOCTL_MODE_ADDFB:
        return drm_kms_addfb(arg);

    case DRM_IOCTL_MODE_ADDFB2:
        return drm_kms_addfb2(arg);

    case DRM_IOCTL_MODE_RMFB:
        return drm_kms_rmfb(arg);

    case DRM_IOCTL_MODE_PAGE_FLIP:
        return drm_kms_page_flip(arg);

    case DRM_IOCTL_MODE_CREATE_DUMB:
        return drm_kms_create_dumb(arg);

    case DRM_IOCTL_MODE_MAP_DUMB:
        return drm_kms_map_dumb(arg);

    case DRM_IOCTL_MODE_DESTROY_DUMB:
        return drm_kms_destroy_dumb(arg);

    case DRM_IOCTL_MODE_GETFB:
        /* 未实现 */
        return -1;

    case DRM_IOCTL_MODE_DIRTYFB:
        /* 标记脏帧 - 接受但不做额外处理 */
        return 0;

    case DRM_IOCTL_MODE_ATOMIC:
        /* 未实现 */
        return -1;

    default:
        return -1;
    }
}

static int drm_mmap(vfs_file_t *file, uint64_t addr, size_t len,
                    int prot, int flags, uint64_t offset)
{
    (void)file;
    (void)flags;

    /* 根据 mmap offset 查找 GEM 对象 */
    for (int i = 0; i < 64; i++) {
        drm_gem_object_t *obj = &g_drm_dev.gem_manager.objects[i];
        if (obj->refcount > 0 && obj->mmap_offset != 0 &&
            offset >= obj->mmap_offset &&
            offset < obj->mmap_offset + obj->size) {

            uint64_t off_in_obj = offset - obj->mmap_offset;
            size_t map_len = len;
            if (off_in_obj + len > obj->size)
                map_len = (size_t)(obj->size - off_in_obj);

            /* 将 GEM 对象的物理页面映射到用户虚拟地址空间。
             * 这样用户空间对 mmap 区域的写入直接反映到 GEM 缓冲区，
             * 无需额外的 memcpy 或 ioctl 刷新。 */
            process_t *proc = process_current();
            if (!proc || !obj->vaddr)
                return -1;

            uint64_t pte_flags = PTE_PRESENT | PTE_USER;
            if (prot & PROT_WRITE)
                pte_flags |= PTE_WRITABLE;
            /* DRM 帧缓冲不需要缓存（避免 DMA 一致性问题） */
            pte_flags |= PTE_PCD | PTE_PWT;

            /* 计算起始物理地址（GEM 对象物理地址 + 偏移） */
            uint64_t gem_phys = obj->paddr + off_in_obj;
            size_t map_pages = (map_len + PAGE_SIZE - 1) / PAGE_SIZE;

            for (size_t p = 0; p < map_pages; p++) {
                uint64_t virt = addr + p * PAGE_SIZE;
                uint64_t phys = (gem_phys + p * PAGE_SIZE) & ~(uint64_t)(PAGE_SIZE - 1);

                /* 如果 GEM 对象使用非连续页面，回退到 vaddr 转换 */
                if (obj->paddr == 0) {
                    /* 逐页映射：通过内核虚拟地址获取物理地址 */
                    uint64_t *src_vaddr = (uint64_t *)((uintptr_t)obj->vaddr + off_in_obj + p * PAGE_SIZE);
                    (void)src_vaddr; /* 暂时不可用 */
                    return -1;
                }

                if (mmu_map_page(proc->pml4, virt, phys, pte_flags) < 0) {
                    serial_puts("[drm_mmap] mmu_map_page FAILED\n");
                    return -1;
                }
            }

            return 0;
        }
    }

    return -1;
}

static int drm_poll(vfs_file_t *file, int events)
{
    (void)file;
    (void)events;

    /* 处理 pending vblank */
    drm_kms_handle_vblank();

    if (g_drm_dev.event_count > 0)
        return POLLIN;

    return 0;
}

/* ---------- DRM 字符设备操作表 ---------- */

static const chardev_ops_t drm_chardev_ops = {
    .open  = drm_open,
    .close = drm_close,
    .read  = drm_read,
    .write = drm_write,
    .ioctl = drm_ioctl,
    .mmap  = drm_mmap,
    .poll  = drm_poll,
};

/* ========================================================================
 * DRM 初始化
 * ======================================================================== */

void drm_init(void)
{
    printf("[DRM] Initializing...\n");

    /* 初始化 GEM 管理器 */
    drm_gem_manager_init(&g_drm_dev.gem_manager);

    /* 初始化 KMS 对象 */
    drm_kms_init();

    /* 初始化事件队列 */
    g_drm_dev.event_head  = 0;
    g_drm_dev.event_tail  = 0;
    g_drm_dev.event_count = 0;
    g_drm_dev.vblank_seq  = 0;

    /* 初始化 Bochs VBE 驱动 */
    bochs_drm_init();

    /* 注册字符设备操作表 */
    chardev_register(CHARDEV_ID_DRM_CARD0,  &drm_chardev_ops);
    chardev_register(CHARDEV_ID_DRM_CTRL,   &drm_chardev_ops);
    chardev_register(CHARDEV_ID_DRM_RENDER, &drm_chardev_ops);

    /* 创建 /dev/dri/ 设备节点 */
    chardev_create_node("/dev/dri/card0",
                        CHARDEV_ID_DRM_CARD0,
                        VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IRGRP | VFS_S_IWGRP);
    chardev_create_node("/dev/dri/controlD64",
                        CHARDEV_ID_DRM_CTRL,
                        VFS_S_IRUSR | VFS_S_IWUSR);
    chardev_create_node("/dev/dri/renderD128",
                        CHARDEV_ID_DRM_RENDER,
                        VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IRGRP | VFS_S_IWGRP);

    printf("[DRM] Initialized (bochs VBE, 1 CRTC, 1 connector)\n");
}
