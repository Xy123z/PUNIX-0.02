/**
 * src/fs.c - Lazy Loading Filesystem (Load-On-Demand)
 *
 * Changes from previous version:
 *  - FS_TYPE_BLOCKDEV support in fs_create_node() (mode = S_IFBLK|0660,
 *    indirect_block = ATA_BLK_ENCODE(drive_index))
 *  - fs_init() registers /dev/hda, /dev/hdb … for every drive reported
 *    present in ata_drive_present[].  hda = master (index 0, the OS disk),
 *    hdb = slave (index 1, e.g. the Minix FS image), and so on.
 *    Drive letter is computed as 'a' + drive_index so the mapping is stable.
 */

#include "../include/fs.h"
#include "../include/string.h"
#include "../include/memory.h"
#include "../include/console.h"
#include "../include/ata.h"
#include "../include/task.h"
#include "../include/vfs.h"

// --- Configuration ---
#define FS_MAGIC         0xEF5342
#define FS_ROOT_ID       1
#define SECTOR_SIZE      512
#define PTRS_PER_BLOCK   (SECTOR_SIZE / sizeof(uint32_t))  // 128

// --- Cache Management ---
#define FS_CACHE_SIZE    64

typedef struct {
    fs_node_t node;
    uint32_t  id;
    uint32_t  last_access;
    uint8_t   dirty;
} fs_cache_entry_t;

// --- Superblock ---
typedef struct {
    uint32_t magic;
    uint32_t root_inode;
    uint32_t total_inodes;
    uint32_t total_blocks;
    uint32_t free_inodes;
    uint32_t free_blocks;
    uint8_t  fs_state;
    uint8_t  reserved[487];
} superblock_t;

// --- Globals ---
uint32_t fs_root_id        = FS_ROOT_ID;
uint32_t fs_current_dir_id = FS_ROOT_ID;

static superblock_t      sb;
static fs_cache_entry_t  cache[FS_CACHE_SIZE];
static uint32_t          access_counter = 0;

// --- Timestamp helper ---
extern volatile uint32_t current_unix_time;
static uint32_t fs_timestamp() {
    return current_unix_time;
}

// Helper macros
#define INODE_TO_SECTOR(id)      (FS_INODE_TABLE_START + ((id) / (SECTOR_SIZE / sizeof(inode_t))))
#define INODE_OFF_IN_SECTOR(id)  (((id) % (SECTOR_SIZE / sizeof(inode_t))) * sizeof(inode_t))

// ============================================================
// Bitmap Helpers
// ============================================================

static int bitmap_get(uint32_t sector_start, uint32_t bit) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t sector  = sector_start + (bit / (SECTOR_SIZE * 8));
    uint32_t bit_off = bit % (SECTOR_SIZE * 8);
    ata_read_sectors(sector, 1, buf);
    return (buf[bit_off / 8] & (1 << (bit_off % 8))) != 0;
}

static void bitmap_set(uint32_t sector_start, uint32_t bit, int value) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t sector  = sector_start + (bit / (SECTOR_SIZE * 8));
    uint32_t bit_off = bit % (SECTOR_SIZE * 8);
    ata_read_sectors(sector, 1, buf);
    if (value) buf[bit_off / 8] |=  (1 << (bit_off % 8));
    else       buf[bit_off / 8] &= ~(1 << (bit_off % 8));
    ata_write_sectors(sector, 1, buf);
}

static int bitmap_find_free(uint32_t sector_start, uint32_t count) {
    uint8_t buf[SECTOR_SIZE];
    for (uint32_t s = 0; s < (count + 4095) / 4096; s++) {
        ata_read_sectors(sector_start + s, 1, buf);
        for (int i = 0; i < SECTOR_SIZE; i++) {
            if (buf[i] != 0xFF) {
                for (int b = 0; b < 8; b++) {
                    if (!(buf[i] & (1 << b))) {
                        uint32_t res = (s * 4096) + (i * 8) + b;
                        if (res < count) return res;
                    }
                }
            }
        }
    }
    return -1;
}

// ============================================================
// Internal Helpers
// ============================================================

static void save_superblock() {
    ata_write_sectors(FS_SUPERBLOCK_SECTOR, 1, &sb);
}

static void save_node(uint32_t id);

static int inode_alloc() {
    int id = bitmap_find_free(FS_INODE_BITMAP_SECTOR, FS_MAX_INODES);
    if (id < 0) return -1;
    bitmap_set(FS_INODE_BITMAP_SECTOR, id, 1);
    sb.free_inodes--;
    save_superblock();
    return id;
}

static int block_alloc() {
    int id = bitmap_find_free(FS_BLOCK_BITMAP_START, sb.total_blocks);
    if (id < 0) return -1;
    bitmap_set(FS_BLOCK_BITMAP_START, id, 1);
    sb.free_blocks--;
    save_superblock();
    uint8_t zero[SECTOR_SIZE];
    memset(zero, 0, SECTOR_SIZE);
    ata_write_sectors(id, 1, zero);
    return id;
}

static void block_free(uint32_t id) {
    if (id < FS_DATA_BLOCKS_START) return;
    bitmap_set(FS_BLOCK_BITMAP_START, id, 0);
    sb.free_blocks++;
    save_superblock();
}

// --- Cache ---

static int cache_find(uint32_t id) {
    for (int i = 0; i < FS_CACHE_SIZE; i++) {
        if (cache[i].id == id) {
            cache[i].last_access = ++access_counter;
            return i;
        }
    }
    return -1;
}

static int cache_find_slot() {
    for (int i = 0; i < FS_CACHE_SIZE; i++) {
        if (cache[i].id == 0) return i;
    }
    int lru_index = 0;
    uint32_t oldest = cache[0].last_access;
    for (int i = 1; i < FS_CACHE_SIZE; i++) {
        if (cache[i].last_access < oldest) {
            oldest    = cache[i].last_access;
            lru_index = i;
        }
    }
    if (cache[lru_index].dirty) {
        save_node(cache[lru_index].id);
    }
    return lru_index;
}

static inode_t* cache_load(uint32_t id) {
    if (id == 0 || id >= FS_MAX_INODES) return 0;

    int idx = cache_find(id);
    if (idx >= 0) return &cache[idx].node;

    int slot = cache_find_slot();

    uint8_t sector_buf[SECTOR_SIZE];
    ata_read_sectors(INODE_TO_SECTOR(id), 1, sector_buf);
    memcpy(&cache[slot].node, sector_buf + INODE_OFF_IN_SECTOR(id), sizeof(inode_t));

    if (cache[slot].node.id != id) return 0;

    cache[slot].id          = id;
    cache[slot].dirty       = 0;
    cache[slot].last_access = ++access_counter;
    return &cache[slot].node;
}

static void cache_mark_dirty(uint32_t id) {
    int idx = cache_find(id);
    if (idx >= 0) cache[idx].dirty = 1;
}

static void save_node(uint32_t id) {
    if (id == 0 || id >= FS_MAX_INODES) return;

    int idx = cache_find(id);
    inode_t* node_ptr = 0;

    if (idx >= 0) {
        node_ptr = &cache[idx].node;
    } else {
        node_ptr = cache_load(id);
    }
    if (!node_ptr) return;

    uint8_t sector_buf[SECTOR_SIZE];
    ata_read_sectors(INODE_TO_SECTOR(id), 1, sector_buf);
    memcpy(sector_buf + INODE_OFF_IN_SECTOR(id), node_ptr, sizeof(inode_t));
    ata_write_sectors(INODE_TO_SECTOR(id), 1, sector_buf);

    if (idx >= 0) cache[idx].dirty = 0;
}

static int dir_add_entry(inode_t* dir_node, uint32_t inode_id, const char* name) {
    fs_dirent_t entries[SECTOR_SIZE / sizeof(fs_dirent_t)];

    for (int i = 0; i < 12; i++) {
        uint32_t b_id = dir_node->blocks[i];
        if (b_id == 0) {
            b_id = block_alloc();
            if (b_id == 0) return 0;
            dir_node->blocks[i] = b_id;
            dir_node->block_count++;
            memset(entries, 0, SECTOR_SIZE);
        } else {
            ata_read_sectors(b_id, 1, entries);
        }

        for (int j = 0; j < (int)(SECTOR_SIZE / sizeof(fs_dirent_t)); j++) {
            if (entries[j].inode_id == 0) {
                entries[j].inode_id = inode_id;
                strncpy(entries[j].name, name, FS_MAX_NAME - 1);
                entries[j].name[FS_MAX_NAME - 1] = '\0';
                ata_write_sectors(b_id, 1, entries);
                dir_node->size += sizeof(fs_dirent_t);
                return 1;
            }
        }
    }
    return 0;
}

// ============================================================
// mkfs
// ============================================================

static void mkfs() {
    console_print_colored("FS: Formatting drive (UNIX-Lite)...\n", COLOR_YELLOW_ON_BLACK);

    uint8_t zero_sector[SECTOR_SIZE];
    memset(zero_sector, 0, SECTOR_SIZE);

    ata_write_sectors(FS_INODE_BITMAP_SECTOR, 1, zero_sector);

    for (int i = 0; i < FS_BLOCK_BITMAP_COUNT; i++) {
        ata_write_sectors(FS_BLOCK_BITMAP_START + i, 1, zero_sector);
    }

    for (int i = 0; i < FS_INODE_TABLE_COUNT; i++) {
        ata_write_sectors(FS_INODE_TABLE_START + i, 1, zero_sector);
    }

    memset(&sb, 0, sizeof(sb));
    sb.magic        = FS_MAGIC;
    sb.root_inode   = FS_ROOT_ID;
    sb.total_inodes = FS_MAX_INODES;
    sb.total_blocks = 102400;
    sb.free_inodes  = FS_MAX_INODES;
    sb.free_blocks  = sb.total_blocks - FS_DATA_BLOCKS_START;
    sb.fs_state     = FS_STATE_DIRTY;
    save_superblock();

    bitmap_set(FS_INODE_BITMAP_SECTOR, 0, 1);
    for (int i = 0; i < FS_DATA_BLOCKS_START; i++) {
        bitmap_set(FS_BLOCK_BITMAP_START, i, 1);
    }

    inode_t root;
    memset(&root, 0, sizeof(inode_t));
    root.id         = FS_ROOT_ID;
    root.parent_id  = FS_ROOT_ID;
    root.type       = FS_TYPE_DIRECTORY;
    root.mode       = S_IFDIR | 0755;
    root.link_count = 2;
    root.ctime      = root.mtime = root.atime = fs_timestamp();

    uint8_t buf[SECTOR_SIZE];
    ata_read_sectors(INODE_TO_SECTOR(FS_ROOT_ID), 1, buf);
    memcpy(buf + INODE_OFF_IN_SECTOR(FS_ROOT_ID), &root, sizeof(inode_t));
    ata_write_sectors(INODE_TO_SECTOR(FS_ROOT_ID), 1, buf);

    bitmap_set(FS_INODE_BITMAP_SECTOR, FS_ROOT_ID, 1);
    sb.free_inodes--;
    save_superblock();

    uint32_t root_block = block_alloc();
    root.blocks[0]   = root_block;
    root.block_count = 1;

    fs_dirent_t root_entries[SECTOR_SIZE / sizeof(fs_dirent_t)];
    memset(root_entries, 0, SECTOR_SIZE);
    root_entries[0].inode_id = FS_ROOT_ID;
    strncpy(root_entries[0].name, ".",  FS_MAX_NAME - 1);
    root_entries[1].inode_id = FS_ROOT_ID;
    strncpy(root_entries[1].name, "..", FS_MAX_NAME - 1);
    ata_write_sectors(root_block, 1, root_entries);

    root.size = 2 * sizeof(fs_dirent_t);

    ata_read_sectors(INODE_TO_SECTOR(FS_ROOT_ID), 1, buf);
    memcpy(buf + INODE_OFF_IN_SECTOR(FS_ROOT_ID), &root, sizeof(inode_t));
    ata_write_sectors(INODE_TO_SECTOR(FS_ROOT_ID), 1, buf);

    fs_create_node(FS_ROOT_ID, "home", FS_TYPE_DIRECTORY, 0, 0);
    fs_create_node(FS_ROOT_ID, "bin",  FS_TYPE_DIRECTORY, 0, 0);
    fs_create_node(FS_ROOT_ID, "etc",  FS_TYPE_DIRECTORY, 0, 0);

    console_print_colored("FS: Format complete.\n", COLOR_GREEN_ON_BLACK);
}

// ============================================================
// Public API
// ============================================================

void fs_init() {
    memset(cache, 0, sizeof(cache));
    access_counter = 0;

    ata_read_sectors(FS_SUPERBLOCK_SECTOR, 1, &sb);

    if (sb.magic != FS_MAGIC) {
        console_print_colored("FS: No filesystem detected. Formatting...\n", COLOR_LIGHT_RED);
        mkfs();
    } else {
        if (sb.fs_state == FS_STATE_DIRTY) {
            console_print_colored(
                "FS: WARNING - Filesystem was not cleanly unmounted!\n"
                "FS: Run fs_fsck(1) to check and repair. Mounting anyway...\n",
                COLOR_LIGHT_RED);
        } else {
            console_print_colored("FS: PUNIX-FS mounted (clean).\n", COLOR_GREEN_ON_BLACK);
        }
        sb.fs_state = FS_STATE_DIRTY;
        save_superblock();
    }

    fs_root_id        = FS_ROOT_ID;
    fs_current_dir_id = FS_ROOT_ID;

    // -----------------------------------------------------------------
    // Ensure /dev exists
    // -----------------------------------------------------------------
    fs_node_t* dev_dir = fs_find_node("/dev", FS_ROOT_ID);
    if (!dev_dir) {
        fs_create_node(FS_ROOT_ID, "dev", FS_TYPE_DIRECTORY, 0, 0);
        dev_dir = fs_find_node("/dev", FS_ROOT_ID);
    }

    if (dev_dir) {
        // -----------------------------------------------------------------
        // Register TTY character devices: /dev/tty0–3, /dev/ttyS0–3
        // -----------------------------------------------------------------
        char tty_name[16];
        for (int i = 0; i < 8; i++) {
            if (i >= 4) {
                strcpy(tty_name, "ttyS");
                tty_name[4] = '0' + (i - 4);
                tty_name[5] = '\0';
            } else {
                strcpy(tty_name, "tty");
                tty_name[3] = '0' + i;
                tty_name[4] = '\0';
            }
            if (!fs_find_node_local_id(dev_dir->id, tty_name)) {
                fs_create_node(dev_dir->id, tty_name, FS_TYPE_CHARDEV, 0, 0);
                fs_node_t* tty_node = fs_find_node(tty_name, dev_dir->id);
                if (tty_node) {
                    tty_node->indirect_block = (4 << 8) | i;
                    fs_update_node(tty_node);
                }
            }
        }

        // -----------------------------------------------------------------
        // Register ATA block devices: /dev/hda, /dev/hdb, …
        //
        // Drive index → device name mapping:
        //   0 (ATA_DRIVE_MASTER) → hda   (the OS disk — registered but not
        //                                  mounted by the Minix FS layer)
        //   1 (ATA_DRIVE_SLAVE)  → hdb   (e.g. the Minix FS image)
        //   2 …                  → hdc …  (future expansion)
        //
        // Only drives confirmed present by ata_init() are registered.
        // The inode's indirect_block stores ATA_BLK_ENCODE(drive_index) so
        // the rest of the kernel can recover the drive index with
        // ATA_BLK_DRIVE(node->indirect_block) and call ata_read_sectors_ex().
        // -----------------------------------------------------------------
        for (int drive = 0; drive < 2; drive++) {
            if (!ata_drive_present[drive]) continue;

            // Build name: "hd" + ('a' + drive_index)
            char hd_name[8];
            hd_name[0] = 'h';
            hd_name[1] = 'd';
            hd_name[2] = (char)('a' + drive);
            hd_name[3] = '\0';

            if (!fs_find_node_local_id(dev_dir->id, hd_name)) {
                fs_create_node(dev_dir->id, hd_name, FS_TYPE_BLOCKDEV, 0, 0);
            }

            // Always refresh the node to (re)set indirect_block in case this
            // is a re-mount of an existing filesystem image.
            fs_node_t* hd_node = fs_find_node(hd_name, dev_dir->id);
            if (hd_node) {
                hd_node->indirect_block = ATA_BLK_ENCODE(drive);
                fs_update_node(hd_node);

                // Friendly boot message
                console_print_colored("FS: Registered /dev/", COLOR_GREEN_ON_BLACK);
                console_print_colored(hd_name,                COLOR_GREEN_ON_BLACK);
                console_print_colored("\n",                    COLOR_GREEN_ON_BLACK);
            }
        }
    }

    // Set working directory to /home if it exists
    inode_t* root = cache_load(FS_ROOT_ID);
    if (root) {
        fs_node_t* dir_home = fs_find_node("home", FS_ROOT_ID);
        if (dir_home && dir_home->type == FS_TYPE_DIRECTORY) {
            fs_current_dir_id = dir_home->id;
        }
    }
}

fs_node_t* fs_get_node(uint32_t id) {
    return cache_load(id);
}

int fs_update_node(fs_node_t* node) {
    if (!node || node->id == 0) return 0;
    cache_mark_dirty(node->id);
    save_node(node->id);
    return 1;
}

int fs_check_permission(inode_t* node, uint32_t uid, uint32_t gid, uint32_t mask) {
    if (uid == 0) return 1;

    if (uid == node->uid) {
        if (((node->mode >> 6) & mask) == mask) return 1;
        return 0;
    }
    if (gid == node->gid) {
        if (((node->mode >> 3) & mask) == mask) return 1;
    }
    if ((node->mode & mask) == mask) return 1;
    return 0;
}

uint32_t fs_find_node_local_id(uint32_t parent_id, char* name) {
    inode_t* parent = cache_load(parent_id);
    if (!parent || parent->type != FS_TYPE_DIRECTORY) return 0;

    fs_dirent_t entries[SECTOR_SIZE / sizeof(fs_dirent_t)];

    for (int i = 0; i < 12; i++) {
        uint32_t b_id = parent->blocks[i];
        if (b_id == 0) continue;
        ata_read_sectors(b_id, 1, entries);
        for (int j = 0; j < (int)(SECTOR_SIZE / sizeof(fs_dirent_t)); j++) {
            if (entries[j].inode_id != 0 && strcmp(entries[j].name, name) == 0) {
                return entries[j].inode_id;
            }
        }
    }
    return 0;
}

fs_node_t* fs_find_node(const char* path, uint32_t start_id) {
    if (!path) return 0;
    
    if (path[0] == '/') {
        start_id = fs_root_id;
        path++;
    }
    
    // --- VFS Hook Start ---
    const char* rel_path;
    vfs_mount_entry_t* mnt = vfs_get_mount_by_path(path, &rel_path);
    if (mnt && mnt->sb.ops.lookup) {
        // We need to return a generic fs_node_t shaped object, but the file descriptor
        // management expects `fs_node_t` to be an in-memory `inode_t` with an ID.
        // What we will do is return a special "fake" inode that signifies a VFS node.
        vfs_node_t* vnode = mnt->sb.ops.lookup(&mnt->sb.root_node, rel_path);
        if (vnode) {
            // Pack vfs info into an inode_t so syscall layer can pass it around
            // Use id > FS_MAX_INODES (like 0x80000000 | (mount_idx << 24) | VFS pointer) to identify VFS nodes
            inode_t* fake = (inode_t*)kmalloc(sizeof(inode_t)); 
            memset(fake, 0, sizeof(inode_t));
            fake->id = 0x80000000 | ((uint32_t)mnt->sb.mount_index << 24) | (uint32_t)vnode->inode_id;
            fake->type = vnode->type;
            fake->mode = vnode->mode;
            fake->size = vnode->size;
            fake->uid = vnode->uid;
            fake->gid = vnode->gid;
            fake->atime = vnode->atime;
            fake->mtime = vnode->mtime;
            fake->ctime = vnode->ctime;
            // Store vnode pointer in a safe place, maybe double_indirect_block?
            fake->double_indirect_block = (uint32_t)vnode; // VFS mode indicator
            return fake;
        }
        return 0;
    }
    // --- VFS Hook End ---

    if (start_id & 0x80000000) {
        uint32_t slot = (start_id >> 24) & 0x7F;
        vfs_mount_entry_t* m = vfs_get_mount(slot);
        if (m && m->sb.ops.lookup) {
            vfs_node_t s_vnode;
            s_vnode.inode_id = start_id & 0x00FFFFFF;
            s_vnode.sb = &m->sb;
            vfs_node_t* vnode = m->sb.ops.lookup(&s_vnode, path);
            if (vnode) {
                inode_t* fake = (inode_t*)kmalloc(sizeof(inode_t)); 
                memset(fake, 0, sizeof(inode_t));
                fake->id = 0x80000000 | ((uint32_t)m->sb.mount_index << 24) | (uint32_t)vnode->inode_id;
                fake->type = vnode->type;
                fake->mode = vnode->mode;
                fake->size = vnode->size;
                fake->uid = vnode->uid;
                fake->gid = vnode->gid;
                fake->atime = vnode->atime;
                fake->mtime = vnode->mtime;
                fake->ctime = vnode->ctime;
                fake->double_indirect_block = (uint32_t)vnode;
                return fake;
            }
            return 0;
        }
    }

    uint32_t current_id = start_id;

    char temp_path[128];
    strncpy(temp_path, path, 127);
    temp_path[127] = '\0';
    char* component      = temp_path;
    char* next_component = 0;

    while (*component != '\0') {
        int i = 0;
        while (component[i] != '/' && component[i] != '\0') i++;

        if (component[i] == '/') {
            component[i]  = '\0';
            next_component = component + i + 1;
        } else {
            next_component = 0;
        }

        if (strlen(component) > 0) {
            if (strcmp(component, "..") == 0) {
                fs_node_t* cur = fs_get_node(current_id);
                if (cur) current_id = cur->parent_id;
            } else if (strcmp(component, ".") == 0) {
                // stay put
            } else {
                fs_node_t* parent = fs_get_node(current_id);
                if (parent && !fs_check_permission(parent, current_task->uid, current_task->gid, 1)) {
                    return 0;
                }
                uint32_t next_id = fs_find_node_local_id(current_id, component);
                if (next_id == 0) return 0;
                current_id = next_id;
            }
        }

        if (!next_component) break;
        component = next_component;
    }

    extern vfs_mount_entry_t* vfs_get_mount_by_native_id(uint32_t native_id);
    vfs_mount_entry_t* hook = vfs_get_mount_by_native_id(current_id);
    if (hook && hook->sb.ops.lookup) {
        vfs_node_t* vnode = hook->sb.ops.lookup(&hook->sb.root_node, "/");
        if (vnode) {
            inode_t* fake = (inode_t*)kmalloc(sizeof(inode_t));
            memset(fake, 0, sizeof(inode_t));
            fake->id = 0x80000000 | ((uint32_t)hook->sb.mount_index << 24) | (uint32_t)vnode->inode_id;
            fake->type = vnode->type;
            fake->mode = vnode->mode;
            fake->size = vnode->size;
            fake->uid = vnode->uid;
            fake->gid = vnode->gid;
            fake->atime = vnode->atime;
            fake->mtime = vnode->mtime;
            fake->ctime = vnode->ctime;
            fake->double_indirect_block = (uint32_t)vnode;
            return fake;
        }
    }

    return fs_get_node(current_id);
}

int fs_create_node(uint32_t parent_id, char* name, uint8_t type, uint32_t uid, uint32_t gid) {
    if (sb.free_inodes == 0) {
        console_print_colored("FS: Disk full (inodes).\n", COLOR_LIGHT_RED);
        return 0;
    }

    inode_t* parent = cache_load(parent_id);
    if (!parent || parent->type != FS_TYPE_DIRECTORY) return 0;

    if (current_task && !fs_check_permission(parent, current_task->uid, current_task->gid, 3)) {
        console_print_colored("FS: create permission denied on parent directory.\n", COLOR_LIGHT_RED);
        return 0;
    }

    int b_idx = -1, e_idx = -1;
    fs_dirent_t entries[SECTOR_SIZE / sizeof(fs_dirent_t)];

    for (int i = 0; i < 12; i++) {
        uint32_t b_id = parent->blocks[i];
        if (b_id == 0) {
            b_id = block_alloc();
            if (b_id == 0) return 0;
            parent->blocks[i] = b_id;
            parent->block_count++;
            fs_update_node(parent);
        }
        ata_read_sectors(b_id, 1, entries);
        for (int j = 0; j < (int)(SECTOR_SIZE / sizeof(fs_dirent_t)); j++) {
            if (entries[j].inode_id == 0) {
                b_idx = i; e_idx = j;
                break;
            }
        }
        if (b_idx != -1) break;
    }

    if (b_idx == -1) {
        console_print_colored("FS: Directory full.\n", COLOR_LIGHT_RED);
        return 0;
    }

    int new_id = inode_alloc();
    if (new_id < 0) return 0;

    inode_t node;
    memset(&node, 0, sizeof(inode_t));
    node.id         = new_id;
    node.parent_id  = parent_id;
    node.type       = type;
    node.uid        = uid;
    node.gid        = gid;
    node.ctime      = node.mtime = node.atime = fs_timestamp();

    if (type == FS_TYPE_DIRECTORY) {
        node.mode       = S_IFDIR | 0755;
        node.link_count = 2;
    } else if (type == FS_TYPE_CHARDEV) {
        node.mode       = S_IFCHR | 0666;
        node.link_count = 1;
    } else if (type == FS_TYPE_BLOCKDEV) {
        // Block devices are owned by root, writable by root+disk group (gid=6).
        // indirect_block is set by the caller immediately after creation via
        // fs_update_node(), so we leave it 0 here.
        node.mode       = S_IFBLK | 0660;
        node.link_count = 1;
    } else {
        node.mode       = S_IFREG | 0644;
        node.link_count = 1;
    }

    // Inherit set-gid from parent if set
    if (parent->mode & 02000) {
        node.gid = parent->gid;
        if (type == FS_TYPE_DIRECTORY) node.mode |= 02000;
    }

    uint8_t buf[SECTOR_SIZE];
    ata_read_sectors(INODE_TO_SECTOR(new_id), 1, buf);
    memcpy(buf + INODE_OFF_IN_SECTOR(new_id), &node, sizeof(inode_t));
    ata_write_sectors(INODE_TO_SECTOR(new_id), 1, buf);

    if (type == FS_TYPE_DIRECTORY) {
        inode_t* new_dir = cache_load(new_id);
        if (new_dir) {
            dir_add_entry(new_dir, new_id,    ".");
            dir_add_entry(new_dir, parent_id, "..");
            save_node(new_id);
        }

        parent = cache_load(parent_id);
        if (parent) {
            parent->link_count++;
            parent->mtime = fs_timestamp();
            cache_mark_dirty(parent_id);
            save_node(parent_id);
        }
    }

    parent = cache_load(parent_id);
    if (!parent) return 0;

    ata_read_sectors(parent->blocks[b_idx], 1, entries);
    entries[e_idx].inode_id = new_id;
    strncpy(entries[e_idx].name, name, FS_MAX_NAME - 1);
    entries[e_idx].name[FS_MAX_NAME - 1] = '\0';
    ata_write_sectors(parent->blocks[b_idx], 1, entries);

    parent->size  += sizeof(fs_dirent_t);
    parent->mtime  = fs_timestamp();
    fs_update_node(parent);

    return 1;
}

int fs_delete_node(uint32_t id) {
    inode_t* node = cache_load(id);
    if (!node || id == FS_ROOT_ID) return 0;

    if (current_task) {
        inode_t* parent_check = cache_load(node->parent_id);
        if (parent_check && !fs_check_permission(parent_check, current_task->uid, current_task->gid, 3)) {
            console_print_colored("FS: delete permission denied on parent directory.\n", COLOR_LIGHT_RED);
            return 0;
        }
        if ((parent_check->mode & 01000) &&
            current_task->uid != 0 &&
            current_task->uid != node->uid &&
            current_task->uid != parent_check->uid) {
            console_print_colored("FS: delete denied (sticky bit).\n", COLOR_LIGHT_RED);
            return 0;
        }
    }

    if (node->type == FS_TYPE_DIRECTORY) {
        fs_dirent_t entries[SECTOR_SIZE / sizeof(fs_dirent_t)];
        for (int i = 0; i < 12; i++) {
            if (node->blocks[i] == 0) continue;
            ata_read_sectors(node->blocks[i], 1, entries);
            for (int j = 0; j < (int)(SECTOR_SIZE / sizeof(fs_dirent_t)); j++) {
                if (entries[j].inode_id == 0) continue;
                if (strcmp(entries[j].name, ".") == 0) continue;
                if (strcmp(entries[j].name, "..") == 0) continue;
                return 0;
            }
        }
    }

    inode_t* parent = cache_load(node->parent_id);
    if (parent) {
        fs_dirent_t entries[SECTOR_SIZE / sizeof(fs_dirent_t)];
        int found = 0;
        for (int i = 0; i < 12 && !found; i++) {
            if (parent->blocks[i] == 0) continue;
            ata_read_sectors(parent->blocks[i], 1, entries);
            for (int j = 0; j < (int)(SECTOR_SIZE / sizeof(fs_dirent_t)); j++) {
                if (entries[j].inode_id == id) {
                    entries[j].inode_id = 0;
                    memset(entries[j].name, 0, FS_MAX_NAME);
                    ata_write_sectors(parent->blocks[i], 1, entries);
                    found = 1;
                    break;
                }
            }
        }
        if (node->type == FS_TYPE_DIRECTORY && parent->link_count > 2) {
            parent->link_count--;
        }
        parent->mtime = fs_timestamp();
        fs_update_node(parent);
    }

    for (int i = 0; i < 12; i++) {
        if (node->blocks[i] != 0) {
            block_free(node->blocks[i]);
            node->blocks[i] = 0;
        }
    }

    // For CHARDEV and BLOCKDEV, indirect_block encodes major/minor — don't
    // treat it as a data block pointer.
    if (node->indirect_block != 0 &&
        node->type != FS_TYPE_CHARDEV &&
        node->type != FS_TYPE_BLOCKDEV) {
        uint32_t indices[PTRS_PER_BLOCK];
        ata_read_sectors(node->indirect_block, 1, indices);
        for (int i = 0; i < (int)PTRS_PER_BLOCK; i++) {
            if (indices[i] != 0) block_free(indices[i]);
        }
        block_free(node->indirect_block);
        node->indirect_block = 0;
    }

    if (node->double_indirect_block != 0) {
        uint32_t l1[PTRS_PER_BLOCK];
        ata_read_sectors(node->double_indirect_block, 1, l1);
        for (int i = 0; i < (int)PTRS_PER_BLOCK; i++) {
            if (l1[i] != 0) {
                uint32_t l2[PTRS_PER_BLOCK];
                ata_read_sectors(l1[i], 1, l2);
                for (int j = 0; j < (int)PTRS_PER_BLOCK; j++) {
                    if (l2[j] != 0) block_free(l2[j]);
                }
                block_free(l1[i]);
            }
        }
        block_free(node->double_indirect_block);
        node->double_indirect_block = 0;
    }

    bitmap_set(FS_INODE_BITMAP_SECTOR, id, 0);
    sb.free_inodes++;
    save_superblock();

    int idx = cache_find(id);
    if (idx >= 0) cache[idx].id = 0;

    return 1;
}

// ============================================================
// fs_sync
// ============================================================

void fs_sync() {
    for (int i = 0; i < FS_CACHE_SIZE; i++) {
        if (cache[i].id != 0 && cache[i].dirty) {
            save_node(cache[i].id);
        }
    }
    sb.fs_state = FS_STATE_CLEAN;
    save_superblock();
    console_print_colored("FS: Cache synced. Filesystem marked CLEAN.\n", COLOR_GREEN_ON_BLACK);
}

// ============================================================
// fs_read
// ============================================================

int fs_read(inode_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !buffer) return 0;

    if (node->id & 0x80000000) {
        vfs_node_t* vnode = (vfs_node_t*)node->double_indirect_block;
        if (vnode && vnode->sb && vnode->sb->ops.read) {
            return vnode->sb->ops.read(vnode, offset, size, buffer);
        }
        return 0;
    }

    if (current_task && !fs_check_permission(node, current_task->uid, current_task->gid, 4)) {
        console_print_colored("FS: read permission denied.\n", COLOR_LIGHT_RED);
        return -1;
    }

    if (offset >= node->size) return 0;
    if (offset + size > node->size) size = node->size - offset;

    uint32_t read_count = 0;
    uint8_t  sector_buf[SECTOR_SIZE];

    while (read_count < size) {
        uint32_t block_idx = (offset + read_count) / SECTOR_SIZE;
        uint32_t block_off = (offset + read_count) % SECTOR_SIZE;
        uint32_t to_read   = SECTOR_SIZE - block_off;
        if (to_read > (size - read_count)) to_read = size - read_count;

        uint32_t b_id = 0;

        if (block_idx < 12) {
            b_id = node->blocks[block_idx];

        } else if (block_idx < 12 + PTRS_PER_BLOCK) {
            if (node->indirect_block != 0) {
                uint32_t indices[PTRS_PER_BLOCK];
                ata_read_sectors(node->indirect_block, 1, indices);
                b_id = indices[block_idx - 12];
            }

        } else if (block_idx < 12 + PTRS_PER_BLOCK + PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
            if (node->double_indirect_block != 0) {
                uint32_t di_idx = block_idx - 12 - PTRS_PER_BLOCK;
                uint32_t l1_idx = di_idx / PTRS_PER_BLOCK;
                uint32_t l2_idx = di_idx % PTRS_PER_BLOCK;

                uint32_t l1[PTRS_PER_BLOCK];
                ata_read_sectors(node->double_indirect_block, 1, l1);
                if (l1[l1_idx] != 0) {
                    uint32_t l2[PTRS_PER_BLOCK];
                    ata_read_sectors(l1[l1_idx], 1, l2);
                    b_id = l2[l2_idx];
                }
            }
        } else {
            break;
        }

        if (b_id == 0) break;

        ata_read_sectors(b_id, 1, sector_buf);
        memcpy(buffer + read_count, sector_buf + block_off, to_read);
        read_count += to_read;
    }

    node->atime = fs_timestamp();
    cache_mark_dirty(node->id);

    return read_count;
}

// ============================================================
// fs_write
// ============================================================

int fs_write(inode_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !buffer) return 0;

    if (node->id & 0x80000000) {
        vfs_node_t* vnode = (vfs_node_t*)node->double_indirect_block;
        if (vnode && vnode->sb && vnode->sb->ops.write) {
            return vnode->sb->ops.write(vnode, offset, size, buffer);
        }
        return -1;
    }

    if (current_task && !fs_check_permission(node, current_task->uid, current_task->gid, 2)) {
        console_print_colored("FS: write permission denied.\n", COLOR_LIGHT_RED);
        return -1;
    }

    uint32_t write_count = 0;
    uint8_t  sector_buf[SECTOR_SIZE];

    while (write_count < size) {
        uint32_t block_idx = (offset + write_count) / SECTOR_SIZE;
        uint32_t block_off = (offset + write_count) % SECTOR_SIZE;
        uint32_t to_write  = SECTOR_SIZE - block_off;
        if (to_write > (size - write_count)) to_write = size - write_count;

        uint32_t b_id = 0;

        if (block_idx < 12) {
            if (node->blocks[block_idx] == 0) {
                b_id = block_alloc();
                if (b_id == 0) break;
                node->blocks[block_idx] = b_id;
                node->block_count++;
            } else {
                b_id = node->blocks[block_idx];
            }

        } else if (block_idx < 12 + PTRS_PER_BLOCK) {
            uint32_t si_idx = block_idx - 12;

            if (node->indirect_block == 0) {
                uint32_t ind_id = block_alloc();
                if (ind_id == 0) break;
                node->indirect_block = ind_id;
                uint8_t zero[SECTOR_SIZE];
                memset(zero, 0, SECTOR_SIZE);
                ata_write_sectors(ind_id, 1, zero);
            }

            uint32_t indices[PTRS_PER_BLOCK];
            ata_read_sectors(node->indirect_block, 1, indices);
            if (indices[si_idx] == 0) {
                b_id = block_alloc();
                if (b_id == 0) break;
                indices[si_idx] = b_id;
                node->block_count++;
                ata_write_sectors(node->indirect_block, 1, indices);
            } else {
                b_id = indices[si_idx];
            }

        } else if (block_idx < 12 + PTRS_PER_BLOCK + PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
            uint32_t di_idx = block_idx - 12 - PTRS_PER_BLOCK;
            uint32_t l1_idx = di_idx / PTRS_PER_BLOCK;
            uint32_t l2_idx = di_idx % PTRS_PER_BLOCK;
            uint8_t zero[SECTOR_SIZE];
            memset(zero, 0, SECTOR_SIZE);

            if (node->double_indirect_block == 0) {
                uint32_t dib_id = block_alloc();
                if (dib_id == 0) break;
                node->double_indirect_block = dib_id;
                ata_write_sectors(dib_id, 1, zero);
            }

            uint32_t l1[PTRS_PER_BLOCK];
            ata_read_sectors(node->double_indirect_block, 1, l1);

            if (l1[l1_idx] == 0) {
                uint32_t l1_blk = block_alloc();
                if (l1_blk == 0) break;
                l1[l1_idx] = l1_blk;
                ata_write_sectors(node->double_indirect_block, 1, l1);
                ata_write_sectors(l1_blk, 1, zero);
            }

            uint32_t l2[PTRS_PER_BLOCK];
            ata_read_sectors(l1[l1_idx], 1, l2);

            if (l2[l2_idx] == 0) {
                b_id = block_alloc();
                if (b_id == 0) break;
                l2[l2_idx] = b_id;
                node->block_count++;
                ata_write_sectors(l1[l1_idx], 1, l2);
            } else {
                b_id = l2[l2_idx];
            }

        } else {
            console_print_colored("FS: File too large (exceeded double-indirect limit ~8MB).\n",
                                  COLOR_LIGHT_RED);
            break;
        }

        if (b_id == 0) break;

        if (to_write < SECTOR_SIZE) {
            ata_read_sectors(b_id, 1, sector_buf);
        }
        memcpy(sector_buf + block_off, buffer + write_count, to_write);
        ata_write_sectors(b_id, 1, sector_buf);

        write_count += to_write;
    }

    if (offset + write_count > node->size) {
        node->size = offset + write_count;
    }

    uint32_t now = fs_timestamp();
    node->mtime  = now;
    node->ctime  = now;

    fs_update_node(node);
    return write_count;
}

// ============================================================
// Stats
// ============================================================

void fs_get_disk_stats(uint32_t* total_kb, uint32_t* used_kb, uint32_t* free_kb) {
    *total_kb = (sb.total_blocks * SECTOR_SIZE) / 1024;
    *used_kb  = ((sb.total_blocks - sb.free_blocks) * SECTOR_SIZE) / 1024;
    *free_kb  = (sb.free_blocks * SECTOR_SIZE) / 1024;
}

void fs_get_cache_stats(uint32_t* cache_size, uint32_t* cached_nodes, uint32_t* dirty_nodes) {
    *cache_size    = FS_CACHE_SIZE;
    *cached_nodes  = 0;
    *dirty_nodes   = 0;
    for (int i = 0; i < FS_CACHE_SIZE; i++) {
        if (cache[i].id != 0) {
            (*cached_nodes)++;
            if (cache[i].dirty) (*dirty_nodes)++;
        }
    }
}

// ============================================================
// Path helpers
// ============================================================

int fs_get_inode_name(uint32_t id, char* buffer) {
    if (id == FS_ROOT_ID) { strcpy(buffer, "/"); return 1; }

    inode_t* node = cache_load(id);
    if (!node) return 0;

    inode_t* parent = cache_load(node->parent_id);
    if (!parent || parent->type != FS_TYPE_DIRECTORY) return 0;

    fs_dirent_t entries[SECTOR_SIZE / sizeof(fs_dirent_t)];
    for (int i = 0; i < 12; i++) {
        uint32_t b_id = parent->blocks[i];
        if (b_id == 0) continue;
        ata_read_sectors(b_id, 1, entries);
        for (int j = 0; j < (int)(SECTOR_SIZE / sizeof(fs_dirent_t)); j++) {
            if (entries[j].inode_id == id) {
                strncpy(buffer, entries[j].name, FS_MAX_NAME);
                return 1;
            }
        }
    }
    return 0;
}

void fs_get_full_path(uint32_t id, char* buffer) {
    if (id == FS_ROOT_ID) { strcpy(buffer, "/"); return; }
    
    if (id & 0x80000000) {
        uint32_t slot = (id >> 24) & 0x7F;
        extern vfs_mount_entry_t* vfs_get_mount(int slot);
        vfs_mount_entry_t* m = vfs_get_mount(slot);
        if (m) {
            strcpy(buffer, m->mountpoint);
            // Optionally we should trace relative inside the VFS, but for now mountpoint is better than '/'
            return;
        }
    }

    char     components[10][FS_MAX_NAME];
    int      count      = 0;
    uint32_t current_id = id;

    while (current_id != FS_ROOT_ID && count < 10) {
        char name[FS_MAX_NAME];
        if (fs_get_inode_name(current_id, name)) {
            strcpy(components[count++], name);
            inode_t* node = cache_load(current_id);
            if (node) current_id = node->parent_id;
            else break;
        } else break;
    }

    buffer[0] = '\0';
    if (count == 0) { strcpy(buffer, "/"); return; }
    for (int i = count - 1; i >= 0; i--) {
        strcat(buffer, "/");
        strcat(buffer, components[i]);
    }
}

// ============================================================
// fs_fsck
// ============================================================

int fs_fsck(int repair) {
    console_print_colored("FS: fsck starting...\n", COLOR_YELLOW_ON_BLACK);

    int errors = 0;

    uint8_t ref_inode_bitmap[SECTOR_SIZE];
    uint8_t ref_block_bitmap[FS_BLOCK_BITMAP_COUNT * SECTOR_SIZE];
    memset(ref_inode_bitmap, 0, sizeof(ref_inode_bitmap));
    memset(ref_block_bitmap, 0, sizeof(ref_block_bitmap));

    #define REF_INODE_SET(id)   (ref_inode_bitmap[(id)/8] |= (1 << ((id)%8)))
    #define REF_BLOCK_SET(blk)  do { \
        uint32_t _b = (blk); \
        if (_b < (uint32_t)(FS_BLOCK_BITMAP_COUNT * SECTOR_SIZE * 8)) \
            ref_block_bitmap[_b/8] |= (1 << (_b%8)); \
    } while(0)

    for (uint32_t b = 0; b < FS_DATA_BLOCKS_START; b++) REF_BLOCK_SET(b);

    for (uint32_t id = 1; id < FS_MAX_INODES; id++) {
        uint8_t sector_buf[SECTOR_SIZE];
        ata_read_sectors(INODE_TO_SECTOR(id), 1, sector_buf);
        inode_t* node = (inode_t*)(sector_buf + INODE_OFF_IN_SECTOR(id));

        if (node->id != id) continue;

        REF_INODE_SET(id);

        for (int i = 0; i < 12; i++) {
            if (node->blocks[i] != 0) REF_BLOCK_SET(node->blocks[i]);
        }

        // For regular files only: account for indirect block pointers.
        // CHARDEV and BLOCKDEV reuse indirect_block for major/minor encoding.
        if (node->indirect_block != 0 &&
            node->type != FS_TYPE_CHARDEV &&
            node->type != FS_TYPE_BLOCKDEV) {
            REF_BLOCK_SET(node->indirect_block);
            uint32_t indices[PTRS_PER_BLOCK];
            ata_read_sectors(node->indirect_block, 1, indices);
            for (int i = 0; i < (int)PTRS_PER_BLOCK; i++) {
                if (indices[i] != 0) REF_BLOCK_SET(indices[i]);
            }
        }

        if (node->double_indirect_block != 0) {
            REF_BLOCK_SET(node->double_indirect_block);
            uint32_t l1[PTRS_PER_BLOCK];
            ata_read_sectors(node->double_indirect_block, 1, l1);
            for (int i = 0; i < (int)PTRS_PER_BLOCK; i++) {
                if (l1[i] != 0) {
                    REF_BLOCK_SET(l1[i]);
                    uint32_t l2[PTRS_PER_BLOCK];
                    ata_read_sectors(l1[i], 1, l2);
                    for (int j = 0; j < (int)PTRS_PER_BLOCK; j++) {
                        if (l2[j] != 0) REF_BLOCK_SET(l2[j]);
                    }
                }
            }
        }
    }

    uint8_t disk_inode_bm[SECTOR_SIZE];
    ata_read_sectors(FS_INODE_BITMAP_SECTOR, 1, disk_inode_bm);

    for (uint32_t id = 1; id < FS_MAX_INODES; id++) {
        int on_disk = (disk_inode_bm[id/8] >> (id%8)) & 1;
        int ref      = (ref_inode_bitmap[id/8] >> (id%8)) & 1;

        if (on_disk && !ref) {
            console_print_colored("FS: fsck: leaked inode detected\n", COLOR_LIGHT_RED);
            errors++;
            if (repair) {
                bitmap_set(FS_INODE_BITMAP_SECTOR, id, 0);
                sb.free_inodes++;
            }
        } else if (!on_disk && ref) {
            console_print_colored("FS: fsck: phantom inode (in table, not in bitmap)\n",
                                  COLOR_LIGHT_RED);
            errors++;
            if (repair) {
                bitmap_set(FS_INODE_BITMAP_SECTOR, id, 1);
                sb.free_inodes--;
            }
        }
    }

    for (int s = 0; s < FS_BLOCK_BITMAP_COUNT; s++) {
        uint8_t disk_blk_bm[SECTOR_SIZE];
        ata_read_sectors(FS_BLOCK_BITMAP_START + s, 1, disk_blk_bm);

        for (int bit = 0; bit < SECTOR_SIZE * 8; bit++) {
            uint32_t global_bit = s * SECTOR_SIZE * 8 + bit;
            if (global_bit >= sb.total_blocks) break;

            int on_disk = (disk_blk_bm[bit/8] >> (bit%8)) & 1;
            int ref      = 0;
            if (global_bit < (uint32_t)(FS_BLOCK_BITMAP_COUNT * SECTOR_SIZE * 8)) {
                ref = (ref_block_bitmap[global_bit/8] >> (global_bit%8)) & 1;
            }

            if (on_disk && !ref) {
                errors++;
                if (repair && global_bit >= FS_DATA_BLOCKS_START) {
                    bitmap_set(FS_BLOCK_BITMAP_START, global_bit, 0);
                    sb.free_blocks++;
                }
            } else if (!on_disk && ref) {
                errors++;
                if (repair) {
                    bitmap_set(FS_BLOCK_BITMAP_START, global_bit, 1);
                    sb.free_blocks--;
                }
            }
        }
    }

    #undef REF_INODE_SET
    #undef REF_BLOCK_SET

    if (repair && errors > 0) {
        save_superblock();
    }

    if (errors == 0) {
        console_print_colored("FS: fsck complete — no errors found.\n", COLOR_GREEN_ON_BLACK);
    } else {
        if (repair) {
            console_print_colored("FS: fsck complete — errors repaired.\n", COLOR_YELLOW_ON_BLACK);
        } else {
            console_print_colored("FS: fsck complete — errors found (run with repair=1 to fix).\n",
                                  COLOR_LIGHT_RED);
        }
    }

    return errors;
}
