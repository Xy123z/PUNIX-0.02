#ifndef VFS_H
#define VFS_H

#include "types.h"
#include "fs.h"

#define VFS_MAX_MOUNTS 8

struct fs_ops;
struct vfs_superblock;
struct dirent;


// A generic VFS node representing a file/directory inside a mounted FS
typedef struct vfs_node {
    uint32_t inode_id;       // Driver-specific identifier
    uint8_t  type;           // FS_TYPE_FILE / FS_TYPE_DIRECTORY
    uint32_t mode;
    uint32_t size;
    uint32_t uid;
    uint32_t gid;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    struct vfs_superblock* sb; // The FS this node belongs to
} vfs_node_t;

// Standard VFS operations provided by the specific FS driver
typedef struct fs_ops {
    int (*read)(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
    int (*write)(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
    vfs_node_t* (*lookup)(vfs_node_t* start_node, const char* path);
    int (*create)(struct vfs_superblock* sb, const char* path, uint8_t type, uint32_t uid, uint32_t gid);
    int (*getdents)(vfs_node_t* dir_node, struct dirent* dirents, int max);
} fs_ops_t;

// A generic VFS superblock
typedef struct vfs_superblock {
    fs_ops_t ops;
    uint8_t  drive_index;    // 0 = hda, 1 = hdb, etc.
    uint8_t  mount_index;    // Index in the VFS mount array
    void*    fs_info;        // Driver-specific superblock info
    vfs_node_t root_node;    // The root node of this mounted filesystem
} vfs_superblock_t;

// A mount table entry
typedef struct vfs_mount_entry {
    int active;
    char mountpoint[64];       // e.g., "/mnt/fs1"
    uint32_t native_mountpoint_id; // The native FS inode ID that this mounts over
    char device[32];           // e.g., "/dev/hdb"
    char fs_type[16];          // e.g., "minix"
    vfs_superblock_t sb;
} vfs_mount_entry_t;

// VFS Public Interface
void vfs_init();
int vfs_mount(const char* device, const char* mountpoint, const char* fs_type);
vfs_mount_entry_t* vfs_get_mount_by_path(const char* path, const char** out_rel_path);
vfs_mount_entry_t* vfs_get_mount_by_native_id(uint32_t native_id);
vfs_mount_entry_t* vfs_get_mount(int slot);

#endif // VFS_H
