/**
 * src/punixfs_vfs.c - VFS Driver for the native PUNIX filesystem
 *
 * This driver allows the VFS layer to mount a PUNIX-FS image (e.g. extracted
 * from disk.img starting at FS_SUPERBLOCK_SECTOR) on a secondary drive like
 * /dev/hdb, and expose it at a mount point like /mnt/fs1.
 *
 * The PUNIX-FS disk layout (relative to the start of the image / drive):
 *   Sector 0   : FS_SUPERBLOCK_SECTOR  (magic, inode count, etc.)
 *   Sector 1   : FS_INODE_BITMAP_SECTOR
 *   Sector 2   : FS_BLOCK_BITMAP_START  (25 sectors)
 *   Sector 27  : FS_INODE_TABLE_START   (256 sectors)
 *   Sector 283 : FS_DATA_BLOCKS_START
 *
 * When the image is extracted from disk.img at skip=256 (FS_SUPERBLOCK_SECTOR),
 * all of these offsets become relative to sector 0 of the new image, so we
 * can read them using ata_read_sectors_ex(drive, sector, ...) directly.
 */

#include "../include/punixfs_vfs.h"
#include "../include/vfs.h"
#include "../include/ata.h"
#include "../include/string.h"
#include "../include/memory.h"
#include "../include/console.h"
#include "../include/fs.h"
#include "../include/syscall.h"

// ── Disk layout constants (mirrored from fs.h, relative to image start) ──
#define PFS_MAGIC               0xEF5342
#define PFS_SUPERBLOCK_SECTOR   0
#define PFS_INODE_BITMAP_SECTOR 1
#define PFS_BLOCK_BITMAP_START  2
#define PFS_BLOCK_BITMAP_COUNT  25
#define PFS_INODE_TABLE_START   27
#define PFS_INODE_TABLE_COUNT   256
#define PFS_DATA_BLOCKS_START   283
#define PFS_ROOT_INODE_ID       1
#define PFS_SECTOR_SIZE         512
#define PFS_MAX_NAME            60

// The on-disk superblock (must match the kernel's superblock_t layout)
typedef struct {
    uint32_t magic;
    uint32_t root_inode;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t block_size;
    uint8_t  fs_state;
    uint8_t  _pad[3];
} pfs_superblock_t;

// Private state stored in vfs_superblock_t.fs_info
typedef struct {
    pfs_superblock_t sb;
    uint8_t          drive;
} pfs_info_t;

// ── Helpers ──────────────────────────────────────────────────────────────

#define PFS_INODE_TO_SECTOR(id)      (PFS_INODE_TABLE_START + ((id) / (PFS_SECTOR_SIZE / sizeof(inode_t))))
#define PFS_INODE_OFF_IN_SECTOR(id)  (((id) % (PFS_SECTOR_SIZE / sizeof(inode_t))) * sizeof(inode_t))

static inode_t pfs_get_inode(uint8_t drive, uint32_t id) {
    inode_t result;
    memset(&result, 0, sizeof(inode_t));
    if (id == 0) return result;
    uint8_t buf[PFS_SECTOR_SIZE];
    ata_read_sectors_ex(drive, PFS_INODE_TO_SECTOR(id), 1, buf);
    memcpy(&result, buf + PFS_INODE_OFF_IN_SECTOR(id), sizeof(inode_t));
    return result;
}

// Convert a native inode_t to a heap-allocated vfs_node_t
static vfs_node_t* pfs_inode_to_vnode(struct vfs_superblock* sb, uint32_t id, inode_t* inode) {
    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) return 0;
    memset(node, 0, sizeof(vfs_node_t));
    node->inode_id = id;
    node->type     = inode->type;   // FS_TYPE_FILE / FS_TYPE_DIRECTORY
    node->mode     = inode->mode;
    node->size     = inode->size;
    node->uid      = inode->uid;
    node->gid      = inode->gid;
    node->atime    = inode->atime;
    node->mtime    = inode->mtime;
    node->ctime    = inode->ctime;
    node->sb       = sb;
    return node;
}

// Look up a single name component inside a directory inode, returning
// the inode ID of the child (or 0 if not found).
static uint32_t pfs_lookup_in_dir(uint8_t drive, inode_t* dir, const char* name) {
    fs_dirent_t entries[PFS_SECTOR_SIZE / sizeof(fs_dirent_t)];
    for (int b = 0; b < 12; b++) {
        uint32_t block = dir->blocks[b];
        if (block == 0) continue;
        ata_read_sectors_ex(drive, block, 1, entries);
        int count = PFS_SECTOR_SIZE / sizeof(fs_dirent_t);
        for (int i = 0; i < count; i++) {
            if (entries[i].inode_id == 0) continue;
            if (strcmp(entries[i].name, name) == 0)
                return entries[i].inode_id;
        }
    }
    return 0;
}

// ── VFS Operations ───────────────────────────────────────────────────────

static vfs_node_t* pfs_lookup(vfs_node_t* start_node, const char* path) {
    if (!start_node || !path) return 0;
    struct vfs_superblock* sb = start_node->sb;
    pfs_info_t* info = (pfs_info_t*)sb->fs_info;
    uint8_t drive = info->drive;

    uint32_t current_id = start_node->inode_id;
    inode_t  current    = pfs_get_inode(drive, current_id);

    // Root of the mounted FS
    if (path[0] == '\0' || strcmp(path, "/") == 0)
        return pfs_inode_to_vnode(sb, current_id, &current);

    char temp[256];
    strncpy(temp, path, 255);
    temp[255] = '\0';
    char* comp = temp;
    if (comp[0] == '/') comp++; // skip leading slash

    while (*comp != '\0') {
        // Isolate next component
        char* slash = comp;
        while (*slash != '/' && *slash != '\0') slash++;
        int had_slash = (*slash == '/');
        *slash = '\0';
        char* next = had_slash ? (slash + 1) : 0;

        if (strcmp(comp, ".") == 0) {
            // stay
        } else if (strcmp(comp, "..") == 0) {
            current_id = current.parent_id ? current.parent_id : current_id;
            current = pfs_get_inode(drive, current_id);
        } else if (strlen(comp) > 0) {
            uint32_t child_id = pfs_lookup_in_dir(drive, &current, comp);
            if (child_id == 0) return 0; // not found
            current_id = child_id;
            current = pfs_get_inode(drive, current_id);
        }

        if (!next) break;
        comp = next;
    }

    return pfs_inode_to_vnode(sb, current_id, &current);
}

static int pfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !buffer) return 0;
    if (node->size == 0) return 0;
    if (offset >= node->size) return 0;
    if (offset + size > node->size) size = node->size - offset;

    struct vfs_superblock* sb = node->sb;
    pfs_info_t* info = (pfs_info_t*)sb->fs_info;
    uint8_t drive = info->drive;
    inode_t inode = pfs_get_inode(drive, (uint32_t)node->inode_id);

    uint32_t read_bytes = 0;
    uint8_t sector_buf[PFS_SECTOR_SIZE];

    while (read_bytes < size) {
        uint32_t cur_offset   = offset + read_bytes;
        uint32_t block_index  = cur_offset / PFS_SECTOR_SIZE;
        uint32_t off_in_block = cur_offset % PFS_SECTOR_SIZE;
        uint32_t to_read      = PFS_SECTOR_SIZE - off_in_block;
        if (to_read > size - read_bytes) to_read = size - read_bytes;

        uint32_t block = 0;
        if (block_index < 12) {
            block = inode.blocks[block_index];
        } else if (block_index < 12 + 128) {
            // Single indirect
            if (inode.indirect_block) {
                uint32_t indices[PFS_SECTOR_SIZE / sizeof(uint32_t)];
                ata_read_sectors_ex(drive, inode.indirect_block, 1, indices);
                block = indices[block_index - 12];
            }
        } else {
            // Double indirect – skip for now
            console_print_colored("punixfs_vfs: double-indirect not yet supported\n", COLOR_LIGHT_RED);
            break;
        }

        if (block == 0) {
            memset(buffer + read_bytes, 0, to_read);
        } else {
            ata_read_sectors_ex(drive, block, 1, sector_buf);
            memcpy(buffer + read_bytes, sector_buf + off_in_block, to_read);
        }
        read_bytes += to_read;
    }
    return (int)read_bytes;
}

static int pfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    // Read-only for now — we don't want to write back across the VFS boundary
    (void)node; (void)offset; (void)size; (void)buffer;
    console_print_colored("punixfs_vfs: write not implemented\n", COLOR_LIGHT_RED);
    return -1;
}

static int pfs_create(struct vfs_superblock* sb, const char* path, uint8_t type, uint32_t uid, uint32_t gid) {
    (void)sb; (void)path; (void)type; (void)uid; (void)gid;
    console_print_colored("punixfs_vfs: create not implemented\n", COLOR_LIGHT_RED);
    return -1;
}

static int pfs_getdents(vfs_node_t* dir_node, struct dirent* dirents, int max) {
    if (!dir_node || !dirents) return -1;
    struct vfs_superblock* sb = dir_node->sb;
    pfs_info_t* info = (pfs_info_t*)sb->fs_info;
    uint8_t drive = info->drive;
    inode_t dir = pfs_get_inode(drive, (uint32_t)dir_node->inode_id);

    if (dir.type != FS_TYPE_DIRECTORY) return -1;

    int cnt = 0;
    fs_dirent_t entries[PFS_SECTOR_SIZE / sizeof(fs_dirent_t)];
    for (int b = 0; b < 12 && cnt < max; b++) {
        uint32_t block = dir.blocks[b];
        if (block == 0) continue;
        ata_read_sectors_ex(drive, block, 1, entries);
        int per_block = PFS_SECTOR_SIZE / sizeof(fs_dirent_t);
        for (int i = 0; i < per_block && cnt < max; i++) {
            if (entries[i].inode_id == 0) continue;
            dirents[cnt].d_ino = entries[i].inode_id;
            // Determine type from the inode
            inode_t child_inode = pfs_get_inode(drive, entries[i].inode_id);
            dirents[cnt].d_type = (child_inode.type == FS_TYPE_DIRECTORY) ? 4 : 8; // DT_DIR=4, DT_REG=8
            strncpy(dirents[cnt].d_name, entries[i].name, PFS_MAX_NAME - 1);
            dirents[cnt].d_name[PFS_MAX_NAME - 1] = '\0';
            cnt++;
        }
    }
    return cnt;
}

// ── Mount entry point ─────────────────────────────────────────────────────

int punixfs_init_mount(uint8_t drive, vfs_superblock_t* sb) {
    // Read the superblock (sector 0 of the extracted image)
    pfs_superblock_t disk_sb;
    ata_read_sectors_ex(drive, PFS_SUPERBLOCK_SECTOR, 1, &disk_sb);

    if (disk_sb.magic != PFS_MAGIC) {
        console_print_colored("punixfs_vfs: Invalid magic — not a PUNIX-FS image\n", COLOR_LIGHT_RED);
        return -1;
    }
    console_print_colored("punixfs_vfs: Detected PUNIX-FS image\n", COLOR_LIGHT_GREEN);

    pfs_info_t* info = (pfs_info_t*)kmalloc(sizeof(pfs_info_t));
    if (!info) return -1;
    info->drive = drive;
    memcpy(&info->sb, &disk_sb, sizeof(pfs_superblock_t));

    sb->drive_index = drive;
    sb->fs_info     = info;
    sb->ops.read    = pfs_read;
    sb->ops.write   = pfs_write;
    sb->ops.lookup  = pfs_lookup;
    sb->ops.create  = pfs_create;
    sb->ops.getdents= pfs_getdents;

    // Load root node
    inode_t root_inode = pfs_get_inode(drive, PFS_ROOT_INODE_ID);
    sb->root_node.inode_id = PFS_ROOT_INODE_ID;
    sb->root_node.type     = FS_TYPE_DIRECTORY;
    sb->root_node.mode     = root_inode.mode;
    sb->root_node.sb       = sb;

    return 0;
}
