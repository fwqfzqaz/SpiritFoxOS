#include "shell.h"
#include "string.h"
#include "module.h"
#include "hal.h"
#include "vga.h"
#include "keyboard.h"
#include "memory.h"
#include "pci.h"
#include "timer.h"
#include "apic.h"
#include "blkdev.h"
#include "terminal.h"
#include "vfs.h"
#include "window.h"
#include "process.h"
#include "elf64.h"
#include "autorun.h"

/* ---------- Screen helpers ---------- */
static void shell_clear_screen(void) {
    vga_clear();
}

/* ---------- Stdin/Stdout override for redirection and pipes ---------- */
static int shell_stdin_fd = -1;
static int shell_stdout_fd = -1;

/* Write data to current stdout (redirected fd or terminal) */
static void shell_write(const char *buf, size_t count)
{
    if (shell_stdout_fd >= 0) {
        vfs_write(shell_stdout_fd, buf, count);
    } else {
        for (size_t i = 0; i < count; i++)
            terminal_putchar(buf[i]);
    }
}

/* Write a null-terminated string to current stdout */
static void shell_puts(const char *s)
{
    shell_write(s, strlen(s));
}

/* Write a character to current stdout */
static void shell_putchar(char c)
{
    shell_write(&c, 1);
}

/* ---------- VFS commands ---------- */

static int cmd_ls(int argc, char** argv) {
    const char *path = vfs_get_cwd();
    if (argc >= 2)
        path = argv[1];

    int fd = vfs_open(path, VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
    if (fd < 0) {
        printf("ls: cannot access '%s'\n", path);
        return -1;
    }

    int idx = 0;
    char name[VFS_MAX_NAME];
    vfs_inode_t stat;
    char tmp[128];
    while (1) {
        int ret = vfs_readdir(fd, idx, name, VFS_MAX_NAME, &stat);
        if (ret <= 0) break;
        if (stat.type == VFS_TYPE_DIR) {
            strcpy(tmp, "  ");
            strcat(tmp, name);
            strcat(tmp, "/\n");
            shell_puts(tmp);
        } else if (stat.type == VFS_TYPE_BLKDEV) {
            strcpy(tmp, "  ");
            strcat(tmp, name);
            strcat(tmp, " [blkdev]\n");
            shell_puts(tmp);
        } else if (stat.type == VFS_TYPE_CHARDEV) {
            strcpy(tmp, "  ");
            strcat(tmp, name);
            strcat(tmp, " [chardev]\n");
            shell_puts(tmp);
        } else {
            /* Simple formatting without printf for size */
            strcpy(tmp, "  ");
            strcat(tmp, name);
            strcat(tmp, "  (");
            /* Convert size to string */
            char numbuf[20];
            int pos = 0;
            uint64_t sz = stat.size;
            if (sz == 0) {
                numbuf[pos++] = '0';
            } else {
                char tmp2[20];
                int p2 = 0;
                while (sz > 0) {
                    tmp2[p2++] = '0' + (sz % 10);
                    sz /= 10;
                }
                for (int j = p2 - 1; j >= 0; j--)
                    numbuf[pos++] = tmp2[j];
            }
            numbuf[pos] = '\0';
            strcat(tmp, numbuf);
            strcat(tmp, " bytes)\n");
            shell_puts(tmp);
        }
        idx++;
    }

    vfs_close(fd);
    return 0;
}

static int cmd_cd(int argc, char** argv) {
    if (argc < 2) {
        vfs_chdir("/");
        return 0;
    }
    if (vfs_chdir(argv[1]) != 0) {
        printf("cd: no such directory '%s'\n", argv[1]);
        return -1;
    }
    return 0;
}

static int cmd_pwd(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("%s\n", vfs_get_cwd());
    return 0;
}

static int cmd_cat(int argc, char** argv) {
    char buf[256];
    int n;
    int fd;
    int use_stdin = 0;

    if (argc < 2) {
        /* Read from shell_stdin_fd if available */
        if (shell_stdin_fd >= 0) {
            fd = shell_stdin_fd;
            use_stdin = 1;
        } else {
            printf("cat: missing operand\n");
            return -1;
        }
    } else {
        fd = vfs_open(argv[1], VFS_O_RDONLY, 0);
        if (fd < 0) {
            printf("cat: %s: No such file\n", argv[1]);
            return -1;
        }
    }

    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
        shell_write(buf, n);
    }

    if (!use_stdin)
        vfs_close(fd);
    return 0;
}

static int cmd_mkdir(int argc, char** argv) {
    if (argc < 2) {
        printf("mkdir: missing operand\n");
        return -1;
    }
    if (vfs_mkdir(argv[1], VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR) != 0) {
        printf("mkdir: cannot create '%s'\n", argv[1]);
        return -1;
    }
    return 0;
}

static int cmd_touch(int argc, char** argv) {
    if (argc < 2) {
        printf("touch: missing operand\n");
        return -1;
    }
    int fd = vfs_open(argv[1], VFS_O_CREAT | VFS_O_WRONLY, VFS_S_IRUSR | VFS_S_IWUSR);
    if (fd < 0) {
        printf("touch: cannot create '%s'\n", argv[1]);
        return -1;
    }
    vfs_close(fd);
    return 0;
}

static int cmd_rm(int argc, char** argv) {
    if (argc < 2) {
        printf("rm: missing operand\n");
        return -1;
    }
    if (vfs_unlink(argv[1]) != 0) {
        printf("rm: cannot remove '%s'\n", argv[1]);
        return -1;
    }
    return 0;
}

static int cmd_rmdir(int argc, char** argv) {
    if (argc < 2) {
        printf("rmdir: missing operand\n");
        return -1;
    }
    if (vfs_rmdir(argv[1]) != 0) {
        printf("rmdir: cannot remove '%s'\n", argv[1]);
        return -1;
    }
    return 0;
}

static int cmd_writefile(int argc, char** argv) {
    /* writefile <path> <text> - write text to a file */
    if (argc < 3) {
        printf("writefile: usage: writefile <path> <text>\n");
        return -1;
    }
    int fd = vfs_open(argv[1], VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC,
                      VFS_S_IRUSR | VFS_S_IWUSR);
    if (fd < 0) {
        printf("writefile: cannot open '%s'\n", argv[1]);
        return -1;
    }
    /* Concatenate argv[2..] with spaces */
    for (int i = 2; i < argc; i++) {
        if (i > 2) vfs_write(fd, " ", 1);
        vfs_write(fd, argv[i], strlen(argv[i]));
    }
    vfs_close(fd);
    return 0;
}

static int cmd_mount(int argc, char** argv) {
    if (argc < 4) {
        printf("mount: usage: mount <device> <path> <fstype>\n");
        return -1;
    }
    if (vfs_mount(argv[1], argv[2], argv[3], 0, NULL) != 0) {
        printf("mount: failed\n");
        return -1;
    }
    return 0;
}

static int cmd_vfstest(int argc, char** argv) {
    (void)argc; (void)argv;
    int passed = 0, failed = 0;

    printf("[VFS Test] Starting...\n");

    /* Test 1: ls / */
    printf("[Test 1] ls / ... ");
    {
        int fd = vfs_open("/", VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
        if (fd >= 0) {
            char name[VFS_MAX_NAME];
            vfs_inode_t stat;
            int count = 0;
            int idx = 0;
            while (vfs_readdir(fd, idx, name, VFS_MAX_NAME, &stat) > 0) {
                count++;
                idx++;
            }
            vfs_close(fd);
            printf("OK (%d entries)\n", count);
            passed++;
        } else {
            printf("FAILED (fd=%d)\n", fd);
            failed++;
        }
    }

    /* Test 2: mkdir /tmp */
    printf("[Test 2] mkdir /tmp ... ");
    {
        int ret = vfs_mkdir("/tmp", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
        if (ret == 0) {
            printf("OK\n");
            passed++;
        } else {
            printf("FAILED (ret=%d)\n", ret);
            failed++;
        }
    }

    /* Test 3: touch /hello.txt */
    printf("[Test 3] touch /hello.txt ... ");
    {
        int fd = vfs_open("/hello.txt", VFS_O_CREAT | VFS_O_WRONLY, VFS_S_IRUSR | VFS_S_IWUSR);
        if (fd >= 0) {
            vfs_close(fd);
            printf("OK\n");
            passed++;
        } else {
            printf("FAILED (fd=%d)\n", fd);
            failed++;
        }
    }

    /* Test 4: write to /hello.txt */
    printf("[Test 4] write /hello.txt ... ");
    {
        int fd = vfs_open("/hello.txt", VFS_O_WRONLY, 0);
        if (fd >= 0) {
            const char *msg = "Hello, SpiritFoxOS VFS!";
            int n = vfs_write(fd, msg, strlen(msg));
            vfs_close(fd);
            if (n == (int)strlen(msg)) {
                printf("OK (%d bytes)\n", n);
                passed++;
            } else {
                printf("PARTIAL (wrote %d/%d)\n", n, (int)strlen(msg));
                failed++;
            }
        } else {
            printf("FAILED (fd=%d)\n", fd);
            failed++;
        }
    }

    /* Test 5: read from /hello.txt */
    printf("[Test 5] read /hello.txt ... ");
    {
        int fd = vfs_open("/hello.txt", VFS_O_RDONLY, 0);
        if (fd >= 0) {
            char buf[128];
            int n = vfs_read(fd, buf, sizeof(buf) - 1);
            vfs_close(fd);
            if (n > 0) {
                buf[n] = '\0';
                printf("OK (\"%s\")\n", buf);
                passed++;
            } else {
                printf("FAILED (read %d)\n", n);
                failed++;
            }
        } else {
            printf("FAILED (fd=%d)\n", fd);
            failed++;
        }
    }

    /* Test 6: ls / again - should show tmp, hello.txt, dev */
    printf("[Test 6] ls / (after create) ... ");
    {
        int fd = vfs_open("/", VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
        if (fd >= 0) {
            char name[VFS_MAX_NAME];
            vfs_inode_t stat;
            int count = 0;
            int idx = 0;
            while (vfs_readdir(fd, idx, name, VFS_MAX_NAME, &stat) > 0) {
                count++;
                idx++;
            }
            vfs_close(fd);
            if (count >= 3) {
                printf("OK (%d entries)\n", count);
                passed++;
            } else {
                printf("FAILED (only %d entries, expected >=3)\n", count);
                failed++;
            }
        } else {
            printf("FAILED\n");
            failed++;
        }
    }

    /* Test 7: cd /tmp */
    printf("[Test 7] cd /tmp ... ");
    {
        int ret = vfs_chdir("/tmp");
        if (ret == 0 && strcmp(vfs_get_cwd(), "/tmp") == 0) {
            printf("OK (cwd=%s)\n", vfs_get_cwd());
            passed++;
        } else {
            printf("FAILED (cwd=%s)\n", vfs_get_cwd());
            failed++;
        }
    }

    /* Test 8: ls /dev (devfs) */
    printf("[Test 8] ls /dev (devfs) ... ");
    {
        int fd = vfs_open("/dev", VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
        if (fd >= 0) {
            char name[VFS_MAX_NAME];
            vfs_inode_t stat;
            int count = 0;
            int idx = 0;
            while (vfs_readdir(fd, idx, name, VFS_MAX_NAME, &stat) > 0) {
                count++;
                idx++;
            }
            vfs_close(fd);
            printf("OK (%d device entries)\n", count);
            passed++;
        } else {
            printf("FAILED (fd=%d)\n", fd);
            failed++;
        }
    }

    /* Test 9: rm /hello.txt */
    printf("[Test 9] rm /hello.txt ... ");
    {
        int ret = vfs_unlink("/hello.txt");
        if (ret == 0) {
            printf("OK\n");
            passed++;
        } else {
            printf("FAILED (ret=%d)\n", ret);
            failed++;
        }
    }

    /* Test 10: verify /hello.txt is gone */
    printf("[Test 10] verify /hello.txt gone ... ");
    {
        int fd = vfs_open("/hello.txt", VFS_O_RDONLY, 0);
        if (fd < 0) {
            printf("OK (file removed)\n");
            passed++;
        } else {
            vfs_close(fd);
            printf("FAILED (file still exists)\n");
            failed++;
        }
    }

    /* Test 11: Pipe test */
    printf("[Test 11] pipe ... ");
    {
        int pipefd[2];
        int ret = vfs_pipe(pipefd);
        if (ret == 0) {
            vfs_write(pipefd[1], "hello pipe", 10);
            char buf[32];
            int n = vfs_read(pipefd[0], buf, 10);
            vfs_close(pipefd[0]);
            vfs_close(pipefd[1]);
            if (n == 10 && memcmp(buf, "hello pipe", 10) == 0) {
                printf("OK\n");
                passed++;
            } else {
                printf("FAILED (read %d bytes)\n", n);
                failed++;
            }
        } else {
            printf("FAILED (vfs_pipe returned %d)\n", ret);
            failed++;
        }
    }

    /* Test 12: dup test */
    printf("[Test 12] dup ... ");
    {
        int fd = vfs_open("/dutest.txt", VFS_O_CREAT | VFS_O_WRONLY, VFS_S_IRUSR | VFS_S_IWUSR);
        if (fd >= 0) {
            vfs_write(fd, "dup test", 8);
            int fd2 = vfs_dup(fd);
            vfs_close(fd);
            if (fd2 >= 0) {
                vfs_close(fd2);
                printf("OK\n");
                passed++;
            } else {
                printf("FAILED (vfs_dup returned %d)\n", fd2);
                failed++;
            }
        } else {
            printf("FAILED (open returned %d)\n", fd);
            failed++;
        }
        vfs_unlink("/dutest.txt");
    }

    /* Reset CWD */
    vfs_chdir("/");

    printf("[VFS Test] Done: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? -1 : 0;
}

/* ---------- Extra utility commands ---------- */

static int cmd_cp(int argc, char** argv) {
    if (argc < 3) {
        printf("cp: usage: cp <src> <dst>\n");
        return -1;
    }
    int src_fd = vfs_open(argv[1], VFS_O_RDONLY, 0);
    if (src_fd < 0) {
        printf("cp: cannot open '%s'\n", argv[1]);
        return -1;
    }
    int dst_fd = vfs_open(argv[2], VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, VFS_S_IRUSR | VFS_S_IWUSR);
    if (dst_fd < 0) {
        printf("cp: cannot create '%s'\n", argv[2]);
        vfs_close(src_fd);
        return -1;
    }
    char buf[512];
    int total = 0;
    int n;
    while ((n = vfs_read(src_fd, buf, sizeof(buf))) > 0) {
        vfs_write(dst_fd, buf, n);
        total += n;
    }
    vfs_close(src_fd);
    vfs_close(dst_fd);
    printf("cp: %d bytes copied\n", total);
    return 0;
}

static int cmd_mv(int argc, char** argv) {
    if (argc < 3) {
        printf("mv: usage: mv <src> <dst>\n");
        return -1;
    }
    /* Copy then delete - no rename across filesystems */
    int src_fd = vfs_open(argv[1], VFS_O_RDONLY, 0);
    if (src_fd < 0) {
        printf("mv: cannot open '%s'\n", argv[1]);
        return -1;
    }
    int dst_fd = vfs_open(argv[2], VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, VFS_S_IRUSR | VFS_S_IWUSR);
    if (dst_fd < 0) {
        printf("mv: cannot create '%s'\n", argv[2]);
        vfs_close(src_fd);
        return -1;
    }
    char buf[512];
    int n;
    while ((n = vfs_read(src_fd, buf, sizeof(buf))) > 0) {
        vfs_write(dst_fd, buf, n);
    }
    vfs_close(src_fd);
    vfs_close(dst_fd);
    vfs_unlink(argv[1]);
    return 0;
}

static int cmd_stat(int argc, char** argv) {
    if (argc < 2) {
        printf("stat: usage: stat <path>\n");
        return -1;
    }
    vfs_inode_t *inode = vfs_resolve_path(argv[1]);
    if (!inode) {
        printf("stat: cannot stat '%s'\n", argv[1]);
        return -1;
    }
    const char *type_str = "unknown";
    switch (inode->type) {
    case VFS_TYPE_FILE:    type_str = "file"; break;
    case VFS_TYPE_DIR:     type_str = "directory"; break;
    case VFS_TYPE_BLKDEV:  type_str = "block device"; break;
    case VFS_TYPE_CHARDEV: type_str = "char device"; break;
    case VFS_TYPE_PIPE:    type_str = "pipe"; break;
    case VFS_TYPE_SYMLINK: type_str = "symlink"; break;
    }
    printf("  File: %s\n", argv[1]);
    printf("  Type: %s\n", type_str);
    printf("  Size: %llu bytes\n", inode->size);
    printf("  Inode: %llu\n", inode->ino);
    printf("  Mode: 0x%x\n", inode->mode);
    printf("  Links: %llu\n", inode->nlinks);
    if (inode->type == VFS_TYPE_BLKDEV)
        printf("  Device ID: %u\n", inode->blkdev_id);
    return 0;
}

static int cmd_hexdump(int argc, char** argv) {
    if (argc < 2) {
        printf("hexdump: usage: hexdump <file>\n");
        return -1;
    }
    int fd = vfs_open(argv[1], VFS_O_RDONLY, 0);
    if (fd < 0) {
        printf("hexdump: cannot open '%s'\n", argv[1]);
        return -1;
    }
    char buf[16];
    uint64_t offset = 0;
    int n;
    while ((n = vfs_read(fd, buf, 16)) > 0) {
        printf("%06llx: ", offset);
        for (int i = 0; i < n; i++) {
            printf("%02x ", (unsigned char)buf[i]);
        }
        /* Pad with spaces if less than 16 bytes */
        for (int i = n; i < 16; i++) printf("   ");
        printf(" |");
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c >= 32 && c < 127)
                printf("%c", c);
            else
                printf(".");
        }
        printf("|\n");
        offset += n;
    }
    vfs_close(fd);
    return 0;
}

static int cmd_dmesg(int argc, char** argv) {
    (void)argc; (void)argv;
    /* For now, just print kernel info from serial log */
    printf("Kernel log not available in buffer (use serial output)\n");
    printf("Subsystems initialized: GDT, IDT, Memory, VGA, ACPI, APIC, IOAPIC,\n");
    printf("  Timer (PIT), PCI, Keyboard, Block dev, AHCI, Ramdisk,\n");
    printf("  Terminal, VFS, memfs, devfs, FAT32\n");
    printf("  kmalloc, Process, Scheduler, Syscall, Registry, PkgMgr, Sandbox\n");
    return 0;
}

/* ---------- Registry / Package / Sandbox commands ---------- */

#include "registry.h"
#include "pkgmgr.h"
#include "sandbox.h"
#include "process.h"
#include "syscall.h"

/* Forward declaration */
static int shell_atoi(const char *s);

static int cmd_reg(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: reg <get|set|del|list|dump|save|load> [args...]\n");
        return -1;
    }
    if (strcmp(argv[1], "get") == 0 && argc >= 4) {
        uint32_t type = 0, size = 256;
        char buf[256];
        int ret = registry_read_value(argv[2], argv[3], &type, buf, &size);
        if (ret == 0) {
            if (type == REG_TYPE_STRING) printf("%s = \"%s\"\n", argv[3], buf);
            else if (type == REG_TYPE_UINT32) printf("%s = %u\n", argv[3], *(uint32_t*)buf);
            else if (type == REG_TYPE_UINT64) printf("%s = %llu\n", argv[3], *(uint64_t*)buf);
            else printf("%s = [binary, %u bytes]\n", argv[3], size);
        } else printf("Value not found\n");
    } else if (strcmp(argv[1], "set") == 0 && argc >= 5) {
        uint32_t val = (uint32_t)shell_atoi(argv[4]);
        int ret = registry_write_value(argv[2], argv[3], REG_TYPE_UINT32, &val, sizeof(val));
        printf("Write: %s\n", ret == 0 ? "OK" : "FAILED");
    } else if (strcmp(argv[1], "del") == 0 && argc >= 4) {
        int ret = registry_delete_value(argv[2], argv[3]);
        printf("Delete: %s\n", ret == 0 ? "OK" : "FAILED");
    } else if (strcmp(argv[1], "list") == 0 && argc >= 3) {
        char names[32][128];
        int count = registry_list_keys(argv[2], names, 32);
        printf("Subkeys (%d):\n", count);
        for (int i = 0; i < count; i++) printf("  %s\n", names[i]);
        count = registry_list_values(argv[2], names, 32);
        printf("Values (%d):\n", count);
        for (int i = 0; i < count; i++) printf("  %s\n", names[i]);
    } else if (strcmp(argv[1], "dump") == 0) {
        registry_dump();
    } else if (strcmp(argv[1], "save") == 0 && argc >= 3) {
        int ret = registry_save(argv[2]);
        printf("Save: %s\n", ret == 0 ? "OK" : "FAILED");
    } else if (strcmp(argv[1], "load") == 0 && argc >= 3) {
        int ret = registry_load(argv[2]);
        printf("Load: %s\n", ret == 0 ? "OK" : "FAILED");
    } else {
        printf("Unknown reg subcommand\n");
    }
    return 0;
}

static int cmd_pkg(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: pkg <install|remove|list|info|verify> [args...]\n");
        return -1;
    }
    if (strcmp(argv[1], "install") == 0 && argc >= 3) {
        printf("Installing %s...\n", argv[2]);
        int ret = pkgmgr_install(argv[2], 0);
        printf("Install: %s\n", ret == 0 ? "OK" : "FAILED");
    } else if (strcmp(argv[1], "remove") == 0 && argc >= 3) {
        int ret = pkgmgr_remove(argv[2], 0);
        printf("Remove: %s\n", ret == 0 ? "OK" : "FAILED");
    } else if (strcmp(argv[1], "list") == 0) {
        pkg_info_t packages[32];
        int count = pkgmgr_list(packages, 32);
        printf("Installed packages (%d):\n", count);
        for (int i = 0; i < count; i++) {
            printf("  %s %s [%s] %s\n", packages[i].pkg_id,
                   packages[i].version, packages[i].format,
                   packages[i].pkg_name);
        }
    } else if (strcmp(argv[1], "info") == 0 && argc >= 3) {
        pkg_info_t info;
        if (pkgmgr_query(argv[2], &info) == 0) {
            printf("Package: %s\n", info.pkg_name);
            printf("  ID: %s\n", info.pkg_id);
            printf("  Version: %s\n", info.version);
            printf("  Format: %s\n", info.format);
            printf("  Vendor: %s\n", info.vendor);
            printf("  Path: %s\n", info.install_path);
            printf("  Size: %llu bytes\n", info.install_size);
            if (info.sfk_perms)
                printf("  SFK Perms: 0x%x\n", info.sfk_perms);
        } else {
            printf("Package not found\n");
        }
    } else if (strcmp(argv[1], "verify") == 0 && argc >= 3) {
        int ret = pkgmgr_verify(argv[2]);
        printf("Verify: %s\n", ret == 0 ? "OK" : "FAILED");
    } else {
        printf("Unknown pkg subcommand\n");
    }
    return 0;
}

static int cmd_sandbox(int argc, char** argv) {
    if (argc < 2) {
        sandbox_dump();
        return 0;
    }
    if (strcmp(argv[1], "check") == 0 && argc >= 4) {
        int pid = shell_atoi(argv[2]);
        int action = shell_atoi(argv[3]);
        int ret = sandbox_check(pid, action, NULL);
        printf("Sandbox check pid=%d action=%d: %s\n", pid, action,
               ret == 0 ? "ALLOWED" : "DENIED");
    } else if (strcmp(argv[1], "dump") == 0) {
        sandbox_dump();
    } else {
        printf("Usage: sandbox [check <pid> <action>|dump]\n");
    }
    return 0;
}

/* User-mode launcher thread: loads ELF and jumps to user mode */
static void user_proc_launcher(void *arg)
{
    char *path = (char *)arg;
    process_t *self = process_current();

    printf("[exec] launcher started, PID=%d path='%s'\n", self ? self->pid : -1, path);
    printf("[exec] pre-elf: self=%p pml4=%llx kstack=%p entry=%llx\n",
           (void *)self,
           (unsigned long long)self->pml4,
           self->kernel_stack,
           (unsigned long long)self->entry_point);

    /* Load ELF into this process's address space */
    if (elf_load_from_vfs(self, path) != 0) {
        printf("exec: failed to load ELF '%s'\n", path);
        process_exit(1);
        return;
    }

    printf("[exec] post-elf: self=%p pml4=%llx kstack=%p entry=%llx stack_top=%llx\n",
           (void *)self,
           (unsigned long long)self->pml4,
           self->kernel_stack,
           (unsigned long long)self->entry_point,
           (unsigned long long)self->stack_top);

    /* Set up the user-mode trap frame */
    process_setup_frame(self, self->entry_point, self->stack_top, 0);
    printf("[exec] setup_frame done, trap_frame=%p pml4=%llx\n",
           (void *)self->trap_frame, (unsigned long long)self->pml4);

    /* Clear kernel thread flag – this is now a user process */
    self->flags &= ~PROC_FLAG_KERNEL;

    printf("[exec] about to call process_enter_user, frame=%p\n",
           (void *)self->trap_frame);

    /* Jump to user mode via iretq */
    process_enter_user(self->trap_frame);
}

static int cmd_exec(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: exec <elf-path> [args...]\n");
        return -1;
    }

    const char *path = argv[1];

    /* Check the file exists */
    int fd = vfs_open(path, VFS_O_RDONLY, 0);
    if (fd < 0) {
        printf("exec: cannot open '%s' (err=%d)\n", path, fd);
        return -1;
    }
    vfs_close(fd);

    /* Create a kernel thread that will transition to user mode */
    printf("[exec] creating kthread for '%s'...\n", path);
    process_t *proc = process_create_kthread(user_proc_launcher, (void *)path);
    if (!proc) {
        printf("exec: failed to create process\n");
        return -1;
    }

    printf("exec: launched '%s' as PID %d (state=%d)\n", path, proc->pid, proc->state);

    /* Debug: verify stack contents right after creation */
    {
        volatile uint64_t *vp = (volatile uint64_t *)proc->kernel_rsp;
        printf("[exec] pre-sched: krsp=%lx v[0]=%lx v[6]=%lx v[7]=%lx v[8]=%lx\n",
               (unsigned long)proc->kernel_rsp,
               (unsigned long)vp[0], (unsigned long)vp[6],
               (unsigned long)vp[7], (unsigned long)vp[8]);
    }

    /* Wait for the process to finish - yield CPU to scheduler.
     * We set need_reschedule so the next timer tick will switch
     * to the new process.  Then we hlt to wait. */
    need_reschedule = 1;
    uint64_t wait_start = timer_get_ms();
    while (proc->state != PROC_ZOMBIE && proc->state != PROC_UNUSED) {
        hal_enable_interrupts();
        __asm__ volatile ("hlt");
        if (timer_get_ms() - wait_start > 30000) {
            printf("exec: timeout waiting for PID %d (state=%d)\n", proc->pid, proc->state);
            break;
        }
    }

    if (proc->state == PROC_ZOMBIE) {
        printf("exec: PID %d exited with code %d\n", proc->pid, proc->exit_code);
        proc->state = PROC_UNUSED;
    }

    return 0;
}

static int cmd_ps(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("PID  PPID  STATE    FLAGS   UID  GID  CMD\n");
    for (int i = 0; i < 256; i++) {
        process_t *p = process_get(i);
        if (p && p->state != PROC_UNUSED) {
            const char *state = "?";
            switch (p->state) {
                case PROC_READY:   state = "READY"; break;
                case PROC_RUNNING: state = "RUNNING"; break;
                case PROC_BLOCKED: state = "BLOCKED"; break;
                case PROC_ZOMBIE:  state = "ZOMBIE"; break;
            }
            printf("%-4d %-5d %-8s 0x%-5x %-4d %-4d [pid %d]\n",
                   p->pid, p->ppid, state, p->flags,
                   p->uid, p->gid, p->pid);
        }
    }
    return 0;
}

static int cmd_fileassoc(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: fileassoc <get|set|list> [args...]\n");
        return -1;
    }
    if (strcmp(argv[1], "get") == 0 && argc >= 3) {
        reg_fileassoc_t fa;
        if (registry_get_fileassoc(argv[2], &fa) == 0) {
            printf("Extension: %s\n", fa.extension);
            printf("  MIME: %s\n", fa.mime_type);
            printf("  App: %s (%s)\n", fa.app_name, fa.app_path);
            printf("  Package: %s\n", fa.pkg_id);
        } else {
            printf("No association for %s\n", argv[2]);
        }
    } else if (strcmp(argv[1], "list") == 0) {
        /* List all file associations by scanning HKEY_FILEASSOC */
        char names[32][128];
        int count = registry_list_keys("HKEY_FILEASSOC", names, 32);
        printf("File associations (%d):\n", count);
        for (int i = 0; i < count; i++) {
            reg_fileassoc_t fa;
            if (registry_get_fileassoc(names[i], &fa) == 0) {
                printf("  %s -> %s (%s)\n", fa.extension, fa.app_name, fa.app_path);
            }
        }
    } else if (strcmp(argv[1], "set") == 0 && argc >= 5) {
        reg_fileassoc_t fa = {0};
        strncpy(fa.extension, argv[2], 15);
        strcpy(fa.app_path, argv[3]);
        strcpy(fa.app_name, argv[4]);
        strcpy(fa.pkg_id, "manual");
        int ret = registry_register_fileassoc(&fa);
        printf("Set association: %s\n", ret == 0 ? "OK" : "FAILED");
    } else {
        printf("Unknown fileassoc subcommand\n");
    }
    return 0;
}

/* Simple atoi implementation for shell commands */
static int shell_atoi(const char *s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

/* ---------- Original commands ---------- */

static int cmd_help(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("Available commands:\n");
    printf("  help                - Show this help message\n");
    printf("  clear               - Clear the screen\n");
    printf("  echo [args...]      - Print arguments\n");
    printf("  version             - Show OS version\n");
    printf("  reboot              - Reboot the system\n");
    printf("  meminfo             - Show memory information\n");
    printf("  pcilist             - List PCI devices\n");
    printf("  blklist             - List block devices\n");
    printf("  uptime              - Show system uptime\n");
    printf("  lsmod               - List loaded modules\n");
    printf("  loadmod <name>      - Load a module\n");
    printf("  unloadmod <name>    - Unload a module\n");
    printf("  about               - About SpiritFoxOS\n");
    printf("  autorun [cmd]       - Execute autorun config\n");
    printf("  --- Filesystem ---\n");
    printf("  ls [path]           - List directory contents\n");
    printf("  cd [path]           - Change directory\n");
    printf("  pwd                 - Print working directory\n");
    printf("  cat <file>          - Display file contents\n");
    printf("  mkdir <dir>         - Create directory\n");
    printf("  touch <file>        - Create empty file\n");
    printf("  rm <file>           - Remove file\n");
    printf("  rmdir <dir>         - Remove empty directory\n");
    printf("  writefile <f> <txt> - Write text to file\n");
    printf("  mount <dev> <path> <type> - Mount filesystem\n");
    printf("  --- Utilities ---\n");
    printf("  cp <src> <dst>      - Copy file\n");
    printf("  mv <src> <dst>      - Move/rename file\n");
    printf("  stat <path>         - Show file info\n");
    printf("  hexdump <file>      - Hex dump file\n");
    printf("  dmesg               - Show kernel log\n");
    printf("  --- Registry & Packages ---\n");
    printf("  reg <cmd> [args]    - Registry operations\n");
    printf("  pkg <cmd> [args]    - Package manager\n");
    printf("  sandbox [cmd]       - Sandbox status\n");
    printf("  ps                  - List processes\n");
    printf("  fileassoc <cmd>     - File associations\n");
    printf("  --- Redirection & Pipes ---\n");
    printf("  cmd > file         - Redirect stdout to file\n");
    printf("  cmd >> file        - Append stdout to file\n");
    printf("  cmd < file         - Redirect stdin from file\n");
    printf("  cmd1 | cmd2        - Pipe cmd1 stdout to cmd2 stdin\n");
    return 0;
}

static int cmd_clear(int argc, char** argv) {
    (void)argc; (void)argv;
    shell_clear_screen();
    return 0;
}

static int cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) shell_putchar(' ');
        shell_puts(argv[i]);
    }
    shell_putchar('\n');
    return 0;
}

static int cmd_version(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("SpiritFoxOS v0.4.0\n");
    return 0;
}

static int cmd_reboot(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("Rebooting...\n");
    uint8_t val = hal_inb(0x61);
    hal_outb(0x61, (uint8_t)(val | 0x80));
    hal_io_wait();
    hal_outb(0x61, val);
    hal_outb(0x64, 0xFE);
    for (;;) __asm__ volatile ("hlt");
    return 0;
}

static int cmd_meminfo(int argc, char** argv) {
    (void)argc; (void)argv;
    uint64_t total = pmm_total_pages();
    uint64_t used  = pmm_used_pages();
    printf("Memory Information:\n");
    printf("  Total usable pages : %u\n", (unsigned int)total);
    printf("  Used pages         : %u\n", (unsigned int)used);
    printf("  Free pages         : %u\n", (unsigned int)(total - used));
    printf("  Total memory       : %u KB\n", (unsigned int)(total * 4));
    printf("  Used memory        : %u KB\n", (unsigned int)(used * 4));
    return 0;
}

static int cmd_pcilist(int argc, char** argv) {
    (void)argc; (void)argv;
    pci_list_devices();
    return 0;
}

static int cmd_blklist(int argc, char** argv) {
    (void)argc; (void)argv;
    blkdev_list();
    return 0;
}

static int cmd_uptime(int argc, char** argv) {
    (void)argc; (void)argv;
    uint64_t ms = timer_get_ms();
    uint64_t sec = ms / 1000;
    uint64_t rem = ms % 1000;
    printf("Uptime: %llu.%03u seconds\n", sec, (unsigned int)rem);
    printf("LAPIC ID: %u\n", apic_get_lapic_id());
    return 0;
}

static int cmd_lsmod(int argc, char** argv) {
    (void)argc; (void)argv;
    module_list();
    return 0;
}

static int cmd_loadmod(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: loadmod <name>\n");
        return -1;
    }
    return module_load(argv[1]);
}

static int cmd_unloadmod(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: unloadmod <name>\n");
        return -1;
    }
    return module_unload(argv[1]);
}

static int cmd_about(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("SpiritFoxOS - A modular operating system\n");
    printf("HAL: x86_64 | APIC: IOAPIC | Timer: 8254 PIT\n");
    printf("VFS: memfs + devfs | Block: AHCI + Ramdisk\n");
    return 0;
}

static int cmd_autorun(int argc, char** argv) {
    const char *path = NULL;
    if (argc >= 2) {
        if (strcmp(argv[1], "run") == 0) {
            path = (argc >= 3) ? argv[2] : NULL;
            int count = autorun_execute(path);
            if (count < 0)
                printf("autorun: file not found\n");
            return 0;
        } else if (strcmp(argv[1], "create") == 0) {
            path = (argc >= 3) ? argv[2] : NULL;
            int ret = autorun_create_default(path);
            printf("autorun: %s\n", ret == 0 ? "OK" : "FAILED");
            return ret;
        } else if (strcmp(argv[1], "edit") == 0) {
            /* Show current autorun.cfg content */
            path = (argc >= 3) ? argv[2] : AUTORUN_DEFAULT_PATH;
            int fd = vfs_open(path, VFS_O_RDONLY, 0);
            if (fd < 0) {
                printf("autorun: %s not found\n", path);
                return -1;
            }
            char buf[256];
            int n;
            while ((n = vfs_read(fd, buf, sizeof(buf))) > 0)
                shell_write(buf, n);
            vfs_close(fd);
            return 0;
        } else {
            path = argv[1];
            int count = autorun_execute(path);
            if (count < 0)
                printf("autorun: file not found\n");
            return 0;
        }
    }
    printf("Usage: autorun [run|create|edit] [path]\n");
    printf("  autorun          - Execute /etc/autorun.cfg\n");
    printf("  autorun run [path]  - Execute autorun file\n");
    printf("  autorun create [path] - Create default autorun.cfg\n");
    printf("  autorun edit [path]  - View autorun.cfg content\n");
    return 0;
}

static int cmd_window(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("Entering graphical mode... (ESC to exit, TAB to switch apps)\n");
    window_enter();
    printf("Returned to command line.\n");
    return 0;
}

/* ---------- Command table ---------- */

typedef struct {
    const char* name;
    int (*handler)(int argc, char** argv);
    const char* help;
} ShellCommand;

static const ShellCommand builtin_commands[] = {
    { "help",      cmd_help,      "Show this help message" },
    { "clear",     cmd_clear,     "Clear the screen" },
    { "echo",      cmd_echo,      "Print arguments" },
    { "version",   cmd_version,   "Show OS version" },
    { "reboot",    cmd_reboot,    "Reboot the system" },
    { "meminfo",   cmd_meminfo,   "Show memory information" },
    { "pcilist",   cmd_pcilist,   "List PCI devices" },
    { "blklist",   cmd_blklist,   "List block devices" },
    { "uptime",    cmd_uptime,    "Show system uptime" },
    { "lsmod",     cmd_lsmod,     "List loaded modules" },
    { "loadmod",   cmd_loadmod,   "Load a module" },
    { "unloadmod", cmd_unloadmod, "Unload a module" },
    { "about",     cmd_about,     "About SpiritFoxOS" },
    { "autorun",   cmd_autorun,   "Execute autorun config" },
    { "window",    cmd_window,    "Enter graphical window manager" },
    /* VFS commands */
    { "ls",        cmd_ls,        "List directory" },
    { "cd",        cmd_cd,        "Change directory" },
    { "pwd",       cmd_pwd,       "Print working directory" },
    { "cat",       cmd_cat,       "Display file" },
    { "mkdir",     cmd_mkdir,     "Create directory" },
    { "touch",     cmd_touch,     "Create empty file" },
    { "rm",        cmd_rm,        "Remove file" },
    { "rmdir",     cmd_rmdir,     "Remove directory" },
    { "writefile", cmd_writefile, "Write text to file" },
    { "mount",     cmd_mount,     "Mount filesystem" },
    { "vfstest",   cmd_vfstest,   "Run VFS tests" },
    /* Extra utilities */
    { "cp",        cmd_cp,        "Copy file" },
    { "mv",        cmd_mv,        "Move/rename file" },
    { "stat",      cmd_stat,      "Show file info" },
    { "hexdump",   cmd_hexdump,   "Hex dump file" },
    { "dmesg",     cmd_dmesg,     "Show kernel log" },
    /* Registry / Package / Sandbox */
    { "reg",       cmd_reg,       "Registry operations" },
    { "pkg",       cmd_pkg,       "Package manager" },
    { "sandbox",   cmd_sandbox,   "Sandbox status" },
    { "ps",        cmd_ps,        "List processes" },
    { "exec",      cmd_exec,      "Execute ELF binary" },
    { "fileassoc", cmd_fileassoc, "File associations" },
};

#define NUM_BUILTIN (sizeof(builtin_commands) / sizeof(builtin_commands[0]))

/* ---------- Command history ---------- */

#define SHELL_HISTORY_SIZE 32

static char shell_history[SHELL_HISTORY_SIZE][SHELL_MAX_LINE];
static int  shell_history_count = 0;
static int  shell_history_idx = 0;    /* Next write position (ring buffer) */
static int  shell_history_scroll = 0; /* Offset from most recent when browsing */

/* Saved input before history browsing started */
static char shell_saved_input[SHELL_MAX_LINE];
static int  shell_has_saved_input = 0;

static void shell_history_add(const char *line)
{
    int len = strlen(line);
    if (len == 0) return;

    /* Don't add duplicate of most recent entry */
    if (shell_history_count > 0) {
        int last = (shell_history_idx - 1 + SHELL_HISTORY_SIZE) % SHELL_HISTORY_SIZE;
        if (strcmp(shell_history[last], line) == 0)
            return;
    }

    strcpy(shell_history[shell_history_idx], line);
    shell_history_idx = (shell_history_idx + 1) % SHELL_HISTORY_SIZE;
    if (shell_history_count < SHELL_HISTORY_SIZE)
        shell_history_count++;
    shell_history_scroll = 0;
}

static const char* shell_history_get(int scroll)
{
    if (scroll <= 0 || scroll > shell_history_count)
        return NULL;
    int idx = (shell_history_idx - scroll + SHELL_HISTORY_SIZE) % SHELL_HISTORY_SIZE;
    return shell_history[idx];
}

/* ---------- Tab completion ---------- */

static void shell_tab_complete(void)
{
    const char *input = terminal_get_input();
    int input_len = terminal_get_input_len();

    if (input_len == 0)
        return;

    /* Find the start of the current word */
    int word_start = input_len;
    while (word_start > 0 && input[word_start - 1] != ' ')
        word_start--;

    int word_len = input_len - word_start;
    int is_command = (word_start == 0);

    /* Collect matches */
    #define MAX_MATCHES 64
    const char *matches[MAX_MATCHES];
    int match_count = 0;
    static char file_match_buf[MAX_MATCHES][VFS_MAX_NAME];

    if (is_command) {
        /* Complete command name from builtin_commands */
        for (size_t i = 0; i < NUM_BUILTIN && match_count < MAX_MATCHES; i++) {
            if (strncmp(builtin_commands[i].name, input, word_len) == 0) {
                matches[match_count++] = builtin_commands[i].name;
            }
        }
    } else {
        /* Complete filename from current directory */
        const char *cwd = vfs_get_cwd();
        int fd = vfs_open(cwd, VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
        if (fd >= 0) {
            char name[VFS_MAX_NAME];
            vfs_inode_t stat;
            int idx = 0;
            while (vfs_readdir(fd, idx, name, VFS_MAX_NAME, &stat) > 0 && match_count < MAX_MATCHES) {
                if (strncmp(name, input + word_start, word_len) == 0) {
                    strcpy(file_match_buf[match_count], name);
                    matches[match_count] = file_match_buf[match_count];
                    match_count++;
                }
                idx++;
            }
            vfs_close(fd);
        }
    }

    if (match_count == 0) {
        /* No match - do nothing */
        return;
    }

    if (match_count == 1) {
        /* Single match - complete it */
        const char *match = matches[0];
        int match_len = strlen(match);
        /* Append the unmatched portion */
        int append_start = word_len;
        int append_len = match_len - append_start;

        if (is_command && append_len > 0) {
            /* Append remaining chars plus a space for commands */
            char buf[SHELL_MAX_LINE];
            memcpy(buf, input, input_len);
            memcpy(buf + input_len, match + append_start, append_len);
            buf[input_len + append_len] = ' ';
            int new_len = input_len + append_len + 1;
            terminal_set_input(buf, new_len);
        } else if (!is_command && append_len > 0) {
            /* For file completion, append remaining chars */
            char buf[SHELL_MAX_LINE];
            memcpy(buf, input, input_len);
            memcpy(buf + input_len, match + append_start, append_len);

            /* Check if directory - append '/' */
            const char *cwd = vfs_get_cwd();
            int check_fd = vfs_open(cwd, VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
            int is_dir = 0;
            if (check_fd >= 0) {
                char dname[VFS_MAX_NAME];
                vfs_inode_t dstat;
                int di = 0;
                while (vfs_readdir(check_fd, di, dname, VFS_MAX_NAME, &dstat) > 0) {
                    if (strcmp(dname, match) == 0) {
                        is_dir = (dstat.type == VFS_TYPE_DIR);
                        break;
                    }
                    di++;
                }
                vfs_close(check_fd);
            }

            int new_len = input_len + append_len;
            if (is_dir) {
                buf[new_len] = '/';
                new_len++;
            }
            terminal_set_input(buf, new_len);
        }
    } else {
        /* Multiple matches - print them */
        printf("\n");
        for (int i = 0; i < match_count; i++) {
            printf("  %s", matches[i]);
            if (!is_command) {
                /* Check if directory for trailing / - skip for brevity in multi-match */
            }
            printf("\n");
        }
        /* Re-print prompt and current input */
        const char *cwd = vfs_get_cwd();
        printf("%s> %s", cwd, input);
    }
}

/* ---------- Shell key handler ---------- */

static void shell_key_handler(char key)
{
    unsigned char uk = (unsigned char)key;

    if (uk == TERM_CHAR_UP) {
        if (shell_history_scroll < shell_history_count) {
            /* Save current input before first scroll */
            if (shell_history_scroll == 0) {
                const char *cur = terminal_get_input();
                int cur_len = terminal_get_input_len();
                memcpy(shell_saved_input, cur, cur_len);
                shell_saved_input[cur_len] = '\0';
                shell_has_saved_input = 1;
            }
            shell_history_scroll++;
            const char *entry = shell_history_get(shell_history_scroll);
            if (entry) {
                terminal_set_input(entry, strlen(entry));
            }
        }
    } else if (uk == TERM_CHAR_DOWN) {
        if (shell_history_scroll > 0) {
            shell_history_scroll--;
            if (shell_history_scroll == 0) {
                /* Restore saved input */
                if (shell_has_saved_input) {
                    terminal_set_input(shell_saved_input, strlen(shell_saved_input));
                } else {
                    terminal_set_input("", 0);
                }
            } else {
                const char *entry = shell_history_get(shell_history_scroll);
                if (entry) {
                    terminal_set_input(entry, strlen(entry));
                }
            }
        }
    } else if (uk == TERM_CHAR_TAB) {
        shell_tab_complete();
    }
}

/* ---------- Line editing ---------- */

static void shell_read_line(char* buf, int maxlen) {
    terminal_readline(buf, maxlen);
}

/* ---------- Command parsing ---------- */

static int shell_parse_line(char* line, char** argv, int max_args) {
    int argc = 0;
    char* token = strtok(line, " \t");
    while (token != (void*)0 && argc < max_args) {
        argv[argc++] = token;
        token = strtok((void*)0, " \t");
    }
    return argc;
}

/* ---------- Public API ---------- */

static const char *logo_ascii[] = {
    "                               *****                      ******",
    "                              *+++++++++++          ++++++++++++*",
    "                              +++++===========++=========++++++++",
    "                             +++==============++++===========+++++",
    "                             +=======::=================:=======++",
    "                             ====::::======================:=====+",
    "                              ==:::===::============::=======::===",
    "                              =====:::::::::::::::::::::::=======",
    "                              ==::::::::::::::::::::::::::::::::=",
    "                                        .:::::::::::::.",
    "                                           .:::::::.          ..",
    "                              ++++++++++++   .:+:.     =:      ...",
    "                             ++++++++++++===        :::::::=   ....",
    "                             +****+++++++=====     :::::==      .",
    "                             ******++++++=======    ==",
    "                              ****++++++======::==",
    "                                *+++++++======:::::::..      ...",
    "                                  ++++++=======::::::::::::==",
    "                                        +++=============",
    NULL
};

void shell_init(void) {
    terminal_set_key_callback(shell_key_handler);

    /* Show splash logo for 2 seconds */
    vga_clear();
    vga_set_color(0x0B, 0x00); /* Cyan on black */
    for (int i = 0; logo_ascii[i] != NULL; i++) {
        printf("%s\n", logo_ascii[i]);
    }
    vga_set_color(0x0F, 0x00); /* White on black */
    timer_sleep_ms(2000);

    vga_clear();
    printf("========================================\n");
    printf("  SpiritFoxOS v0.4.0\n");
    printf("  Modular Command Line Interface\n");
    printf("  VFS | memfs | devfs | AHCI | Ramdisk\n");
    printf("  Pipes | Redirection | History | Tab\n");
    printf("  Type 'help' for available commands.\n");
    printf("========================================\n");
}

/* ---------- Pipeline and redirection execution ---------- */

#define MAX_PIPE_STAGES 8

/* Parse a single command stage for redirections.
 * Returns the argc for the command (excluding redirection tokens).
 * Sets stdin_file, stdout_file, append_mode. */
static int parse_stage(char **argv, int raw_argc,
                       char **stdin_file, char **stdout_file, int *append_mode)
{
    *stdin_file = NULL;
    *stdout_file = NULL;
    *append_mode = 0;

    int argc = 0;
    int i = 0;
    while (i < raw_argc) {
        if (strcmp(argv[i], ">") == 0) {
            i++;
            if (i < raw_argc) {
                *stdout_file = argv[i];
                *append_mode = 0;
                i++;
            }
        } else if (strcmp(argv[i], ">>") == 0) {
            i++;
            if (i < raw_argc) {
                *stdout_file = argv[i];
                *append_mode = 1;
                i++;
            }
        } else if (strcmp(argv[i], "<") == 0) {
            i++;
            if (i < raw_argc) {
                *stdin_file = argv[i];
                i++;
            }
        } else {
            argv[argc++] = argv[i];
            i++;
        }
    }
    return argc;
}

/* Execute a single command with optional stdin/stdout override */
static int execute_with_fds(const char *cmd, int argc, char **argv)
{
    /* Search built-in commands */
    for (size_t i = 0; i < NUM_BUILTIN; i++) {
        if (strcmp(cmd, builtin_commands[i].name) == 0) {
            return builtin_commands[i].handler(argc, argv);
        }
    }

    /* Try module command handler */
    int ret = module_handle_command(cmd, argc, argv);
    if (ret != -1)
        return ret;

    printf("shell: unknown command '%s'\n", cmd);
    return -1;
}

void shell_run(void) {
    char line[SHELL_MAX_LINE];
    char* argv[SHELL_MAX_ARGS];

    while (1) {
        /* Show CWD in prompt */
        const char *cwd_path = vfs_get_cwd();
        printf("%s> ", cwd_path);

        shell_read_line(line, SHELL_MAX_LINE);

        /* Add to history and reset scroll */
        shell_history_add(line);
        shell_history_scroll = 0;
        shell_has_saved_input = 0;

        /* Skip empty lines */
        if (line[0] == '\0')
            continue;

        /* First, split by '|' to find pipeline stages.
         * We need to do this before tokenizing by spaces. */
        char *stages[MAX_PIPE_STAGES];
        int stage_count = 0;

        /* Make a copy since we'll modify it */
        char line_copy[SHELL_MAX_LINE];
        strcpy(line_copy, line);

        char *stage = line_copy;
        stages[stage_count++] = stage;
        for (char *p = line_copy; *p; p++) {
            if (*p == '|') {
                *p = '\0';
                if (stage_count < MAX_PIPE_STAGES) {
                    stages[stage_count++] = p + 1;
                }
            }
        }

        if (stage_count == 1) {
            /* No pipe - just parse and execute with possible redirections */
            int raw_argc = shell_parse_line(line, argv, SHELL_MAX_ARGS);
            if (raw_argc == 0) continue;

            char *stdin_file = NULL, *stdout_file = NULL;
            int append_mode = 0;
            int argc = parse_stage(argv, raw_argc, &stdin_file, &stdout_file, &append_mode);
            if (argc == 0) continue;

            int saved_stdin = -1, saved_stdout = -1;
            int redir_in_fd = -1, redir_out_fd = -1;

            /* Setup input redirection */
            if (stdin_file) {
                redir_in_fd = vfs_open(stdin_file, VFS_O_RDONLY, 0);
                if (redir_in_fd < 0) {
                    printf("shell: %s: No such file or directory\n", stdin_file);
                    continue;
                }
                saved_stdin = shell_stdin_fd;
                shell_stdin_fd = redir_in_fd;
            }

            /* Setup output redirection */
            if (stdout_file) {
                uint32_t flags = VFS_O_WRONLY | VFS_O_CREAT;
                if (append_mode)
                    flags |= VFS_O_APPEND;
                else
                    flags |= VFS_O_TRUNC;
                redir_out_fd = vfs_open(stdout_file, flags, VFS_S_IRUSR | VFS_S_IWUSR);
                if (redir_out_fd < 0) {
                    printf("shell: %s: Cannot open for writing\n", stdout_file);
                    if (redir_in_fd >= 0) {
                        shell_stdin_fd = saved_stdin;
                        vfs_close(redir_in_fd);
                    }
                    continue;
                }
                saved_stdout = shell_stdout_fd;
                shell_stdout_fd = redir_out_fd;
            }

            execute_with_fds(argv[0], argc, argv);

            /* Restore */
            if (stdin_file) {
                shell_stdin_fd = saved_stdin;
                vfs_close(redir_in_fd);
            }
            if (stdout_file) {
                shell_stdout_fd = saved_stdout;
                vfs_close(redir_out_fd);
            }
        } else {
            /* Pipeline execution - sequential */
            int prev_pipe_read_fd = -1;

            for (int s = 0; s < stage_count; s++) {
                /* Tokenize this stage */
                char *stage_argv[SHELL_MAX_ARGS];
                int stage_argc = shell_parse_line(stages[s], stage_argv, SHELL_MAX_ARGS);
                if (stage_argc == 0) continue;

                /* Parse redirections within this stage */
                char *stdin_file = NULL, *stdout_file = NULL;
                int append_mode = 0;
                int argc = parse_stage(stage_argv, stage_argc, &stdin_file, &stdout_file, &append_mode);
                if (argc == 0) continue;

                int saved_stdin = -1, saved_stdout = -1;
                int redir_in_fd = -1, redir_out_fd = -1;
                int pipe_fd[2] = {-1, -1};
                int is_last = (s == stage_count - 1);

                /* Setup stdin for this stage */
                if (s > 0 && prev_pipe_read_fd >= 0) {
                    /* Read from previous pipe */
                    saved_stdin = shell_stdin_fd;
                    shell_stdin_fd = prev_pipe_read_fd;
                } else if (stdin_file) {
                    redir_in_fd = vfs_open(stdin_file, VFS_O_RDONLY, 0);
                    if (redir_in_fd >= 0) {
                        saved_stdin = shell_stdin_fd;
                        shell_stdin_fd = redir_in_fd;
                    }
                }

                /* Setup stdout for this stage */
                if (!is_last) {
                    /* Create pipe to next stage */
                    if (vfs_pipe(pipe_fd) != 0) {
                        printf("shell: pipe failed\n");
                        break;
                    }
                    saved_stdout = shell_stdout_fd;
                    shell_stdout_fd = pipe_fd[1]; /* write end */
                } else if (stdout_file) {
                    uint32_t flags = VFS_O_WRONLY | VFS_O_CREAT;
                    if (append_mode)
                        flags |= VFS_O_APPEND;
                    else
                        flags |= VFS_O_TRUNC;
                    redir_out_fd = vfs_open(stdout_file, flags, VFS_S_IRUSR | VFS_S_IWUSR);
                    if (redir_out_fd >= 0) {
                        saved_stdout = shell_stdout_fd;
                        shell_stdout_fd = redir_out_fd;
                    }
                }

                /* Execute this stage */
                execute_with_fds(stage_argv[0], argc, stage_argv);

                /* Restore stdout and close pipe write end */
                if (!is_last) {
                    shell_stdout_fd = saved_stdout;
                    vfs_close(pipe_fd[1]); /* Close write end */

                    /* Close previous pipe read end (if any) */
                    if (prev_pipe_read_fd >= 0 && s > 0) {
                        shell_stdin_fd = saved_stdin;
                        vfs_close(prev_pipe_read_fd);
                    }

                    prev_pipe_read_fd = pipe_fd[0]; /* Read end for next stage */
                } else {
                    /* Last stage - restore everything */
                    if (s > 0 && prev_pipe_read_fd >= 0) {
                        shell_stdin_fd = saved_stdin;
                        vfs_close(prev_pipe_read_fd);
                        prev_pipe_read_fd = -1;
                    }
                    if (redir_in_fd >= 0) {
                        shell_stdin_fd = saved_stdin;
                        vfs_close(redir_in_fd);
                    }
                    if (redir_out_fd >= 0) {
                        shell_stdout_fd = saved_stdout;
                        vfs_close(redir_out_fd);
                    }
                }
            }

            /* Cleanup any remaining pipe fds */
            if (prev_pipe_read_fd >= 0) {
                vfs_close(prev_pipe_read_fd);
            }
        }
    }
}

int shell_execute(const char* cmd, int argc, char** argv) {
    return execute_with_fds(cmd, argc, argv);
}
