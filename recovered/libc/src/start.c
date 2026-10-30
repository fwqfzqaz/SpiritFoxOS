/* SpiritFoxOS libc - C runtime startup
 * Entry point for statically-linked executables.
 * Sets up argc/argv/envp and calls main().
 *
 * crt1.S passes: rdi=argc, rsi=argv, rdx=envp
 */

extern int main(int argc, char **argv, char **envp);
extern void _init(void);
extern void _fini(void);
extern void _exit(int status);

void _start_c(int argc, char **argv, char **envp)
{
    /* Run global constructors */
    _init();

    int ret = main(argc, argv, envp);
    _exit(ret);
}
