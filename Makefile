# SpiritFoxOS Build System
# Requires: nasm, gcc (x86_64-elf cross-compiler or host gcc with freestanding), ld, grub-mkrescue

# Toolchain - use system gcc with freestanding flags if cross-compiler not available
CC      = gcc
LD      = ld
NASM    = nasm

# Try to detect cross-compiler
CROSS_CC = x86_64-elf-gcc
CROSS_LD = x86_64-elf-ld

ifneq ($(shell which $(CROSS_CC) 2>/dev/null),)
CC  = $(CROSS_CC)
LD  = $(CROSS_LD)
endif

# Directories
SRCDIR  = src
BUILDDIR = build
ISODIR  = iso

# Compiler flags
CFLAGS  = -ffreestanding -fno-pie -fno-stack-protector -fno-pic \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-sse3 \
          -mcmodel=small -m64 \
          -Wall -Wextra -Werror \
          -I$(SRCDIR)/include -I$(SRCDIR)/kernel \
          -nostdlib -nostdinc -fno-builtin -nodefaultlibs

LDFLAGS = -T linker.ld -nostdlib -static

# Source files
C_SRCS   = $(wildcard $(SRCDIR)/kernel/*.c)
ASM_SRCS = $(wildcard $(SRCDIR)/boot/*.asm) $(wildcard $(SRCDIR)/kernel/*.asm)

# Object files
C_OBJS   = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(C_SRCS))
ASM_OBJS = $(patsubst $(SRCDIR)/%.asm,$(BUILDDIR)/%.o,$(ASM_SRCS))

OBJS = $(ASM_OBJS) $(C_OBJS)

# Target
TARGET = $(BUILDDIR)/kernel.elf

.PHONY: all clean iso run debug

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Compile C sources
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Assemble ASM sources
$(BUILDDIR)/%.o: $(SRCDIR)/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -o $@ $<

# Create bootable ISO
iso: $(TARGET)
	@mkdir -p $(ISODIR)/boot/grub
	cp $(TARGET) $(ISODIR)/boot/kernel.elf
	grub-mkrescue -o $(BUILDDIR)/spiritfoxos.iso $(ISODIR) 2>/dev/null || \
		echo "Note: grub-mkrescue may require xorriso and mtools packages"

# Run in QEMU
run: iso
	qemu-system-x86_64 -cdrom $(BUILDDIR)/spiritfoxos.iso -m 128M

# Run in QEMU with serial debug
debug: iso
	qemu-system-x86_64 -cdrom $(BUILDDIR)/spiritfoxos.iso -m 128M -serial stdio -d int,cpu_reset

# Run without ISO (direct kernel boot with GRUB)
qemu-direct: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -m 128M

clean:
	rm -rf $(BUILDDIR) $(ISODIR)/boot/kernel.elf
