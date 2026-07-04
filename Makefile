# SpiritFoxOS - Makefile
# 灵狐操作系统构建系统

.PHONY: all clean kernel loader image iso run run-iso run-debug logo

# =========================================
# 工具链配置
# =========================================
CROSS   ?= x86_64-linux-gnu-
CC      = $(CROSS)gcc
LD      = $(CROSS)ld
NASM    = nasm
OBJCOPY = $(CROSS)objcopy

# =========================================
# 目录配置
# =========================================
KERNEL_DIR     = kernel
KERNEL_SRC     = $(KERNEL_DIR)/src
KERNEL_INC     = $(KERNEL_DIR)/include
KERNEL_BUILD   = build/kernel

LOADER_DIR     = boot/multiboot
LOADER_BUILD   = build/loader

ISO_DIR        = build/iso

# =========================================
# 64-bit 内核编译选项
# =========================================
KERNEL_CFLAGS  = -ffreestanding -nostdlib \
                 -fno-builtin -fno-stack-protector \
                 -fno-pic -fno-pie -mno-red-zone \
                 -mno-mmx -mno-sse -mno-sse2 \
                 -mcmodel=small \
                 -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
                 -I$(KERNEL_INC)

KERNEL_LDFLAGS = -T $(KERNEL_SRC)/kernel.ld -nostdlib -static

# =========================================
# 64-bit 内核源文件
# =========================================
KERNEL_C_SRCS    := $(shell find $(KERNEL_SRC) -name '*.c' -type f)
KERNEL_ASM_SRCS  := $(shell find $(KERNEL_SRC) -name '*.S' -type f)

KERNEL_C_OBJS    = $(patsubst $(KERNEL_SRC)/%.c,$(KERNEL_BUILD)/%.o,$(KERNEL_C_SRCS))
KERNEL_ASM_OBJS  = $(patsubst $(KERNEL_SRC)/%.S,$(KERNEL_BUILD)/%.o,$(KERNEL_ASM_SRCS))

KERNEL_OBJS      = $(KERNEL_C_OBJS) $(KERNEL_ASM_OBJS)

# =========================================
# Logo data (PNG → C header)
# =========================================
LOGO_PNG       = logo.png
LOGO_HEADER    = $(KERNEL_INC)/logo_data.h
LOGO_MAX_SIZE  ?= 200

$(LOGO_HEADER): $(LOGO_PNG) tools/png2c.py
	@echo "[LOGO] Generating $@"
	@python3 tools/png2c.py $(LOGO_PNG) $@ $(LOGO_MAX_SIZE)

logo: $(LOGO_HEADER)

# =========================================
# 默认目标
# =========================================
all: iso

# =========================================
# 64-bit 内核构建
# =========================================
$(KERNEL_BUILD):
	@mkdir -p $(KERNEL_BUILD)

$(KERNEL_BUILD)/%.o: $(KERNEL_SRC)/%.c | $(KERNEL_BUILD)
	@mkdir -p $(dir $@)
	@echo "[CC64] $<"
	@if echo "$<" | grep -q "nuklear_fb"; then \
		$(CC) $(KERNEL_CFLAGS) -msse -msse2 -mfpmath=sse -c $< -o $@; \
	else \
		$(CC) $(KERNEL_CFLAGS) -c $< -o $@; \
	fi

$(KERNEL_BUILD)/%.o: $(KERNEL_SRC)/%.S | $(KERNEL_BUILD)
	@mkdir -p $(dir $@)
	@echo "[NASM64] $<"
	@$(NASM) -f elf64 $< -o $@

$(KERNEL_BUILD)/kernel.elf: $(KERNEL_OBJS) $(KERNEL_SRC)/kernel.ld
	@echo "[LD64] $@"
	@$(LD) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS)

$(KERNEL_BUILD)/kernel.bin: $(KERNEL_BUILD)/kernel.elf
	@echo "[BIN] $@"
	@$(OBJCOPY) -O binary $< $@

kernel: logo $(KERNEL_BUILD)/kernel.bin

# =========================================
# 32-bit multiboot 加载器构建
# =========================================
LOADER_CFLAGS  = -m32 -ffreestanding -nostdlib -fno-builtin \
                 -fno-stack-protector -fno-pic -fno-pie \
                 -Wall -Wextra

LOADER_LDFLAGS = -m elf_i386 -T $(LOADER_DIR)/linker.ld -nostdlib -static

$(LOADER_BUILD):
	@mkdir -p $(LOADER_BUILD)

$(LOADER_BUILD)/multiboot32.o: $(LOADER_DIR)/multiboot32.c | $(LOADER_BUILD)
	@echo "[CC32] $<"
	@gcc $(LOADER_CFLAGS) -c $< -o $@

$(LOADER_BUILD)/boot32.o: $(LOADER_DIR)/boot32.S | $(LOADER_BUILD)
	@echo "[AS32] $<"
	@gcc -m32 -c $< -o $@

$(LOADER_BUILD)/loader.elf: $(LOADER_BUILD)/multiboot32.o $(LOADER_BUILD)/boot32.o $(LOADER_DIR)/linker.ld
	@echo "[LD32] $@"
	@ld $(LOADER_LDFLAGS) -o $@ $(LOADER_BUILD)/multiboot32.o $(LOADER_BUILD)/boot32.o

loader: $(LOADER_BUILD)/loader.elf

# =========================================
# GRUB ISO 镜像
# =========================================
ISO_FILE = build/spiritfox.iso

iso: kernel loader
	@echo "[ISO] Building GRUB bootable ISO..."
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp $(LOADER_BUILD)/loader.elf $(ISO_DIR)/boot/loader.elf
	@cp $(KERNEL_BUILD)/kernel.bin $(ISO_DIR)/boot/kernel.bin
	@echo 'serial --speed=115200 --unit=0 --word=8 --parity=no --stop=1' > $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'terminal_input serial console' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'terminal_output serial console' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'set timeout=1' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'set default=0' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "SpiritFoxOS" {' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '  echo "Loading SpiritFoxOS..."' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '  multiboot2 /boot/loader.elf' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '  module2 /boot/kernel.bin' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}' >> $(ISO_DIR)/boot/grub/grub.cfg
	@grub-mkrescue -o $(ISO_FILE) $(ISO_DIR) 2>/dev/null
	@echo "========================================="
	@echo "  SpiritFoxOS ISO created: $(ISO_FILE)"
	@echo "========================================="

# =========================================
# UEFI 磁盘镜像 (需要 gnu-efi，暂不可用)
# =========================================
BOOT_DIR       = boot/uefi
BOOT_BUILD     = build/boot
IMAGE_DIR      = build/disk
EFI_DIR        = $(IMAGE_DIR)/EFI/Boot
OS_DIR         = $(IMAGE_DIR)/EFI/SpiritFoxOS
IMAGE_FILE     = build/spiritfox.img

# =========================================
# QEMU 运行
# =========================================
run-iso: iso build/disk.img build/fat32.img
	qemu-system-x86_64 \
	    -cdrom $(ISO_FILE) \
	    -boot d \
	    -m 1G \
	    -serial stdio \
	    -netdev user,id=net0,hostfwd=tcp::25565-:25565 \
	    -device rtl8139,netdev=net0 \
	    -no-reboot \
	    -vga std \
	    -device ahci,id=ahci0 \
	    -drive id=fat32disk,file=build/fat32.img,if=none,format=raw \
	    -device ide-hd,drive=fat32disk,bus=ahci0.0 \
	    -drive id=disk0,file=build/disk.img,if=none,format=raw \
	    -device ide-hd,drive=disk0,bus=ahci0.1

run: run-iso

run-log: iso
	qemu-system-x86_64 \
	    -cdrom $(ISO_FILE) \
	    -m 256M \
	    -serial file:build/serial.log \
	    -display none \
	    -net none \
	    -no-reboot \
	    -d cpu_reset 2>build/qemu_debug.log

run-debug: iso
	qemu-system-x86_64 \
	    -cdrom $(ISO_FILE) \
	    -m 256M \
	    -serial stdio \
	    -net none \
	    -S -gdb tcp::1234

# =========================================
# QEMU disk images (auto-created if missing)
# =========================================
build/disk.img:
	@dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null

build/fat32.img:
	@echo "[FAT32] Creating disk image..."
	@dd if=/dev/zero of=$@ bs=1M count=512 2>/dev/null
	@mkfs.fat -F 32 $@ 2>/dev/null
	@mmd -i $@ ::bin 2>/dev/null
	@mmd -i $@ ::pkg 2>/dev/null
	@if [ -f /tmp/test_hello ]; then \
		mcopy -i $@ /tmp/test_hello ::HELLO 2>/dev/null; \
		mcopy -i $@ /tmp/test_hello ::bin/test_hello 2>/dev/null; \
	fi
	@if [ -f cs/java-jre.deb ]; then \
		if mcopy -i $@ cs/java-jre.deb ::pkg/java-jre.deb; then \
			echo "[FAT32] Added java-jre.deb"; \
		else \
			echo "[FAT32] ERROR: Failed to copy java-jre.deb (disk full?)"; \
		fi; \
	else \
		echo "[FAT32] WARNING: cs/java-jre.deb not found (run tools/build_jre_deb.sh)"; \
	fi
	@if [ -f cs/test-pkg.deb ]; then \
		mcopy -i $@ cs/test-pkg.deb ::pkg/test-pkg.deb; \
		echo "[FAT32] Added test-pkg.deb"; \
	fi
	@if [ -f cs/server.jar ]; then \
		if mcopy -i $@ cs/server.jar ::server.jar; then \
			echo "[FAT32] Added server.jar"; \
		else \
			echo "[FAT32] ERROR: Failed to copy server.jar (disk full?)"; \
		fi; \
	fi
	@echo "[FAT32] Image created (512MB)"

# =========================================
# 清理
# =========================================
clean:
	@echo "[CLEAN] Removing build artifacts..."
	@rm -rf build/
	@rm -f $(LOGO_HEADER)
	@echo "Done."
