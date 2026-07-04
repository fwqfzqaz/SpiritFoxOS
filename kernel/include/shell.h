#ifndef SHELL_H
#define SHELL_H

#define SHELL_MAX_LINE  256
#define SHELL_MAX_ARGS  16

void shell_init(void);
void shell_run(void);
int shell_execute(const char* cmd, int argc, char** argv);

#endif /* SHELL_H */
