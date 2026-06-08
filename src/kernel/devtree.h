#ifndef DEVTREE_H
#define DEVTREE_H

#include <stdint.h>

/* Device types */
typedef enum {
    DEV_TYPE_CPU = 0,
    DEV_TYPE_MEMORY,
    DEV_TYPE_STORAGE,
    DEV_TYPE_INPUT,
    DEV_TYPE_DISPLAY,
    DEV_TYPE_NETWORK,
    DEV_TYPE_USB,
    DEV_TYPE_PCI,
    DEV_TYPE_OTHER
} dev_type_t;

/* Device status */
typedef enum {
    DEV_STATUS_UNKNOWN = 0,
    DEV_STATUS_OK,
    DEV_STATUS_FAILED,
    DEV_STATUS_MISSING,
    DEV_STATUS_DISABLED
} dev_status_t;

/* Device criticality */
typedef enum {
    DEV_CRITICAL_NO = 0,
    DEV_CRITICAL_YES
} dev_critical_t;

/* Device tree entry */
#define DEV_NAME_MAX 32
#define DEV_INFO_MAX 64

typedef struct {
    char name[DEV_NAME_MAX];
    dev_type_t type;
    dev_status_t status;
    dev_critical_t critical;
    char info[DEV_INFO_MAX];
    uint32_t vendor_id;
    uint32_t device_id;
} devtree_entry_t;

#define DEVTREE_MAX_ENTRIES 64

/* Initialize the device tree */
void devtree_init(void);

/* Register a device in the tree */
int devtree_register(const char *name, dev_type_t type, dev_status_t status,
                     dev_critical_t critical, const char *info,
                     uint32_t vendor_id, uint32_t device_id);

/* Update a device's status */
int devtree_update_status(const char *name, dev_status_t status);

/* Check if all critical devices are OK (returns 1 if pass, 0 if fail) */
int devtree_check_critical(void);

/* Get the number of failed critical devices */
int devtree_get_critical_fail_count(void);

/* Print all device errors to console */
void devtree_print_errors(void);

/* Print the full device tree */
void devtree_print_all(void);

/* Get device count */
int devtree_get_count(void);

/* Get device entry by index */
devtree_entry_t *devtree_get_entry(int index);

#endif /* DEVTREE_H */
