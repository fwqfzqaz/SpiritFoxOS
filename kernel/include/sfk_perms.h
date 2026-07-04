#ifndef SFK_PERMS_H
#define SFK_PERMS_H

/* SFK permission flags */
#define SFK_PERM_NETWORK        0x0001
#define SFK_PERM_FILE_READ      0x0002
#define SFK_PERM_FILE_WRITE     0x0004
#define SFK_PERM_AUDIO          0x0008
#define SFK_PERM_CAMERA         0x0010
#define SFK_PERM_LOCATION       0x0020
#define SFK_PERM_NOTIFICATIONS  0x0040
#define SFK_PERM_SYSSERVICE     0x0080
#define SFK_PERM_DEVICE_ACCESS  0x0100
#define SFK_PERM_PROCESS_LIST   0x0200
#define SFK_PERM_PROCESS_KILL   0x0400
#define SFK_PERM_SETUID         0x0800
#define SFK_PERM_ADMIN          0x1000

#endif /* SFK_PERMS_H */
