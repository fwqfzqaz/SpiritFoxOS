#include "hal.h"
#include "vga.h"
#include "drm_gem.h"
#include "pci.h"
#include "serial.h"

/* ========================================================================
 * Bochs / QEMU VBE DRM 驱动
 *
 * 使用 VBE DISPI 端口接口 (0x1CE/0x1CF) 控制显示模式。
 * 提供 LFB 帧缓冲基地址和双缓冲页面翻转支持。
 * ======================================================================== */

/* ---------- VBE DISPI 寄存器 ---------- */
#define VBE_DISPI_IOPORT_INDEX  0x1CE
#define VBE_DISPI_IOPORT_DATA   0x1CF

#define VBE_DISPI_INDEX_ID          0x0
#define VBE_DISPI_INDEX_XRES        0x1
#define VBE_DISPI_INDEX_YRES        0x2
#define VBE_DISPI_INDEX_BPP         0x3
#define VBE_DISPI_INDEX_ENABLE      0x4
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x6
#define VBE_DISPI_INDEX_X_OFFSET    0x8
#define VBE_DISPI_INDEX_Y_OFFSET    0x9

#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED   0x40
#define VBE_DISPI_NOCLEARMEM    0x80

/* ---------- 驱动状态 ---------- */
static int      bochs_vbe_available = 0;
static uint16_t bochs_cur_xres = 0;
static uint16_t bochs_cur_yres = 0;
static uint16_t bochs_cur_bpp  = 0;
static uint64_t bochs_lfb_addr = 0;  /* 从 PCI BAR0 动态获取 */

/* ---------- 16-bit 端口 I/O 辅助 ---------- */

static inline void vbe_write(uint16_t index, uint16_t val)
{
    hal_outw(VBE_DISPI_IOPORT_INDEX, index);
    hal_outw(VBE_DISPI_IOPORT_DATA, val);
}

static inline uint16_t vbe_read(uint16_t index)
{
    hal_outw(VBE_DISPI_IOPORT_INDEX, index);
    return hal_inw(VBE_DISPI_IOPORT_DATA);
}

/* ---------- 从 PCI 配置空间读取 VGA 设备的 BAR0 地址 ---------- */
static uint64_t bochs_detect_lfb_addr(void)
{
    /* 扫描 PCI 总线寻找 VGA 兼容设备 (class 0x030000 或 0x030001) */
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t vendor_dev = pci_read_config(bus, dev, func, 0x00);
                if (vendor_dev == 0xFFFFFFFF)
                    continue;

                uint32_t class_rev = pci_read_config(bus, dev, func, 0x08);
                uint8_t base_class = (class_rev >> 24) & 0xFF;
                uint8_t sub_class  = (class_rev >> 16) & 0xFF;

                /* VGA 兼容设备: class=0x03, subclass=0x00 */
                if (base_class == 0x03 && sub_class == 0x00) {
                    uint32_t bar0 = pci_read_config(bus, dev, func, 0x10);
                    if (!(bar0 & 0x01)) {  /* 必须是内存空间 BAR */
                        uint64_t addr = (uint64_t)(bar0 & 0xFFFFFFF0);
                        /* 对于 64 位 BAR，读取高 32 位 */
                        if (bar0 & 0x04) {
                            uint32_t bar0_hi = pci_read_config(bus, dev, func, 0x14);
                            addr |= ((uint64_t)bar0_hi << 32);
                        }
                        serial_puts("[bochs_drm] VGA found at PCI ");
                        serial_put_dec((uint64_t)bus);
                        serial_puts(":");
                        serial_put_dec((uint64_t)dev);
                        serial_puts(".");
                        serial_put_dec((uint64_t)func);
                        serial_puts(" BAR0=0x");
                        serial_put_hex(addr);
                        serial_puts("\n");
                        return addr;
                    }
                }
            }
        }
    }

    serial_puts("[bochs_drm] No VGA device found in PCI, using default 0xFD000000\n");
    return 0xFD000000ULL;
}

/* ---------- 驱动接口 ---------- */

int bochs_drm_init(void)
{
    /* 检测 VBE DISPI 是否可用 */
    uint16_t id = vbe_read(VBE_DISPI_INDEX_ID);

    if (id == 0xFFFF || id < 0xB0C4) {
        bochs_vbe_available = 0;
        printf("[bochs_drm] VBE DISPI not available (id=0x%04x)\n", id);
        return -1;
    }

    bochs_vbe_available = 1;

    /* 读取当前模式信息 */
    bochs_cur_xres = vbe_read(VBE_DISPI_INDEX_XRES);
    bochs_cur_yres = vbe_read(VBE_DISPI_INDEX_YRES);
    bochs_cur_bpp  = vbe_read(VBE_DISPI_INDEX_BPP);

    /* 从 PCI BAR0 获取 LFB 物理地址 */
    bochs_lfb_addr = bochs_detect_lfb_addr();

    printf("[bochs_drm] VBE DISPI detected (id=0x%04x), mode=%ux%u@%ubpp, LFB=0x%llx\n",
           id, bochs_cur_xres, bochs_cur_yres, bochs_cur_bpp,
           (unsigned long long)bochs_lfb_addr);

    return 0;
}

int bochs_drm_set_mode(uint32_t width, uint32_t height, uint32_t bpp)
{
    if (!bochs_vbe_available) return -1;

    /* 先禁用 VBE（切换模式时必须） */
    vbe_write(VBE_DISPI_INDEX_ENABLE, 0);

    /* 设置分辨率 */
    vbe_write(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    vbe_write(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    vbe_write(VBE_DISPI_INDEX_BPP,  (uint16_t)bpp);

    /* 设置虚拟宽度 = 实际宽度（用于双缓冲行偏移计算） */
    vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)width);

    /* 重置显示偏移 */
    vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);

    /* 启用 VBE：线性帧缓冲 + 不清除显存 */
    vbe_write(VBE_DISPI_INDEX_ENABLE,
              VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM);

    bochs_cur_xres = (uint16_t)width;
    bochs_cur_yres = (uint16_t)height;
    bochs_cur_bpp  = (uint16_t)bpp;

    /* 确保 LFB 地址空间已映射且禁用缓存（MMIO） */
    size_t fb_size = (size_t)width * height * (bpp / 8);
    hal_ensure_mapped_mmio((uintptr_t)bochs_lfb_addr, fb_size);

    serial_puts("[bochs_drm] Set mode ");
    serial_put_dec((uint64_t)width);
    serial_puts("x");
    serial_put_dec((uint64_t)height);
    serial_puts("@");
    serial_put_dec((uint64_t)bpp);
    serial_puts("bpp LFB=0x");
    serial_put_hex(bochs_lfb_addr);
    serial_puts("\n");
    return 0;
}

uint64_t bochs_drm_get_lfb_addr(void)
{
    return bochs_lfb_addr;
}

void bochs_drm_page_flip(uint64_t offset)
{
    if (!bochs_vbe_available) return;

    /* 将字节偏移转换为 Y 扫描线偏移。
     * pitch = xres * (bpp / 8)，y_offset = offset / pitch */
    uint32_t pitch = (uint32_t)bochs_cur_xres * (bochs_cur_bpp / 8);
    uint16_t y_off = 0;
    if (pitch > 0)
        y_off = (uint16_t)(offset / pitch);

    vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    vbe_write(VBE_DISPI_INDEX_Y_OFFSET, y_off);
}
