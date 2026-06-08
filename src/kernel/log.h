#ifndef LOG_H
#define LOG_H

#include <stdint.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
    LOG_FATAL = 4
} log_level_t;

#define LOG_BUF_SIZE  8192
#define LOG_LINE_MAX  256

void log_init(void);
void log_set_level(log_level_t level);
log_level_t log_get_level(void);
void log_write(log_level_t level, const char *module, const char *fmt, ...);

void log_dump_buffer(void);

int log_save_to_disk(void);
int log_load_from_disk(void);
int log_auto_save_enabled(void);
void log_set_auto_save(int enabled);

int log_read_line(int line_index, char *buf, int buf_size);
int log_get_line_count(void);
void log_clear_buffer(void);

const char *log_level_name(log_level_t level);
const char *log_level_color(log_level_t level);

#define LOG_D(module, ...) log_write(LOG_DEBUG, module, __VA_ARGS__)
#define LOG_I(module, ...) log_write(LOG_INFO,  module, __VA_ARGS__)
#define LOG_W(module, ...) log_write(LOG_WARN,  module, __VA_ARGS__)
#define LOG_E(module, ...) log_write(LOG_ERROR, module, __VA_ARGS__)
#define LOG_F(module, ...) log_write(LOG_FATAL, module, __VA_ARGS__)

#endif
