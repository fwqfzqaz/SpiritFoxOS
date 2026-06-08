#ifndef INIT_H
#define INIT_H

#include <stdint.h>

/* Run the init process (first user-space process) */
void init_process(void);

/* Run safe mode (minimal environment when critical devices fail) */
void safe_mode_run(void);

#endif /* INIT_H */
