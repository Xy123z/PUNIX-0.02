#ifndef FS_H
#define FS_H
#include "types.h"

// --- Constants ---
#define FS_TYPE_FILE        0
#define FS_TYPE_DIRECTORY   1
#define FS_TYPE_CHARDEV     2
#define FS_TYPE_BLOCKDEV    3       // NEW: block device (e.g. /dev/hda, /dev/hdb)
#define FS_MAX_NAME         60
#define FS_MAX_INODES       1024
#define SECTOR_SIZE         512

// Permission bits & File Types
#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFCHR 0020000
#define S_IFBLK 0060000             // NEW: block device type bit

#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

// --- Filesystem State Flags (stored in superblock.fs_state) ---
#define FS_STATE_CLEAN  0
#define FS_STATE_DIRTY  1

// --- Disk Layout ---
#define FS_SUPERBLOCK_SECTOR    256
#define FS_INODE_BITMAP_SECTOR  257
#define FS_BLOCK_BITMAP_START   258
#define FS_BLOCK_BITMAP_COUNT   25
#define FS_INODE_TABLE_START    283
#define FS_INODE_TABLE_COUNT    256
#define FS_DATA_BLOCKS_START    539

// --- ATA block device encoding stored in inode.indirect_block ---
// Upper byte = major number (3 = primary IDE bus, matching Linux convention).
// Lower byte = drive index (0 = master / hda, 1 = slave / hdb, …).
#define ATA_BLK_MAJOR        3
#define ATA_BLK_ENCODE(drv)  ((ATA_BLK_MAJOR << 8) | (drv))
#define ATA_BLK_DRIVE(enc)   ((enc) & 0xFF)

// --- Data Structures ---

/**
 * @brief 128-byte on-disk Inode structure.
 */
typedef struct inode {
    uint32_t id;
    uint32_t parent_id;
    uint8_t  type;
    uint8_t  _pad0[3];
    uint32_t mode;
    uint16_t link_count;
    uint8_t  _pad1[2];
    uint32_t uid;
    uint32_t gid;
    uint32_t size;
    uint32_t block_count;
    uint32_t blocks[12];
    uint32_t indirect_block;        // For CHARDEV/BLOCKDEV: (major<<8)|minor
    uint32_t double_indirect_block;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint8_t  padding[24];
} inode_t;

typedef inode_t fs_node_t;

/**
 * @brief 64-byte Directory Entry.
 */
typedef struct {
    uint32_t inode_id;
    char     name[FS_MAX_NAME];
} fs_dirent_t;

// --- Global State ---
extern uint32_t fs_root_id;
extern uint32_t fs_current_dir_id;

// --- Function Prototypes ---
void        fs_init();
fs_node_t*  fs_get_node(uint32_t id);
fs_node_t*  fs_find_node(const char* path, uint32_t start_id);
int         fs_update_node(fs_node_t* node);
uint32_t    fs_find_node_local_id(uint32_t parent_id, char* name);
int         fs_create_node(uint32_t parent_id, char* name, uint8_t type, uint32_t uid, uint32_t gid);
int         fs_check_permission(inode_t* node, uint32_t uid, uint32_t gid, uint32_t mask);
int         fs_delete_node(uint32_t id);
void        fs_get_disk_stats(uint32_t* total_kb, uint32_t* used_kb, uint32_t* free_kb);
void        fs_get_cache_stats(uint32_t* cache_size, uint32_t* cached_nodes, uint32_t* dirty_nodes);
int         fs_get_inode_name(uint32_t id, char* buffer);
int         fs_read(inode_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
int         fs_write(inode_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
void        fs_sync();
void        fs_get_full_path(uint32_t id, char* buffer);
int         fs_fsck(int repair);

#endif // FS_H
