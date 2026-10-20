/* Call the production shell cat handler with deterministic VFS read results. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/services/shell.c"

void terminal_putchar(char character) { (void)character; }

static int scenario;
static int read_calls;
static int close_calls;
static char output[16];
static size_t output_size;

int vfs_open(const char *path, uint32_t flags, uint32_t mode)
{
    (void)flags;
    (void)mode;
    assert(!strcmp(path, "/input"));
    return 42;
}

int vfs_read(int fd, void *buf, size_t count)
{
    assert(fd == ((scenario == 3 || scenario == 4) ? 77 : 42));
    read_calls++;
    if (scenario == 1 || scenario == 3 || (scenario == 2 && read_calls == 2))
        return -5; /* deterministic I/O error */
    if (scenario == 0 || scenario == 2 || scenario == 4) {
        if (read_calls == 1) {
            const char *text = scenario == 2 ? "part" : "hello";
            size_t length = strlen(text);
            assert(length <= count);
            memcpy(buf, text, length);
            return (int)length;
        }
        return 0; /* clean EOF */
    }
    assert(!"unexpected read scenario");
    return -5;
}

int vfs_write(int fd, const void *buf, size_t count)
{
    assert(fd == 41 && count <= sizeof(output) - output_size);
    memcpy(output + output_size, buf, count);
    output_size += count;
    return (int)count;
}

int vfs_close(int fd)
{
    assert(fd == 42);
    close_calls++;
    return 0;
}

static int run_cat(int stdin_mode)
{
    shell_stdin_fd = stdin_mode ? 77 : -1;
    shell_stdout_fd = 41;
    char *file_argv[] = {"cat", "/input"};
    char *stdin_argv[] = {"cat"};
    return cmd_cat(stdin_mode ? 1 : 2, stdin_mode ? stdin_argv : file_argv);
}

int main(int argc, char **argv)
{
    int baseline = argc == 2 && !strcmp(argv[1], "--baseline");
    scenario = 1;
    int result = run_cat(0);
    assert(!output_size && close_calls == 1 && read_calls == 1);
    if (baseline) {
        assert(result == 0);
        puts("reproduced: cat returns success after a VFS read error");
        return 0;
    }
    assert(result < 0);

    scenario = 2;
    read_calls = close_calls = 0;
    output_size = 0;
    result = run_cat(0);
    assert(result < 0 && output_size == 4 && !memcmp(output, "part", 4));
    assert(close_calls == 1 && read_calls == 2);

    scenario = 0;
    read_calls = close_calls = 0;
    output_size = 0;
    result = run_cat(0);
    assert(result == 0 && output_size == 5 && !memcmp(output, "hello", 5));
    assert(close_calls == 1 && read_calls == 2);

    scenario = 3;
    read_calls = close_calls = 0;
    output_size = 0;
    result = run_cat(1);
    assert(result < 0 && !output_size && close_calls == 0 && read_calls == 1);

    scenario = 4;
    read_calls = close_calls = 0;
    output_size = 0;
    result = run_cat(1);
    assert(result == 0 && output_size == 5 && !memcmp(output, "hello", 5));
    assert(close_calls == 0 && read_calls == 2);
    puts("shell cat read-error and EOF paths passed");
    return 0;
}
