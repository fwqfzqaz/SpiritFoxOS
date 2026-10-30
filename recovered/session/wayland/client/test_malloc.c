/* Simple malloc test for SpiritFoxOS - verify musl malloc works */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

int main(void)
{
    write(2, "[test_malloc] starting\n", 23);

    /* Test 1: basic malloc/free */
    write(2, "[test_malloc] test1: basic malloc\n", 34);
    void *p1 = malloc(100);
    if (!p1) { write(2, "[test_malloc] malloc(100) FAILED\n", 33); return 1; }
    memset(p1, 0xAA, 100);
    free(p1);
    write(2, "[test_malloc] test1: OK\n", 24);

    /* Test 2: multiple allocations */
    write(2, "[test_malloc] test2: multiple allocs\n", 37);
    void *ptrs[10];
    for (int i = 0; i < 10; i++) {
        ptrs[i] = malloc(64 + i * 32);
        if (!ptrs[i]) { write(2, "[test_malloc] alloc failed\n", 27); return 1; }
        memset(ptrs[i], i, 64 + i * 32);
    }
    for (int i = 0; i < 10; i++)
        free(ptrs[i]);
    write(2, "[test_malloc] test2: OK\n", 24);

    /* Test 3: large allocation */
    write(2, "[test_malloc] test3: large alloc\n", 33);
    void *p3 = malloc(65536);
    if (!p3) { write(2, "[test_malloc] malloc(65536) FAILED\n", 35); return 1; }
    memset(p3, 0xBB, 65536);
    free(p3);
    write(2, "[test_malloc] test3: OK\n", 24);

    /* Test 4: calloc */
    write(2, "[test_malloc] test4: calloc\n", 28);
    void *p4 = calloc(100, 4);
    if (!p4) { write(2, "[test_malloc] calloc FAILED\n", 28); return 1; }
    for (int i = 0; i < 400; i++) {
        if (((char*)p4)[i] != 0) { write(2, "[test_malloc] calloc not zero!\n", 30); return 1; }
    }
    free(p4);
    write(2, "[test_malloc] test4: OK\n", 24);

    /* Test 5: mmap */
    write(2, "[test_malloc] test5: mmap\n", 26);
    void *p5 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p5 == MAP_FAILED) { write(2, "[test_malloc] mmap FAILED\n", 26); return 1; }
    memset(p5, 0xCC, 4096);
    munmap(p5, 4096);
    write(2, "[test_malloc] test5: OK\n", 24);

    /* Test 6: many small allocations (stress test) */
    write(2, "[test_malloc] test6: stress test\n", 33);
    for (int round = 0; round < 100; round++) {
        void *p = malloc(32);
        if (!p) { write(2, "[test_malloc] stress alloc failed\n", 34); return 1; }
        memset(p, round & 0xFF, 32);
        free(p);
    }
    write(2, "[test_malloc] test6: OK\n", 24);

    write(2, "[test_malloc] ALL TESTS PASSED\n", 31);
    return 0;
}
