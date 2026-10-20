# SpiritFoxOS - Makefile
# 灵狐操作系统构建系统 (Linux EFI Stub 模型)

.PHONY: all clean kernel logo uefi-stub uefi-image run run-uefi run-log run-debug \
        libc libc-clean ap-trampoline test-elf \
        musl-static musl-dynamic musl-test musl-deploy \
        wayland-protocol wayland-libs wayland-compositor wayland-client wayland-all wayland-deploy \
        usb

# =========================================
# 工具链配置
# =========================================
CROSS   ?= $(shell for tool in gcc ld objcopy ar ranlib; do command -v x86_64-linux-gnu-$$tool >/dev/null 2>&1 || exit 0; done; printf '%s' x86_64-linux-gnu-)
CC      = $(CROSS)gcc
LD      = $(CROSS)ld
NASM    = nasm
OBJCOPY = $(CROSS)objcopy
HOST_CC ?= cc

# =========================================
# 目录配置
# =========================================
KERNEL_DIR     = kernel
KERNEL_SRC     = $(KERNEL_DIR)/src
KERNEL_INC     = $(KERNEL_DIR)/include
KERNEL_BUILD   = build/kernel

# EFI Stub (Linux bzImage 式: stub + 内嵌 kernel.elf → kernel.efi)
EFI_STUB_DIR   = boot/efi_stub
EFI_BUILD      = build/efi_stub
KERNEL_EFI     = $(EFI_BUILD)/kernel.efi

IMAGE_DIR      = build/disk
EFI_DIR        = $(IMAGE_DIR)/EFI/Boot
IMAGE_FILE     = build/spiritfox.img

OVMF_CODE      ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE.fd /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/edk2/ovmf/OVMF_CODE.fd))
OVMF_VARS      ?= $(subst OVMF_CODE,OVMF_VARS,$(OVMF_CODE))

# =========================================
# 64-bit 内核编译选项
# =========================================
KERNEL_CFLAGS  = -std=gnu11 -ffreestanding -nostdlib \
                 -fno-builtin -fno-stack-protector \
                 -fno-pic -fno-pie -mno-red-zone \
                 -mno-mmx -mno-sse -mno-sse2 \
                 -mcmodel=small \
                 -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
                 -I$(KERNEL_INC) -MMD -MP

KERNEL_LDFLAGS = -T $(KERNEL_SRC)/kernel.ld -nostdlib -static

# =========================================
# 64-bit 内核源文件
# =========================================
KERNEL_C_SRCS    := $(shell find $(KERNEL_SRC) -name '*.c' -type f)
# Exclude ap_trampoline.S from kernel build - it's built separately as raw binary
KERNEL_ASM_SRCS  := $(shell find $(KERNEL_SRC) -name '*.S' -type f ! -name 'ap_trampoline.S')

KERNEL_C_OBJS    = $(patsubst $(KERNEL_SRC)/%.c,$(KERNEL_BUILD)/%.o,$(KERNEL_C_SRCS))
KERNEL_ASM_OBJS  = $(patsubst $(KERNEL_SRC)/%.S,$(KERNEL_BUILD)/%.o,$(KERNEL_ASM_SRCS))

CORE_C_SRCS     := $(shell find kernel/core -name '*.c' -type f)
CORE_C_OBJS      = $(patsubst kernel/core/%.c,$(KERNEL_BUILD)/core/%.o,$(CORE_C_SRCS))
KERNEL_OBJS      = $(KERNEL_C_OBJS) $(KERNEL_ASM_OBJS) $(CORE_C_OBJS) $(KERNEL_BUILD)/core/boot/package.o

.DEFAULT_GOAL := all
-include $(KERNEL_C_OBJS:.o=.d)
-include $(CORE_C_OBJS:.o=.d)

# =========================================
# 默认目标 (必须是 Makefile 中第一个目标)
# =========================================
all: uefi-image

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
# 64-bit 内核构建 (产物 kernel.elf 既是调试符号, 又是 EFI stub payload)
# =========================================
$(KERNEL_BUILD):
	@mkdir -p $(KERNEL_BUILD)

build:
	@mkdir -p $@

$(KERNEL_BUILD)/video/window.o: $(LOGO_HEADER)

$(KERNEL_BUILD)/proc/process_user.o: KERNEL_CFLAGS += -Werror=frame-larger-than=4096

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

$(KERNEL_BUILD)/core/%.o: kernel/core/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -Werror -Wframe-larger-than=2048 -c $< -o $@

$(KERNEL_BUILD)/core/boot/package.o: kernel/core/boot/package.S build/modules/init.sfpack
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

# 单独编译/链接模块；不链接内核或宿主 libc。
SFM_FLAGS = -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -fno-pic -fno-pie -fno-common -fno-asynchronous-unwind-tables \
            -fno-unwind-tables -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
            -Wall -Wextra -Werror -Ikernel/include
SFM_NAMES := $(shell sed -e '/^[[:space:]]*\#/d' -e '/^[[:space:]]*$$/d' modules/manifest.txt | awk '{print $$1}')
SFM_FILES = $(addprefix build/modules/,$(addsuffix .sfmod,$(SFM_NAMES)))
define SFM_RULE
build/modules/$(1).sfmod: modules/$(2) kernel/include/sfmod.h kernel/include/sfmod_pci.h kernel/include/sfmod_device.h kernel/include/sfmod_block.h
	@mkdir -p build/modules
	$$(CC) $$(SFM_FLAGS) -c $$< -o build/modules/$(1).o
	$$(LD) -r -o $$@ build/modules/$(1).o
endef
$(foreach name,$(SFM_NAMES),$(eval $(call SFM_RULE,$(name),$(shell awk '$$1=="$(name)" {print $$2}' modules/manifest.txt))))

build/modules/init.sfpack: $(SFM_FILES) tools/pack-modules.py modules/manifest.txt
	python3 tools/pack-modules.py

.PHONY: modules test-modules test-ramdisk
modules: build/modules/init.sfpack

build/modules/test.fail.sfmod: tests/modules/fail.c kernel/include/sfmod.h kernel/include/sfmod_pci.h kernel/include/sfmod_device.h kernel/include/sfmod_block.h
	@mkdir -p build/modules
	$(CC) $(SFM_FLAGS) -c $< -o build/modules/test.fail.o
	$(LD) -r -o $@ build/modules/test.fail.o

test-modules: modules build/modules/test.fail.sfmod
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror -no-pie -fsanitize=undefined -fno-sanitize-recover=all -Ikernel/include \
	    tests/test_sfmod.c tests/test_registry.c tests/test_pci_client.c tests/test_block.c tests/test_block_client.c kernel/core/block_client.c kernel/core/ipc/endpoint.c kernel/core/module/elf.c \
	    kernel/core/module/loader.c kernel/core/selftest.c kernel/core/pci.c kernel/core/pci_test.c kernel/core/pci_client.c kernel/core/block.c kernel/core/block_io.c -o build/test_sfmod
	build/test_sfmod build/modules/init.sfpack build/modules/test.fail.sfmod $(SFM_FILES)

test-ramdisk:
	@mkdir -p build
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fno-builtin -fsanitize=address,undefined -fno-sanitize-recover=all -Ikernel/include \
	    tests/test_ramdisk.c kernel/src/fs/ramdisk.c kernel/src/drivers/blkdev.c -o build/test_ramdisk
	build/test_ramdisk

.PHONY: test-ata test-fat32 test-shell-cat
test-shell-cat: build/test_shell_cat
	build/test_shell_cat $(SHELL_CAT_ARGS)

build/test_shell_cat: tests/test_shell_cat.c kernel/src/services/shell.c | build
	@mkdir -p build
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -fno-builtin -ffunction-sections -fdata-sections -fsanitize=undefined -fno-sanitize-recover=all -Ikernel/include \
	    tests/test_shell_cat.c -Wl,--gc-sections -o build/test_shell_cat

test-fat32:
	@mkdir -p build
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fno-builtin -fsanitize=address,undefined -fno-sanitize-recover=all -Ikernel/include \
	    tests/test_fat32.c kernel/core/block_client.c kernel/core/ipc/endpoint.c -o build/test_fat32
	build/test_fat32

test-ata:
	@mkdir -p build
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -Ikernel/include \
	    tests/test_ata.c kernel/src/drivers/ata_geometry.c -o build/test_ata
	build/test_ata

$(KERNEL_BUILD)/kernel.elf: $(KERNEL_OBJS) $(KERNEL_SRC)/kernel.ld
	@echo "[LD64] $@"
	@$(LD) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS)

kernel: logo $(KERNEL_BUILD)/kernel.elf

# =========================================
# libc 构建
# =========================================
LIBC_DIR       = libc
LIBC_BUILD     = build/libc

libc:
	@echo "[LIBC] Building libc..."
	@$(MAKE) -C $(LIBC_DIR) CROSS=$(CROSS)

libc-clean:
	@$(MAKE) -C $(LIBC_DIR) clean

# =========================================
# AP Trampoline (raw binary, not linked into kernel)
# =========================================
$(KERNEL_BUILD)/ap_trampoline.bin: $(KERNEL_SRC)/arch/x86_64/ap_trampoline.S
	@mkdir -p $(dir $@)
	@echo "[NASM-TRAMP] $<"
	@$(NASM) -f bin $< -o $@

ap-trampoline: $(KERNEL_BUILD)/ap_trampoline.bin

# =========================================
# EFI Stub 构建 (Linux EFI Stub: kernel.efi = stub + 内嵌 kernel.elf)
#   UEFI 固件直接加载 kernel.efi；stub 解析内嵌 ELF，加载段到 0x100000，
#   完成 GOP/内存映射/ExitBootServices/页表/GDT，跳转 _start64。
# =========================================
EFI_CFLAGS = -std=gnu11 -ffreestanding -fno-stack-protector -fpic \
             -fvisibility=hidden -fno-plt \
             -fshort-wchar -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
             -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
             -I$(EFI_STUB_DIR)

EFI_LDFLAGS = -nostdlib -T $(EFI_STUB_DIR)/linker.ld -static

$(EFI_BUILD):
	@mkdir -p $(EFI_BUILD)

# payload.S 依赖 kernel.elf (内嵌对象)
$(EFI_BUILD)/payload.o: $(EFI_STUB_DIR)/payload.S $(KERNEL_BUILD)/kernel.elf | $(EFI_BUILD)
	@echo "[AS-PAYLOAD] $< (embedding kernel.elf)"
	@$(CC) $(EFI_CFLAGS) -DKERNEL_PAYLOAD_PATH='"$(KERNEL_BUILD)/kernel.elf"' -c $< -o $@

$(EFI_BUILD)/crt0.o: $(EFI_STUB_DIR)/crt0-efi-x86_64.S | $(EFI_BUILD)
	@echo "[AS-EFI] $<"
	@$(CC) -c $< -o $@

$(EFI_BUILD)/efi_main.o: $(EFI_STUB_DIR)/efi_main.c | $(EFI_BUILD)
	@echo "[CC-EFI] $<"
	@$(CC) $(EFI_CFLAGS) -c $< -o $@

$(EFI_BUILD)/stub.elf: $(EFI_BUILD)/crt0.o $(EFI_BUILD)/efi_main.o $(EFI_BUILD)/payload.o $(EFI_STUB_DIR)/linker.ld
	@echo "[LD-EFI] $@"
	@$(LD) $(EFI_LDFLAGS) -o $@ $(EFI_BUILD)/crt0.o $(EFI_BUILD)/efi_main.o $(EFI_BUILD)/payload.o

# ELF → PE32+ (EFI application)
$(KERNEL_EFI): $(EFI_BUILD)/stub.elf $(EFI_STUB_DIR)/gen_reloc.py
	@echo "[OBJCOPY-EFI] $@"
	@$(OBJCOPY) -j .text -j .rodata -j .data -j .bss \
	         --output-target=efi-app-x86_64 \
	         $< $@ 2>/dev/null || \
	  $(OBJCOPY) -j .text -j .rodata -j .data -j .bss \
	         --output-target=pei-x86-64 --subsystem=10 --image-base=0 \
	         $< $@ 2>/dev/null || \
	  { echo 'ERROR: objcopy could not create an EFI PE executable'; exit 1; }
	@echo "[GEN-RELOC] Generating PE .reloc..."
	@python3 $(EFI_STUB_DIR)/gen_reloc.py $< $@
	@echo "[EFI] kernel.efi ready: $@ (`stat -c%s $@ 2>/dev/null || echo ?` bytes)"

uefi-stub: $(KERNEL_EFI)

# =========================================
# UEFI 磁盘镜像 (GPT + ESP, 仅含 BOOTX64.EFI = kernel.efi)
# =========================================
$(IMAGE_DIR):
	@mkdir -p $(IMAGE_DIR)

uefi-image: kernel uefi-stub
	@echo "[UEFI-IMG] Building UEFI disk image..."
	@rm -rf $(IMAGE_DIR)
	@mkdir -p $(EFI_DIR)
	@cp $(KERNEL_EFI) $(EFI_DIR)/BOOTX64.EFI
	@# 创建 64MB 原始镜像 (GPT + ESP 分区)
	@dd if=/dev/zero of=$(IMAGE_FILE) bs=1M count=64 2>/dev/null
	@if command -v sgdisk >/dev/null 2>&1; then \
		echo "[UEFI-IMG] Creating GPT with ESP partition..."; \
		sgdisk -z $(IMAGE_FILE) 2>/dev/null; \
		sgdisk -o $(IMAGE_FILE) 2>/dev/null; \
		sgdisk -n 1::+56M -t 1:EF00 $(IMAGE_FILE) 2>/dev/null; \
		echo "[UEFI-IMG] Extracting ESP partition..."; \
		ESP_OFFSET=$$(sgdisk -i 1 $(IMAGE_FILE) 2>/dev/null | grep "First sector" | awk '{print $$3}'); \
		ESP_SECTORS=$$(sgdisk -i 1 $(IMAGE_FILE) 2>/dev/null | grep "Last sector" | awk '{print $$3}'); \
		if [ -z "$$ESP_OFFSET" ]; then ESP_OFFSET=2048; fi; \
		if [ -z "$$ESP_SECTORS" ]; then ESP_SECTORS=114687; fi; \
		ESP_SIZE=$$((ESP_SECTORS - ESP_OFFSET + 1)); \
		dd if=/dev/zero of=build/esp.img bs=512 count=$$ESP_SIZE 2>/dev/null; \
		echo "[UEFI-IMG] Formatting FAT32..."; \
		mformat -i build/esp.img -v SPIRITFOX :: ; \
		mmd -i build/esp.img ::EFI ::EFI/Boot ; \
		mcopy -i build/esp.img $(EFI_DIR)/BOOTX64.EFI ::EFI/Boot/BOOTX64.EFI ; \
		echo "[UEFI-IMG] Creating startup.nsh for OVMF auto-boot..." ; \
		echo 'FS0:\EFI\Boot\BOOTX64.EFI' > build/startup.nsh ; \
		mcopy -i build/esp.img build/startup.nsh ::startup.nsh ; \
		rm -f build/startup.nsh ; \
		echo "[UEFI-IMG] Writing ESP to disk image at offset $$ESP_OFFSET..." ; \
		dd if=build/esp.img of=$(IMAGE_FILE) bs=512 seek=$$ESP_OFFSET conv=notrunc 2>/dev/null; \
		rm -f build/esp.img; \
	else \
		echo "[UEFI-IMG] sgdisk not found, using raw FAT image..."; \
		mformat -i $(IMAGE_FILE) -v SPIRITFOX :: ; \
		mmd -i $(IMAGE_FILE) ::EFI ::EFI/Boot ; \
		mcopy -i $(IMAGE_FILE) $(EFI_DIR)/BOOTX64.EFI ::EFI/Boot/BOOTX64.EFI ; \
	fi
	@echo "========================================="
	@echo "  UEFI disk image: $(IMAGE_FILE)"
	@echo "  (kernel.efi embedded in BOOTX64.EFI)"
	@echo "========================================="

# =========================================
# QEMU 运行 (UEFI, 默认)
# =========================================
run: run-uefi

run-uefi: uefi-image build/ovmf_vars.fd build/disk.img build/fat32.img
	@if [ ! -f $(OVMF_CODE) ]; then \
		echo "ERROR: OVMF firmware not found at $(OVMF_CODE)"; \
		echo "Install it: sudo apt install ovmf"; \
		exit 1; \
	fi
	qemu-system-x86_64 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=build/ovmf_vars.fd \
	    -drive if=ide,format=raw,file=$(IMAGE_FILE) \
	    -m 2G \
	    -serial stdio \
	    -netdev user,id=net0,hostfwd=tcp::25566-:25565 \
	    -device rtl8139,netdev=net0 \
	    -no-reboot -no-shutdown \
	    -boot c \
	    -device ahci,id=ahci0 \
	    -drive id=fat32disk,file=build/fat32.img,if=none,format=raw \
	    -device ide-hd,drive=fat32disk,bus=ahci0.0 \
	    -drive id=disk0,file=build/disk.img,if=none,format=raw \
	    -device ide-hd,drive=disk0,bus=ahci0.1

# 日志模式: 串口输出到 build/serial.log, 无显示
run-log: uefi-image build/ovmf_vars.fd build/disk.img build/fat32.img
	@if [ ! -f $(OVMF_CODE) ]; then \
		echo "ERROR: OVMF firmware not found at $(OVMF_CODE)"; \
		echo "Install it: sudo apt install ovmf"; \
		exit 1; \
	fi
	qemu-system-x86_64 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=build/ovmf_vars.fd \
	    -drive if=ide,format=raw,file=$(IMAGE_FILE) \
	    -m 2G \
	    -serial file:build/serial.log \
	    -display none \
	    -net none \
	    -no-reboot -no-shutdown \
	    -boot c \
	    -device ahci,id=ahci0 \
	    -drive id=fat32disk,file=build/fat32.img,if=none,format=raw \
	    -device ide-hd,drive=fat32disk,bus=ahci0.0 \
	    -drive id=disk0,file=build/disk.img,if=none,format=raw \
	    -device ide-hd,drive=disk0,bus=ahci0.1

# 调试模式: GDB 远程调试 (端口 1234)
run-debug: uefi-image build/ovmf_vars.fd build/disk.img build/fat32.img
	@if [ ! -f $(OVMF_CODE) ]; then \
		echo "ERROR: OVMF firmware not found at $(OVMF_CODE)"; \
		echo "Install it: sudo apt install ovmf"; \
		exit 1; \
	fi
	qemu-system-x86_64 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=build/ovmf_vars.fd \
	    -drive if=ide,format=raw,file=$(IMAGE_FILE) \
	    -m 2G \
	    -serial stdio \
	    -net none \
	    -no-reboot -no-shutdown \
	    -boot c \
	    -S -gdb tcp::1234 \
	    -device ahci,id=ahci0 \
	    -drive id=fat32disk,file=build/fat32.img,if=none,format=raw \
	    -device ide-hd,drive=fat32disk,bus=ahci0.0 \
	    -drive id=disk0,file=build/disk.img,if=none,format=raw \
	    -device ide-hd,drive=disk0,bus=ahci0.1

build/ovmf_vars.fd:
	@test -f "$(OVMF_VARS)" || { echo 'ERROR: matching OVMF firmware not found; set OVMF_CODE and OVMF_VARS'; exit 1; }
	@mkdir -p build
	@cp "$(OVMF_VARS)" $@

# =========================================
# QEMU disk images (auto-created if missing)
# =========================================
build/disk.img:
	@dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null

build/fat32.img: build/test_hello build/test_musl_hello build/test_musl_dyn build/test_sfmod_file build/sf_compositor build/sf_test_client
	@echo "[FAT32] Creating disk image..."
	@dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null
	@mkfs.fat -F 32 $@
	@mmd -i $@ ::bin 2>/dev/null
	@mmd -i $@ ::pkg 2>/dev/null
	@mmd -i $@ ::lib 2>/dev/null
	@mcopy -i $@ build/test_hello ::bin/test_hello 2>/dev/null; \
		mcopy -i $@ build/test_hello ::test_hello 2>/dev/null
	@mcopy -i $@ build/test_musl_hello ::bin/test_musl_hello 2>/dev/null || true
	@mcopy -i $@ build/test_musl_dyn ::bin/test_musl_dyn 2>/dev/null || true
	@mcopy -i $@ build/test_sfmod_file ::bin/test_sfmod_file
	@mcopy -i $@ build/sf_compositor ::bin/sf_compositor 2>/dev/null || true
	@mcopy -i $@ build/sf_test_client ::bin/sf_test_client 2>/dev/null || true
	@mcopy -i $@ $(SYSROOT)/lib/ld-musl-x86_64.so.1 ::lib/ld-musl-x86_64.so.1 2>/dev/null || true
	@mcopy -i $@ $(SYSROOT)/usr/lib/libc.so ::lib/libc.so 2>/dev/null || true
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
	@echo "[FAT32] Image created (64MB)"

# =========================================
# 测试 ELF 用户程序
# =========================================
TEST_DIR      = tests
TEST_BUILD    = build/tests

build/test_hello: tests/test_hello.c tests/test_crt.S tests/test.ld | build
	@echo "[TEST-ELF] Compiling test_hello..."
	@$(CC) -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
	    -fno-pic -fno-pie -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
	    -mcmodel=small -static -O2 \
	    -c tests/test_hello.c -o build/test_hello.o
	@$(CC) -ffreestanding -nostdlib -fno-stack-protector \
	    -fno-pic -fno-pie -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
	    -mcmodel=small \
	    -c tests/test_crt.S -o build/test_crt.o
	@echo "[TEST-ELF] Linking test_hello..."
	@$(LD) -T tests/test.ld -nostdlib -static -o $@ \
	    build/test_crt.o build/test_hello.o
	@echo "[TEST-ELF] Done: $@"
	@file $@ || true
	@readelf -h $@ 2>/dev/null | head -5 || true

test-elf: build/test_hello
	@echo "[TEST-ELF] Test ELF built: build/test_hello"
	@echo "[TEST-ELF] Run 'make run' and then type 'exec /mnt/bin/test_hello' in the shell"

# =========================================
# musl libc 用户程序
# =========================================
MUSL_CC   = tools/x86_64-spiritfoxos-gcc
SYSROOT   = $(if $(SPIRITFOX_SYSROOT),$(SPIRITFOX_SYSROOT),$(CURDIR)/build/sysroot)

musl-static: build/test_musl_hello
	@echo "[MUSL] Static test binary: build/test_musl_hello"

musl-dynamic: build/test_musl_dyn
	@echo "[MUSL] Dynamic test binary: build/test_musl_dyn"

musl-test: musl-static musl-dynamic
	@echo "[MUSL] All musl test binaries built"
	@file build/test_musl_hello
	@file build/test_musl_dyn

build/test_musl_hello: tests/test_musl_hello.c | build
	@echo "[MUSL-CC] $< (static)"
	@$(MUSL_CC) -static -O2 -o $@ $<

build/test_musl_dyn: tests/test_musl_hello.c | build
	@echo "[MUSL-CC] $< (dynamic)"
	@$(MUSL_CC) -O2 -o $@ $<

build/test_sfmod_file: tests/test_sfmod_file.c | build
	$(MUSL_CC) -static -O2 -Wall -Wextra -Werror -o $@ $<

# 将 musl 测试程序和动态链接器部署到 FAT32 磁盘镜像
musl-deploy: musl-test build/fat32.img
	@echo "[MUSL] Deploying to FAT32 image..."
	@mcopy -i build/fat32.img build/test_musl_hello ::bin/test_musl_hello 2>/dev/null || true
	@mcopy -i build/fat32.img build/test_musl_dyn ::bin/test_musl_dyn 2>/dev/null || true
	@mmd -i build/fat32.img ::lib 2>/dev/null || true
	@mcopy -i build/fat32.img $(SYSROOT)/lib/ld-musl-x86_64.so.1 ::lib/ld-musl-x86_64.so.1 2>/dev/null || true
	@mcopy -i build/fat32.img $(SYSROOT)/usr/lib/libc.so ::lib/libc.so 2>/dev/null || true
	@echo "[MUSL] Deployed. Run 'exec /mnt/bin/test_musl_hello' in shell"

# =========================================
# Wayland 协议与 Compositor
# =========================================
WL_CC       = tools/x86_64-spiritfoxos-gcc
WL_CFLAGS   = -O2 -Wall -Iwayland/compositor -Iwayland/generated \
              -I$(SYSROOT)/usr/include -I$(SYSROOT)/usr/include/wayland
WL_LDFLAGS  = -static -L$(SYSROOT)/usr/lib
WL_SCANNER  ?= $(SYSROOT)/../host-tools/bin/wayland-scanner
WL_PROTOCOL = wayland/protocol

# wayland 协议绑定（已预生成，检查存在性）
wayland-protocol:
	@if [ -f wayland/generated/wayland-client-protocol.h ] && \
	    [ -f wayland/generated/wayland-server-protocol.h ] && \
	    [ -f wayland/generated/xdg-shell-client-protocol.h ] && \
	    [ -f wayland/generated/xdg-shell-server-protocol.h ]; then \
		echo "[WL] Protocol headers already generated"; \
	else \
		echo "[WL] ERROR: Protocol headers not found. Run wayland-scanner first."; \
		exit 1; \
	fi

# libwayland 静态库（预构建，已存在于 sysroot）
wayland-libs:
	@if [ -f $(SYSROOT)/usr/lib/libwayland-server.a ] && \
	    [ -f $(SYSROOT)/usr/lib/libwayland-client.a ]; then \
		echo "[WL] libwayland libraries already in sysroot"; \
	else \
		echo "[WL] ERROR: libwayland libraries not found in sysroot"; \
		exit 1; \
	fi

# Wayland Compositor
wayland-compositor: build/sf_compositor
	@echo "[WL] Compositor: build/sf_compositor"

build/sf_compositor: build/compositor_main.o build/sf_compositor.o build/xdg-shell-protocol.o
	@echo "[WL-LD] $@"
	@$(WL_CC) $(WL_LDFLAGS) -o $@ build/compositor_main.o build/sf_compositor.o build/xdg-shell-protocol.o \
	    -lwayland-server -lffi

build/compositor_main.o: wayland/compositor/main.c | build wayland-protocol wayland-libs
	@echo "[WL-CC] $<"
	@$(WL_CC) $(WL_CFLAGS) -c $< -o $@

build/sf_compositor.o: wayland/compositor/sf_compositor.c | build wayland-protocol wayland-libs
	@echo "[WL-CC] $<"
	@$(WL_CC) $(WL_CFLAGS) -c $< -o $@

# Wayland 测试客户端
wayland-client: build/sf_test_client
	@echo "[WL] Test client: build/sf_test_client"

build/sf_test_client: build/sf_test_client.o build/xdg-shell-protocol.o
	@echo "[WL-LD] $@"
	@$(WL_CC) $(WL_LDFLAGS) -o $@ build/sf_test_client.o build/xdg-shell-protocol.o -lwayland-client -lffi

wayland/generated/xdg-shell-protocol.c: wayland/protocol/xdg-shell.xml
	@mkdir -p wayland/generated
	$(WL_SCANNER) private-code $< $@

build/xdg-shell-protocol.o: wayland/generated/xdg-shell-protocol.c | build wayland-protocol wayland-libs
	$(WL_CC) $(WL_CFLAGS) -c $< -o $@

build/sf_test_client.o: wayland/client/sf_test_client.c | build wayland-protocol wayland-libs
	@echo "[WL-CC] $<"
	@$(WL_CC) $(WL_CFLAGS) -c $< -o $@

# 编译所有 Wayland 组件
wayland-all: wayland-protocol wayland-libs wayland-compositor wayland-client
	@echo "[WL] All Wayland components built"
	@ls -lh build/sf_compositor build/sf_test_client

# 部署 Wayland 到 FAT32 磁盘镜像
wayland-deploy: wayland-all build/fat32.img
	@echo "[WL] Deploying to FAT32 image..."
	@mcopy -i build/fat32.img build/sf_compositor ::bin/sf_compositor 2>/dev/null || true
	@mcopy -i build/fat32.img build/sf_test_client ::bin/sf_test_client 2>/dev/null || true
	@$(WL_CC) $(WL_CFLAGS) -c wayland/client/test_malloc.c -o build/test_malloc.o
	@$(WL_CC) $(WL_LDFLAGS) -o build/test_malloc build/test_malloc.o
	@mcopy -i build/fat32.img build/test_malloc ::bin/test_malloc 2>/dev/null || true
	@echo "[WL] Deployed. Run 'exec /mnt/bin/test_malloc' in shell"

# =========================================
# USB 启动盘制作（实体机用）
# 用法: make usb DEVICE=/dev/sdX
# =========================================
USB_DEVICE ?= /dev/sdX

usb: uefi-image
	@if [ "$(USB_DEVICE)" = "/dev/sdX" ]; then \
		echo "ERROR: 请指定 USB 设备路径，例如: make usb DEVICE=/dev/sdb"; \
		echo "警告: 请确保指定的设备是 USB 驱动器，此操作将擦除其所有数据！"; \
		exit 1; \
	fi
	@if [ ! -b "$(USB_DEVICE)" ]; then \
		echo "ERROR: $(USB_DEVICE) 不是块设备"; \
		exit 1; \
	fi
	@echo "[USB] 将 UEFI 镜像写入 $(USB_DEVICE) ..."
	@echo "[USB] 警告: 这将擦除 $(USB_DEVICE) 上的所有数据！"
	@echo "[USB] 3 秒后开始，按 Ctrl+C 取消..."
	@sleep 3
	@sudo dd if=$(IMAGE_FILE) of=$(USB_DEVICE) bs=4M status=progress
	@sudo sync
	@echo "[USB] 完成！可以从 USB 启动 SpiritFoxOS (UEFI)"

# =========================================
# 清理
# =========================================
clean:
	@echo "[CLEAN] Removing build artifacts..."
	@rm -rf build/
	@rm -f $(LOGO_HEADER)
	@$(MAKE) -C $(LIBC_DIR) clean 2>/dev/null || true
	@echo "Done."
