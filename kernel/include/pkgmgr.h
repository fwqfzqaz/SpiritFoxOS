#ifndef PKGMGR_H
#define PKGMGR_H

#include <stdint.h>
#include "registry.h"

/* ========================================================================
 * Package format constants
 * ======================================================================== */

#define PKG_FORMAT_DEB  1   /* Standard Debian package */
#define PKG_FORMAT_SFK  2   /* SpiritFoxOS native package */

/* ========================================================================
 * Package install flags
 * ======================================================================== */

#define PKG_INSTALL_FORCE     0x01   /* Force install ignoring conflicts */
#define PKG_INSTALL_NODEPS    0x02   /* Skip dependency resolution */
#define PKG_INSTALL_DRYRUN    0x04   /* Simulate only, don't actually install */

/* ========================================================================
 * Package install status
 * ======================================================================== */

#define PKG_STATUS_NOT_INSTALLED   0
#define PKG_STATUS_INSTALLING      1
#define PKG_STATUS_INSTALLED       2
#define PKG_STATUS_BROKEN          3
#define PKG_STATUS_REMOVING        4

/* ========================================================================
 * SFK package permission request entry
 * ======================================================================== */

typedef struct {
    uint32_t perm_flag;     /* SFK_PERM_* bit */
    char     perm_name[32]; /* Human-readable permission name */
    char     perm_desc[128];/* Description of why this permission is needed */
} sfk_perm_request_t;

/* ========================================================================
 * SFK package metadata (embedded in .sfk file)
 * ======================================================================== */

#define SFK_MAGIC       0x53464B21  /* "SFK!" */
#define SFK_VERSION     1

typedef struct {
    uint32_t magic;             /* SFK_MAGIC */
    uint32_t version;           /* Format version */
    char     pkg_id[64];        /* Package identifier (unique) */
    char     pkg_name[64];      /* Display name */
    char     pkg_version[32];   /* Version string */
    char     vendor[64];        /* Vendor/publisher */
    char     description[256];  /* Short description */
    char     category[32];      /* Category (e.g., "utility", "game", "system") */
    char     icon_path[128];    /* Icon resource path inside package */
    char     exec_path[128];    /* Main executable path inside package */
    uint32_t nperms;            /* Number of permission requests */
    uint32_t nreg_entries;      /* Number of registry entries to write */
    uint32_t nfile_assocs;      /* Number of file associations */
    uint32_t nservices;         /* Number of services to register */
    uint32_t ndependencies;     /* Number of dependency strings */
    uint64_t data_offset;       /* Offset to data payload */
    uint64_t data_size;         /* Size of data payload */
    uint64_t meta_offset;       /* Offset to metadata (perms, reg, etc.) */
    uint64_t meta_size;         /* Size of metadata section */
    uint8_t  signature[256];    /* Package signature */
} __attribute__((packed)) sfk_header_t;

/* ========================================================================
 * SFK registry entry declaration (in package metadata)
 * ======================================================================== */

typedef struct {
    char     key_path[256];     /* Registry key path */
    char     value_name[128];   /* Value name */
    uint32_t value_type;        /* REG_TYPE_* */
    uint32_t value_size;        /* Size of value data */
    /* value_data follows immediately after this struct */
} __attribute__((packed)) sfk_reg_entry_t;

/* ========================================================================
 * DEB package control info (parsed from control.tar)
 * ======================================================================== */

typedef struct {
    char     package[64];       /* Package name */
    char     version[32];       /* Version */
    char     architecture[16];  /* Architecture (e.g., "amd64") */
    char     maintainer[128];   /* Maintainer */
    char     description[256];  /* Description */
    char     depends[512];      /* Dependency list */
    char     preinst[256];      /* Pre-install script path */
    char     postinst[256];     /* Post-install script path */
    char     prerm[256];        /* Pre-remove script path */
    char     postrm[256];       /* Post-remove script path */
} deb_control_t;

/* ========================================================================
 * Package info (unified, for both formats)
 * ======================================================================== */

typedef struct {
    uint32_t format;            /* PKG_FORMAT_DEB or PKG_FORMAT_SFK */
    uint32_t status;            /* PKG_STATUS_* */
    char     pkg_id[64];        /* Unique package ID */
    char     pkg_name[64];      /* Display name */
    char     version[32];       /* Version string */
    char     vendor[64];        /* Vendor */
    char     description[256];  /* Description */
    char     install_path[256]; /* Where files are installed */
    uint64_t install_size;      /* Total installed size */
    uint32_t sfk_perms;         /* Granted SFK permissions (0 for DEB) */
    char     dependencies[512]; /* Dependency list */
} pkg_info_t;

/* ========================================================================
 * Package manager API
 * ======================================================================== */

/* Initialize the package manager */
void pkgmgr_init(void);

/* Install a package from a file path
 * For DEB: standard dpkg-like flow, no permission prompts
 * For SFK: permission request flow, interactive authorization
 * Returns 0 on success, negative on error */
int pkgmgr_install(const char *path, uint32_t flags);

/* Install a DEB package - standard flow without permission prompts */
int pkgmgr_install_deb(const char *path, uint32_t flags);

/* Install an SFK package - with permission request flow */
int pkgmgr_install_sfk(const char *path, uint32_t flags,
                       uint32_t *granted_perms);

/* Remove a package by ID */
int pkgmgr_remove(const char *pkg_id, uint32_t flags);

/* Query package info */
int pkgmgr_query(const char *pkg_id, pkg_info_t *info);

/* List all installed packages */
int pkgmgr_list(pkg_info_t *packages, int max_entries);

/* Check if a package is installed */
int pkgmgr_is_installed(const char *pkg_id);

/* Resolve dependencies for a package */
int pkgmgr_resolve_deps(const char *pkg_id, char missing[][64], int max_missing);

/* Verify package integrity */
int pkgmgr_verify(const char *pkg_id);

/* ========================================================================
 * SFK format backend
 * ======================================================================== */

/* Parse SFK header from a .sfk file */
int sfk_parse_header(const void *data, size_t size, sfk_header_t *header);

/* Get SFK permission requests */
int sfk_get_perm_requests(const void *data, size_t size,
                          sfk_perm_request_t *perms, int max_perms);

/* Get SFK registry declarations */
int sfk_get_reg_entries(const void *data, size_t size,
                        sfk_reg_entry_t **entries, int *count);

/* Get SFK file associations */
int sfk_get_file_assocs(const void *data, size_t size,
                        reg_fileassoc_t *assocs, int max_assocs);

#endif /* PKGMGR_H */
