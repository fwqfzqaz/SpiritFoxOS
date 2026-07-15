#ifndef AUTORUN_H
#define AUTORUN_H

/* Default autorun configuration file path */
#define AUTORUN_DEFAULT_PATH  "/etc/autorun.cfg"

/*
 * Read and execute commands from the autorun configuration file.
 * Each non-empty, non-comment line is treated as a shell command.
 * Lines starting with '#' are comments and are skipped.
 *
 * If `path` is NULL, uses AUTORUN_DEFAULT_PATH ("/etc/autorun.cfg").
 * Returns the number of commands executed, or -1 if the file could not be opened.
 */
int autorun_execute(const char *path);

/*
 * Create a default autorun.cfg if one does not already exist.
 * Returns 0 on success, -1 on failure.
 */
int autorun_create_default(const char *path);

#endif /* AUTORUN_H */
