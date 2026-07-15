# SpiritFoxOS musl-based libc port
CROSS   ?= x86_64-linux-gnu-
CC      = $(CROSS)gcc
AR      = $(CROSS)ar
RANLIB  = $(CROSS)ranlib

# musl source directory (clone or extract here)
MUSL_SRCDIR ?= musl-1.2.5

CFLAGS  = -Os -ffreestanding -nostdlib -fno-builtin \
          -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -D_SPIRITFOXOS_ -D_GNU_SOURCE \
          -I./arch/x86_64 -I./include \
          -I$(MUSL_SRCDIR)/arch/x86_64 \
          -I$(MUSL_SRCDIR)/include \
          -I$(MUSL_SRCDIR)/src/include \
          -I$(MUSL_SRCDIR)/src/internal

OBJS = \
	syscall.o \
	start.o \
	stdio.o \
	stdlib.o \
	string.o \
	unistd.o \
	fcntl.o \
	signal.o \
	dirent.o \
	time.o \
	malloc.o

all: libc.a crt1.o crti.o crtn.o

%.o: %.c
	@echo "[CC-LIBC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	@echo "[AS-LIBC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

libc.a: $(OBJS)
	@echo "[AR] $@"
	@$(AR) rcs $@ $^
	@$(RANLIB) $@

clean:
	@rm -f *.o libc.a

.PHONY: all clean
