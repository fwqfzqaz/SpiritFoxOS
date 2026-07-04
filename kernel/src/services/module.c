#include "module.h"
#include "string.h"
#include "vga.h"

static Module* module_head = (void*)0;
static int module_count = 0;

void module_init(void) {
    module_head = (void*)0;
    module_count = 0;
}

int module_register(Module* mod) {
    if (mod == (void*)0)
        return -1;

    if (module_count >= MODULE_MAX) {
        printf("module: maximum number of modules (%d) reached\n", MODULE_MAX);
        return -1;
    }

    if (module_find(mod->name) != (void*)0) {
        printf("module: '%s' already registered\n", mod->name);
        return -1;
    }

    mod->next = module_head;
    module_head = mod;
    module_count++;

    return 0;
}

int module_unregister(const char* name) {
    if (name == (void*)0)
        return -1;

    Module* prev = (void*)0;
    Module* curr = module_head;

    while (curr != (void*)0) {
        if (strcmp(curr->name, name) == 0) {
            if (curr->loaded) {
                printf("module: cannot unregister '%s', still loaded\n", name);
                return -1;
            }
            if (prev == (void*)0) {
                module_head = curr->next;
            } else {
                prev->next = curr->next;
            }
            module_count--;
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    printf("module: '%s' not found\n", name);
    return -1;
}

int module_load(const char* name) {
    Module* mod = module_find(name);
    if (mod == (void*)0) {
        printf("module: '%s' not found\n", name);
        return -1;
    }

    if (mod->loaded) {
        printf("module: '%s' already loaded\n", name);
        return -1;
    }

    if (mod->init) {
        int ret = mod->init();
        if (ret != 0) {
            printf("module: '%s' init failed (code %d)\n", name, ret);
            return ret;
        }
    }

    mod->loaded = 1;
    printf("module: '%s' loaded (v%s)\n", name, mod->version);
    return 0;
}

int module_unload(const char* name) {
    Module* mod = module_find(name);
    if (mod == (void*)0) {
        printf("module: '%s' not found\n", name);
        return -1;
    }

    if (!mod->loaded) {
        printf("module: '%s' is not loaded\n", name);
        return -1;
    }

    if (mod->cleanup) {
        mod->cleanup();
    }

    mod->loaded = 0;
    printf("module: '%s' unloaded\n", name);
    return 0;
}

void module_list(void) {
    if (module_head == (void*)0) {
        printf("No modules registered.\n");
        return;
    }

    printf("%-20s %-10s %-8s %s\n", "Name", "Version", "Status", "Description");
    printf("---------------------------------------------------------------\n");

    Module* curr = module_head;
    while (curr != (void*)0) {
        printf("%-20s %-10s %-8s %s\n",
               curr->name,
               curr->version,
               curr->loaded ? "Loaded" : "Reg",
               curr->description);
        curr = curr->next;
    }
}

Module* module_find(const char* name) {
    Module* curr = module_head;
    while (curr != (void*)0) {
        if (strcmp(curr->name, name) == 0)
            return curr;
        curr = curr->next;
    }
    return (void*)0;
}

int module_handle_command(const char* name, int argc, char** argv) {
    Module* mod = module_find(name);
    if (mod == (void*)0)
        return -1;

    if (!mod->loaded) {
        printf("module: '%s' is not loaded\n", name);
        return -1;
    }

    if (mod->cmd_handler == (void*)0) {
        printf("module: '%s' has no command handler\n", name);
        return -1;
    }

    return mod->cmd_handler(argc, argv);
}
