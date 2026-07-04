#ifndef MODULE_H
#define MODULE_H

#define MODULE_MAX 32
#define MODULE_NAME_LEN 32
#define MODULE_VERSION_LEN 16
#define MODULE_DESC_LEN 64

typedef struct Module {
    char name[MODULE_NAME_LEN];
    char version[MODULE_VERSION_LEN];
    char description[MODULE_DESC_LEN];
    int (*init)(void);
    void (*cleanup)(void);
    int (*cmd_handler)(int argc, char** argv);
    int loaded;
    struct Module* next;
} Module;

void module_init(void);
int module_register(Module* mod);
int module_unregister(const char* name);
int module_load(const char* name);
int module_unload(const char* name);
void module_list(void);
Module* module_find(const char* name);
int module_handle_command(const char* name, int argc, char** argv);

#endif /* MODULE_H */
