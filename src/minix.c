#include "../include/minix.h"
#include "../include/ata.h"
#include "../include/string.h"
#include "../include/memory.h"
#include "../include/console.h"
#include "../include/syscall.h"

// Helper macros for minix
#define BLOCK_SIZE 1024
#define SECTORS_PER_BLOCK (BLOCK_SIZE / ATA_SECTOR_SIZE)
#define MINIX_INODES_PER_BLOCK (BLOCK_SIZE / sizeof(minix_inode_t))

// Private struct to store in vfs_superblock_t.fs_info
typedef struct {
    minix_super_block_t sb;
    uint32_t first_data_sector;
    uint32_t inode_table_sector;
    uint32_t dir_entry_size;  // 16 or 32
} minix_info_t;

// Forward declares
static int minix_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
static int minix_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
static vfs_node_t* minix_lookup(vfs_node_t* start_node, const char* path);
static int minix_create(struct vfs_superblock* sb, const char* path, uint8_t type, uint32_t uid, uint32_t gid);
static int minix_getdents(vfs_node_t* dir_node, struct dirent* dirents, int max);

// Read a 1024-byte block from Minix filesystem
static int minix_read_block(uint8_t drive, uint16_t block, uint8_t* buf) {
    if (ata_read_sectors_ex(drive, block * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK, buf) != 0) {
        return -1;
    }
    return 0;
}

static minix_inode_t get_minix_inode(uint8_t drive, minix_info_t* info, uint16_t inode_id) {
    minix_inode_t inode;
    memset(&inode, 0, sizeof(minix_inode_t));
    if (inode_id == 0) return inode; // Invalid
    
    // Inodes start at 1
    uint32_t block = 2 + info->sb.s_imap_blocks + info->sb.s_zmap_blocks;
    uint32_t offset_in_table = inode_id - 1;
    uint32_t block_offset = offset_in_table / MINIX_INODES_PER_BLOCK;
    uint32_t index_in_block = offset_in_table % MINIX_INODES_PER_BLOCK;
    
    uint8_t buf[BLOCK_SIZE];
    minix_read_block(drive, block + block_offset, buf);
    memcpy(&inode, buf + (index_in_block * sizeof(minix_inode_t)), sizeof(minix_inode_t));
    return inode;
}

// Map a minix inode to our generic vfs_node_t struct layout (needs malloc for persistence)
static vfs_node_t* copy_inode_to_vfs(struct vfs_superblock* sb, uint16_t inode_id, minix_inode_t* mi) {
    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t)); // Using heap!
    if (!node) return 0;
    node->inode_id = (uint32_t)inode_id;
    // Map mode
    if ((mi->i_mode & 0170000) == 0040000) node->type = FS_TYPE_DIRECTORY;
    else node->type = FS_TYPE_FILE;
    node->mode = mi->i_mode;
    node->size = mi->i_size;
    node->uid = mi->i_uid;
    node->gid = mi->i_gid;
    node->mtime = mi->i_time;
    node->atime = mi->i_time;
    node->ctime = mi->i_time;
    node->sb = sb;
    return node;
}

// Map logical block index to physical block number (supports direct, indirect, double indirect)
static uint16_t minix_bmap(uint8_t drive, minix_inode_t* mi, uint32_t block_index) {
    if (block_index < 7) {
        return mi->i_zone[block_index];
    }
    block_index -= 7;
    
    // Single indirect
    if (block_index < 512) {
        uint16_t indirect = mi->i_zone[7];
        if (indirect == 0) return 0;
        
        uint16_t buf[BLOCK_SIZE / 2];
        if (minix_read_block(drive, indirect, (uint8_t*)buf) != 0) return 0;
        return buf[block_index];
    }
    block_index -= 512;
    
    // Double indirect
    if (block_index < 512 * 512) {
        uint16_t double_indirect = mi->i_zone[8];
        if (double_indirect == 0) return 0;
        
        uint16_t buf1[BLOCK_SIZE / 2];
        if (minix_read_block(drive, double_indirect, (uint8_t*)buf1) != 0) return 0;
        
        uint16_t indirect = buf1[block_index / 512];
        if (indirect == 0) return 0;
        
        uint16_t buf2[BLOCK_SIZE / 2];
        if (minix_read_block(drive, indirect, (uint8_t*)buf2) != 0) return 0;
        return buf2[block_index % 512];
    }
    
    return 0;
}

// Basic lookup inside a directory inode
static uint16_t minix_lookup_in_dir(uint8_t drive, minix_info_t* info, minix_inode_t* dir, const char* name) {
    uint32_t num_blocks = (dir->i_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    
    for (uint32_t b = 0; b < num_blocks; b++) {
        uint16_t block = minix_bmap(drive, dir, b);
        if (block == 0) continue;
        uint8_t buf[BLOCK_SIZE];
        if (minix_read_block(drive, block, buf) != 0) return 0;
        
        int entries_per_block = BLOCK_SIZE / info->dir_entry_size;
        for (int i = 0; i < entries_per_block; i++) {
            minix_dir_entry_t* en = (minix_dir_entry_t*)(buf + (i * info->dir_entry_size));
            if (en->inode != 0) {
                // Ensure null termination for cmp
                char ename[32]; // Max 30 for V1-30C
                strncpy(ename, en->name, info->dir_entry_size - 2);
                ename[info->dir_entry_size - 2] = '\0';
                if (strcmp(ename, name) == 0) {
                    return en->inode;
                }
            }
        }
    }
    return 0;
}

static vfs_node_t* minix_lookup(vfs_node_t* start_node, const char* path) {
    if (!start_node || !path) return 0;
    struct vfs_superblock* sb = start_node->sb;
    uint8_t drive = sb->drive_index;
    minix_info_t* info = (minix_info_t*)sb->fs_info;
    
    // Start at given node
    uint16_t current_id = (uint16_t)start_node->inode_id;
    minix_inode_t current = get_minix_inode(drive, info, current_id);

   // console_print_colored("minix: lookup called for path: ", COLOR_LIGHT_CYAN);
  //  console_print_colored(path, COLOR_LIGHT_CYAN);
   // console_print("\n");
    
    if (strcmp(path, "/") == 0 || path[0] == '\0') {
        return copy_inode_to_vfs(sb, current_id, &current);
    }
    
    char temp_path[MAX_PATH];
    strncpy(temp_path, path, MAX_PATH - 1);
    temp_path[MAX_PATH - 1] = '\0';
    char* comp = temp_path;
    if (comp[0] == '/') comp++; // Skip leading slash

    char* next = 0;
    while (*comp != '\0') {
        int i = 0;
        while (comp[i] != '/' && comp[i] != '\0') i++;
        if (comp[i] == '/') {
            comp[i] = '\0';
            next = comp + i + 1;
        } else {
            next = 0;
        }
        
        if (strcmp(comp, ".") == 0) {
            // Stay in current directory
        } else if (strlen(comp) > 0) {
            uint16_t next_id = minix_lookup_in_dir(drive, info, &current, comp);
            if (next_id == 0) return 0; // Not found
            current_id = next_id;
            current = get_minix_inode(drive, info, current_id);
        }
        if (!next) break;
        comp = next;
    }
    
    return copy_inode_to_vfs(sb, current_id, &current);
}

static int minix_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !buffer) return 0;
    if (offset >= node->size) return 0;
    if (offset + size > node->size) size = node->size - offset;
    
    uint8_t drive = node->sb->drive_index;
    minix_info_t* info = (minix_info_t*)node->sb->fs_info;
    
    minix_inode_t mi = get_minix_inode(drive, info, (uint16_t)node->inode_id);
    uint32_t read_bytes = 0;
    
    while (read_bytes < size) {
        uint32_t current_offset = offset + read_bytes;
        uint32_t block_index = current_offset / BLOCK_SIZE;
        uint32_t offset_in_block = current_offset % BLOCK_SIZE;
        uint32_t to_read = BLOCK_SIZE - offset_in_block;
        if (to_read > size - read_bytes) to_read = size - read_bytes;
        
        uint16_t physical_block = minix_bmap(drive, &mi, block_index);
        
        if (physical_block == 0) {
            // Sparse block, just read zeros
            memset(buffer + read_bytes, 0, to_read);
        } else {
            uint8_t bbuf[BLOCK_SIZE];
            minix_read_block(drive, physical_block, bbuf);
            memcpy(buffer + read_bytes, bbuf + offset_in_block, to_read);
        }
        
        read_bytes += to_read;
    }
    
    return read_bytes;
}

static int minix_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    // Readonly for now till we implement full block allocation!
    console_print_colored("minix: write not implemented yet\n", COLOR_LIGHT_RED);
    return -1;
}

static int minix_create(struct vfs_superblock* sb, const char* path, uint8_t type, uint32_t uid, uint32_t gid) {
    console_print_colored("minix: create not implemented yet\n", COLOR_LIGHT_RED);
    return -1;
}

static int minix_getdents(vfs_node_t* dir_node, struct dirent* dirents, int max) {
    if (!dir_node || !dirents) return -1;
    uint8_t drive = dir_node->sb->drive_index;
    minix_info_t* info = (minix_info_t*)dir_node->sb->fs_info;
    
    minix_inode_t dir = get_minix_inode(drive, info, (uint16_t)dir_node->inode_id);
    if ((dir.i_mode & 0170000) != 0040000) return -1;

   // console_print_colored("minix: getdents for inode: ", COLOR_LIGHT_GREEN);
    char buf_id[16]; int_to_str(dir_node->inode_id, buf_id);
    // console_print_colored(buf_id, COLOR_LIGHT_GREEN);
 //   console_print(" (zones: ");
    for(int j=0; j<7; j++) { char bz[16]; int_to_str(dir.i_zone[j], bz);
        // console_print(bz);
    // console_print(" ");

    }
  //  console_print(")\n");
    
    // DEBUG: trace directory listing
    // console_printf("minix: getdents for inode %d, size %d\n", dir_node->inode_id, dir.i_size);

    uint32_t num_blocks = (dir.i_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    int cnt = 0;
    for (uint32_t b = 0; b < num_blocks && cnt < max; b++) {
        uint16_t block = minix_bmap(drive, &dir, b);
        if (block == 0) continue;
        uint8_t buf[BLOCK_SIZE];
        if (minix_read_block(drive, block, buf) != 0) {
            break;
        }
        
        int entries_per_block = BLOCK_SIZE / info->dir_entry_size;
        for (int i = 0; i < entries_per_block && cnt < max; i++) {
            minix_dir_entry_t* en = (minix_dir_entry_t*)(buf + (i * info->dir_entry_size));
            if (en->inode != 0) {
                dirents[cnt].d_ino = en->inode;
                // Minix doesn't store type in dirent natively, we can leave it 0 or load inode
                dirents[cnt].d_type = 0; 
                strncpy(dirents[cnt].d_name, en->name, MIN(60, info->dir_entry_size - 2));
                dirents[cnt].d_name[MIN(60, info->dir_entry_size - 2)] = '\0';
                cnt++;
            }
        }
    }
    return cnt;
}

int minix_init_mount(uint8_t drive, vfs_superblock_t* sb) {
    // Block 1 is the superblock
    uint8_t buf[BLOCK_SIZE];
    if (minix_read_block(drive, 1, buf) != 0) {
        return -1;
    }
    
    minix_super_block_t* disk_sb = (minix_super_block_t*)buf;
    
    // Debug print
    console_print_colored("minix: Read magic ", COLOR_LIGHT_BLUE);
    char hexbuf[16];
    int idx = 0;
    uint32_t magic = disk_sb->s_magic;
    do {
        int rem = magic % 16;
        hexbuf[idx++] = (rem < 10) ? ('0' + rem) : ('a' + rem - 10);
        magic /= 16;
    } while (magic > 0);
    hexbuf[idx] = '\0';
    for (int i=0; i<idx/2; i++) { char t=hexbuf[i]; hexbuf[i]=hexbuf[idx-1-i]; hexbuf[idx-1-i]=t; }
    console_print_colored(hexbuf, COLOR_LIGHT_BLUE);
    console_print_colored("\n", COLOR_LIGHT_BLUE);
    
    if (disk_sb->s_magic != MINIX_V1_MAGIC && disk_sb->s_magic != MINIX_V1_MAGIC_30C) {
        console_print_colored("minix: Invalid magic number.\n", COLOR_LIGHT_RED);
        return -1; // Not a valid minix v1 fs
    }

    
    minix_info_t* info = (minix_info_t*)kmalloc(sizeof(minix_info_t));
    if (!info) return -1;
    memcpy(&info->sb, disk_sb, sizeof(minix_super_block_t));

    // DEBUG: print more details
    console_print("  imap_blocks: ");
    char b1[16]; int_to_str(info->sb.s_imap_blocks, b1); console_print(b1);
    console_print("\n  zmap_blocks: ");
    char b2[16]; int_to_str(info->sb.s_zmap_blocks, b2); console_print(b2);
    console_print("\n  firstdatazone: ");
    char b3[16]; int_to_str(info->sb.s_firstdatazone, b3); console_print(b3);
    console_print("\n  ninodes: ");
    char b4[16]; int_to_str(info->sb.s_ninodes, b4); console_print(b4);
    console_print("\n  nzones: ");
    char b5[16]; int_to_str(info->sb.s_nzones, b5); console_print(b5);
    console_print("\n");
    
    info->inode_table_sector = (2 + info->sb.s_imap_blocks + info->sb.s_zmap_blocks) * SECTORS_PER_BLOCK;
    info->first_data_sector = info->sb.s_firstdatazone * SECTORS_PER_BLOCK;
    info->dir_entry_size = (disk_sb->s_magic == MINIX_V1_MAGIC_30C) ? 32 : 16;
    
    sb->drive_index = drive;
    sb->fs_info = info;
    sb->ops.read = minix_read;
    sb->ops.write = minix_write;
    sb->ops.lookup = minix_lookup;
    sb->ops.create = minix_create;
    sb->ops.getdents = minix_getdents;
    
    // Load root node info
    minix_inode_t root_inode = get_minix_inode(drive, info, MINIX_ROOT_INODE);
    if (root_inode.i_mode == 0) {
        // Failed to read root inode
        kfree(info);
        return -1;
    }
    
    sb->root_node.inode_id = MINIX_ROOT_INODE;
    sb->root_node.type = FS_TYPE_DIRECTORY;
    sb->root_node.mode = root_inode.i_mode;
    sb->root_node.sb = sb;
    
    return 0;
}
