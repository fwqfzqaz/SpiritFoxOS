# SpiritFoxOS libc
CROSS   ?= x86_64-linux-gnu-
CC      = $(CROSS)gcc
AR      = $(CROSS)ar
RANLIB  = $(CROSS)ranlib

CFLAGS  = -Os -ffreestanding -nostdlib -fno-builtin \
          -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -D_SPIRITFOXOS_ \
          -I./arch/x86_64 -I./include -I./src

OBJS = \
	src/syscall.o \
	src/start.o \
	src/stdio.o \
	src/stdlib.o \
	src/string.o \
	src/ctype.o \
	src/unistd.o \
	src/dirent.o \
	src/fcntl.o \
	src/signal.o \
	src/time.o \
	src/malloc.o

all: libc.a crt1.o crti.o crtn.o

src/%.o: src/%.c
	@echo "[CC-LIBC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

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
	@rm -f src/*.o *.o libc.a

.PHONY: all clean
