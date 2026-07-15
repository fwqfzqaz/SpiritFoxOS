/*
 * SpiritFoxOS Kernel Self-Tests
 * Only compiled when KERNEL_SELFTEST is defined.
 */

#include "vfs.h"
#include "string.h"
#include "registry.h"
#include "sandbox.h"
#include "process.h"
#include "kmalloc.h"
#include "timer.h"

#ifdef KERNEL_SELFTEST

void test_registry(void)
{
    printf("\n--- Registry Self-Test ---\n");

    uint32_t val = 42;
    int ret = registry_write_value("HKEY_SYSTEM/Config", "test_key",
                                   REG_TYPE_UINT32, &val, sizeof(val));
    printf("[R1] write value: %s\n", ret == 0 ? "OK" : "FAILED");

    uint32_t read_val = 0;
    uint32_t read_type = 0;
    uint32_t read_size = sizeof(read_val);
    ret = registry_read_value("HKEY_SYSTEM/Config", "test_key",
                              &read_type, &read_val, &read_size);
    printf("[R2] read value: %s (val=%u)\n",
           (ret == 0 && read_val == 42) ? "OK" : "FAILED", read_val);

    reg_software_record_t sw = {0};
    strcpy(sw.name, "TestApp");
    strcpy(sw.version, "1.0.0");
    strcpy(sw.vendor, "SpiritFoxOS");
    strcpy(sw.install_path, "/opt/sfk/testapp");
    strcpy(sw.install_date, "2026-06-27");
    strcpy(sw.pkg_format, "sfk");
    strcpy(sw.pkg_id, "com.spiritfox.testapp");
    sw.install_size = 1024;
    sw.sfk_perms = SFK_PERM_NETWORK | SFK_PERM_FILE_READ;
    ret = registry_register_software(&sw);
    printf("[R3] register software: %s\n", ret == 0 ? "OK" : "FAILED");

    reg_software_record_t sw2 = {0};
    ret = registry_get_software("com.spiritfox.testapp", &sw2);
    printf("[R4] get software: %s (name=%s, perms=0x%x)\n",
           ret == 0 ? "OK" : "FAILED", sw2.name, sw2.sfk_perms);

    reg_fileassoc_t fa = {0};
    strcpy(fa.extension, ".txt");
    strcpy(fa.mime_type, "text/plain");
    strcpy(fa.app_path, "/usr/bin/cat");
    strcpy(fa.app_name, "Cat");
    strcpy(fa.pkg_id, "com.spiritfox.testapp");
    ret = registry_register_fileassoc(&fa);
    printf("[R5] register fileassoc: %s\n", ret == 0 ? "OK" : "FAILED");

    reg_fileassoc_t fa2 = {0};
    ret = registry_get_fileassoc(".txt", &fa2);
    printf("[R6] get fileassoc: %s (app=%s)\n",
           ret == 0 ? "OK" : "FAILED", fa2.app_path);

    char pkg_ids[16][128];
    int count = registry_list_software(pkg_ids, 16);
    printf("[R7] list software: %d packages\n", count);

    sandbox_policy_t policy;
    sandbox_create_policy("com.spiritfox.testapp",
                          SFK_PERM_NETWORK | SFK_PERM_FILE_READ, &policy);
    printf("[R8] sandbox policy: type=%u, perms=0x%x\n",
           policy.type, policy.granted_perms);

    ret = registry_save("/etc/registry.dat");
    printf("[R9] registry save: %s\n", ret == 0 ? "OK" : "FAILED");

    printf("--- End Registry Self-Test ---\n\n");
}

void test_vfs(void)
{
    printf("\n--- VFS Self-Test ---\n");
    extern int vfs_selftest(void);
    vfs_selftest();
    printf("--- End VFS Self-Test ---\n\n");
}

void test_fat32(void)
{
    int fd;
    char name[64];
    vfs_inode_t stat;

    /* ls /mnt */
    fd = vfs_open("/mnt", VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
    if (fd >= 0) {
        printf("[FAT32 Test] ls /mnt:\n");
        int idx = 0;
        while (vfs_readdir(fd, idx, name, 64, &stat) > 0) {
            printf("  %s (type=%u, size=%llu)\n", name, stat.type, stat.size);
            idx++;
        }
        vfs_close(fd);
    } else {
        printf("[FAT32 Test] Cannot open /mnt (fd=%d)\n", fd);
    }

    /* read /mnt/test.txt */
    fd = vfs_open("/mnt/test.txt", VFS_O_RDONLY, 0);
    if (fd >= 0) {
        char buf[128];
        int n = vfs_read(fd, buf, sizeof(buf) - 1);
        vfs_close(fd);
        if (n > 0) {
            buf[n] = '\0';
            printf("[FAT32 Test] read /mnt/test.txt: \"%s\"\n", buf);
        } else {
            printf("[FAT32 Test] read /mnt/test.txt: empty (n=%d)\n", n);
        }
    } else {
        printf("[FAT32 Test] Cannot open /mnt/test.txt (fd=%d)\n", fd);
    }

    /* ls /mnt/testdir */
    fd = vfs_open("/mnt/testdir", VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
    if (fd >= 0) {
        printf("[FAT32 Test] ls /mnt/testdir:\n");
        int idx = 0;
        while (vfs_readdir(fd, idx, name, 64, &stat) > 0) {
            printf("  %s (type=%u, size=%llu)\n", name, stat.type, stat.size);
            idx++;
        }
        vfs_close(fd);
    } else {
        printf("[FAT32 Test] Cannot open /mnt/testdir (fd=%d)\n", fd);
    }

    /* read /mnt/testdir/nested.txt */
    fd = vfs_open("/mnt/testdir/nested.txt", VFS_O_RDONLY, 0);
    if (fd >= 0) {
        char buf[128];
        int n = vfs_read(fd, buf, sizeof(buf) - 1);
        vfs_close(fd);
        if (n > 0) {
            buf[n] = '\0';
            printf("[FAT32 Test] read /mnt/testdir/nested.txt: \"%s\"\n", buf);
        } else {
            printf("[FAT32 Test] read /mnt/testdir/nested.txt: empty (n=%d)\n", n);
        }
    } else {
        printf("[FAT32 Test] Cannot open /mnt/testdir/nested.txt (fd=%d)\n", fd);
    }

    /* FAT32 Write Tests */
    printf("\n--- FAT32 Write Self-Test ---\n");

    fd = vfs_open("/mnt/writetest.txt", VFS_O_CREAT | VFS_O_WRONLY, VFS_S_IRUSR | VFS_S_IWUSR);
    if (fd >= 0) {
        printf("[W1] create /mnt/writetest.txt: OK (fd=%d)\n", fd);
    } else {
        printf("[W1] create /mnt/writetest.txt: FAILED (fd=%d)\n", fd);
    }

    if (fd >= 0) {
        const char *msg = "Written by SpiritFoxOS!";
        int n = vfs_write(fd, msg, strlen(msg));
        if (n == (int)strlen(msg)) {
            printf("[W2] write /mnt/writetest.txt: OK (%d bytes)\n", n);
        } else {
            printf("[W2] write /mnt/writetest.txt: PARTIAL (%d/%d)\n", n, (int)strlen(msg));
        }
        vfs_close(fd);
    }

    {
        fd = vfs_open("/mnt/writetest.txt", VFS_O_RDONLY, 0);
        if (fd >= 0) {
            char buf[128];
            int n = vfs_read(fd, buf, sizeof(buf) - 1);
            vfs_close(fd);
            if (n > 0) {
                buf[n] = '\0';
                if (strcmp(buf, "Written by SpiritFoxOS!") == 0) {
                    printf("[W3] read back /mnt/writetest.txt: OK (\"%s\")\n", buf);
                } else {
                    printf("[W3] read back /mnt/writetest.txt: MISMATCH (\"%s\")\n", buf);
                }
            } else {
                printf("[W3] read back /mnt/writetest.txt: FAILED (n=%d)\n", n);
            }
        } else {
            printf("[W3] read back /mnt/writetest.txt: FAILED (fd=%d)\n", fd);
        }
    }

    {
        int ret = vfs_mkdir("/mnt/newdir", VFS_S_IRUSR | VFS_S_IWUSR | VFS_S_IXUSR);
        printf("[W4] mkdir /mnt/newdir: %s\n", ret == 0 ? "OK" : "FAILED");
    }

    {
        fd = vfs_open("/mnt/newdir", VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
        printf("[W5] verify /mnt/newdir exists: %s\n", fd >= 0 ? "OK" : "FAILED");
        if (fd >= 0) vfs_close(fd);
    }

    {
        fd = vfs_open("/mnt", VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
        if (fd >= 0) {
            printf("[W6] ls /mnt after writes:\n");
            int idx = 0;
            while (vfs_readdir(fd, idx, name, 64, &stat) > 0) {
                printf("  %s (type=%u, size=%llu)\n", name, stat.type, stat.size);
                idx++;
            }
            vfs_close(fd);
        }
    }

    printf("--- End FAT32 Write Self-Test ---\n\n");
}

#endif /* KERNEL_SELFTEST */
