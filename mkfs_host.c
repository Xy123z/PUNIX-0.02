/**
 * mkfs_host.c - Host-side Filesystem Image Creator
 * Creates a PUNIX-FS compatible filesystem image.
 *
 * Compile: gcc -o mkfs_host mkfs_host.c
 * Usage:   ./mkfs_host disk.img
 *
 * Changes from original:
 *  - Constants synced with updated fs.h (FS_MAX_INODES=1024, FS_INODE_TABLE_COUNT=256,
 *    FS_DATA_BLOCKS_START=539)
 *  - inode_t gains double_indirect_block field (padding[32] → padding[24], struct now truly 128 bytes)
 *  - superblock_t gains fs_state field (reserved[488] → reserved[487])
 *  - create_directory() adds "." and ".." dirents, manages parent link_count
 *  - create_file_from_buffer() supports double-indirect blocks (~8 MB max file)
 *  - Root directory gets "." and ".." entries during format
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

// --- Filesystem Constants (Must exactly match fs.h) ---
#define FS_MAGIC              0xEF5342
#define FS_ROOT_ID            1
#define FS_MAX_INODES         1024       // was 256
#define FS_MAX_NAME           60
#define SECTOR_SIZE           512
#define PTRS_PER_BLOCK        (SECTOR_SIZE / sizeof(uint32_t))  // 128

#define FS_TYPE_FILE          0
#define FS_TYPE_DIRECTORY     1
#define FS_TYPE_CHARDEV       2

// FS state flags
#define FS_STATE_CLEAN        0
#define FS_STATE_DIRTY        1

// Permission bits
#ifndef S_IRUSR
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#endif

// Disk Layout (Must exactly match fs.h)
#define FS_SUPERBLOCK_SECTOR   256
#define FS_INODE_BITMAP_SECTOR 257
#define FS_BLOCK_BITMAP_START  258
#define FS_BLOCK_BITMAP_COUNT  25
#define FS_INODE_TABLE_START   283
#define FS_INODE_TABLE_COUNT   256      // was 64; supports 1024 inodes
#define FS_DATA_BLOCKS_START   539      // was 347; = 283 + 256

// --- Data Structures (Must exactly match fs.h / fs.c) ---

typedef struct {
    uint32_t magic;
    uint32_t root_inode;
    uint32_t total_inodes;
    uint32_t total_blocks;
    uint32_t free_inodes;
    uint32_t free_blocks;
    uint8_t  fs_state;       // NEW: FS_STATE_CLEAN / FS_STATE_DIRTY
    uint8_t  reserved[487];  // was reserved[488]
} superblock_t;

typedef struct {
    uint32_t id;
    uint32_t parent_id;
    uint8_t  type;
    uint8_t  _pad0[3];                  // explicit padding
    uint32_t mode;
    uint16_t link_count;
    uint8_t  _pad1[2];                  // explicit padding
    uint32_t uid;
    uint32_t gid;
    uint32_t size;
    uint32_t block_count;
    uint32_t blocks[12];
    uint32_t indirect_block;
    uint32_t double_indirect_block;     // NEW (was first 4 bytes of padding[32])
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint8_t  padding[24];               // 104 bytes before + 24 = 128 bytes exactly
} inode_t;

typedef struct {
    uint32_t inode_id;
    char     name[FS_MAX_NAME];
} fs_dirent_t;

// Sanity check: inode_t must be exactly 128 bytes on the host too.
// If this fires, recheck padding fields above.
typedef char _inode_size_check[ (sizeof(inode_t) == 128) ? 1 : -1 ];

// --- Global State ---
static FILE*        disk_file = NULL;
static superblock_t sb;

// Helper macros
#define INODE_TO_SECTOR(id)     (FS_INODE_TABLE_START + ((id) / (SECTOR_SIZE / sizeof(inode_t))))
#define INODE_OFF_IN_SECTOR(id) (((id) % (SECTOR_SIZE / sizeof(inode_t))) * sizeof(inode_t))

// --- Low-level I/O ---

static void die(const char* msg) {
    fprintf(stderr, "ERROR: %s\n", msg);
    if (disk_file) fclose(disk_file);
    exit(1);
}

static void seek_sector(uint32_t sector) {
    if (fseek(disk_file, (long)sector * SECTOR_SIZE, SEEK_SET) != 0)
        die("Failed to seek to sector");
}

static void read_sector(uint32_t sector, void* buffer) {
    seek_sector(sector);
    if (fread(buffer, SECTOR_SIZE, 1, disk_file) != 1)
        die("Failed to read sector");
}

static void write_sector(uint32_t sector, const void* buffer) {
    seek_sector(sector);
    if (fwrite(buffer, SECTOR_SIZE, 1, disk_file) != 1)
        die("Failed to write sector");
}

static void write_superblock() {
    write_sector(FS_SUPERBLOCK_SECTOR, &sb);
}

// --- Bitmap Management ---

static void bitmap_set(uint32_t sector_start, uint32_t bit, int value) {
    uint8_t  buf[SECTOR_SIZE];
    uint32_t sector  = sector_start + (bit / (SECTOR_SIZE * 8));
    uint32_t bit_off = bit % (SECTOR_SIZE * 8);
    read_sector(sector, buf);
    if (value) buf[bit_off / 8] |=  (1 << (bit_off % 8));
    else       buf[bit_off / 8] &= ~(1 << (bit_off % 8));
    write_sector(sector, buf);
}

static int bitmap_find_free(uint32_t sector_start, uint32_t count) {
    uint8_t buf[SECTOR_SIZE];
    for (uint32_t s = 0; s < (count + 4095) / 4096; s++) {
        read_sector(sector_start + s, buf);
        for (int i = 0; i < SECTOR_SIZE; i++) {
            if (buf[i] != 0xFF) {
                for (int b = 0; b < 8; b++) {
                    if (!(buf[i] & (1 << b))) {
                        uint32_t res = (s * 4096) + (i * 8) + b;
                        if (res < count) return (int)res;
                    }
                }
            }
        }
    }
    return -1;
}

// --- Inode / Block Allocation ---

static int inode_alloc() {
    int id = bitmap_find_free(FS_INODE_BITMAP_SECTOR, FS_MAX_INODES);
    if (id < 0) return -1;
    bitmap_set(FS_INODE_BITMAP_SECTOR, id, 1);
    sb.free_inodes--;
    write_superblock();
    return id;
}

static uint32_t block_alloc() {
    int id = bitmap_find_free(FS_BLOCK_BITMAP_START, sb.total_blocks);
    if (id < 0) die("No free blocks");
    bitmap_set(FS_BLOCK_BITMAP_START, (uint32_t)id, 1);
    sb.free_blocks--;
    write_superblock();
    uint8_t zero[SECTOR_SIZE];
    memset(zero, 0, SECTOR_SIZE);
    write_sector((uint32_t)id, zero);
    return (uint32_t)id;
}

// --- Inode I/O ---

static void write_inode(uint32_t id, const inode_t* node) {
    uint8_t buf[SECTOR_SIZE];
    read_sector(INODE_TO_SECTOR(id), buf);
    memcpy(buf + INODE_OFF_IN_SECTOR(id), node, sizeof(inode_t));
    write_sector(INODE_TO_SECTOR(id), buf);
}

static void read_inode(uint32_t id, inode_t* node) {
    uint8_t buf[SECTOR_SIZE];
    read_sector(INODE_TO_SECTOR(id), buf);
    memcpy(node, buf + INODE_OFF_IN_SECTOR(id), sizeof(inode_t));
}

// --- Low-level: append a directory entry into a directory inode ---
// Returns 1 on success, 0 on failure.
static int dir_add_entry(inode_t* dir, uint32_t inode_id, const char* name) {
    fs_dirent_t entries[SECTOR_SIZE / sizeof(fs_dirent_t)];

    for (int i = 0; i < 12; i++) {
        uint32_t b_id = dir->blocks[i];
        if (b_id == 0) {
            b_id = block_alloc();
            dir->blocks[i]   = b_id;
            dir->block_count++;
            memset(entries, 0, SECTOR_SIZE);
        } else {
            read_sector(b_id, entries);
        }
        for (int j = 0; j < (int)(SECTOR_SIZE / sizeof(fs_dirent_t)); j++) {
            if (entries[j].inode_id == 0) {
                entries[j].inode_id = inode_id;
                strncpy(entries[j].name, name, FS_MAX_NAME - 1);
                entries[j].name[FS_MAX_NAME - 1] = '\0';
                write_sector(b_id, entries);
                dir->size += sizeof(fs_dirent_t);
                return 1;
            }
        }
    }
    return 0;
}

// --- High-level Operations ---

static void format_filesystem() {
    uint8_t zero_sector[SECTOR_SIZE];
    memset(zero_sector, 0, SECTOR_SIZE);

    printf("Formatting filesystem (FS_MAX_INODES=%d, FS_DATA_BLOCKS_START=%d)...\n",
           FS_MAX_INODES, FS_DATA_BLOCKS_START);

    write_sector(FS_INODE_BITMAP_SECTOR, zero_sector);

    for (int i = 0; i < FS_BLOCK_BITMAP_COUNT; i++)
        write_sector(FS_BLOCK_BITMAP_START + i, zero_sector);

    // Now 256 sectors for the inode table
    for (int i = 0; i < FS_INODE_TABLE_COUNT; i++)
        write_sector(FS_INODE_TABLE_START + i, zero_sector);

    memset(&sb, 0, sizeof(sb));
    sb.magic        = FS_MAGIC;
    sb.root_inode   = FS_ROOT_ID;
    sb.total_inodes = FS_MAX_INODES;
    sb.total_blocks = 102400;   // 50 MB
    sb.free_inodes  = FS_MAX_INODES;
    sb.free_blocks  = sb.total_blocks - FS_DATA_BLOCKS_START;
    sb.fs_state     = FS_STATE_CLEAN;  // mkfs produces a clean image
    write_superblock();

    // Reserve inode 0 and all pre-data blocks
    bitmap_set(FS_INODE_BITMAP_SECTOR, 0, 1);
    for (int i = 0; i < FS_DATA_BLOCKS_START; i++)
        bitmap_set(FS_BLOCK_BITMAP_START, i, 1);

    printf("Superblock written (magic: 0x%X, state: CLEAN)\n", sb.magic);
}

/**
 * @brief Creates a directory, writes it to disk, and links it into parent.
 *        Adds "." and ".." entries. Increments parent's link_count for subdirs.
 * @return New directory's inode ID.
 */
static uint32_t create_directory(uint32_t parent_id, const char* name, uint32_t mode) {
    if (sb.free_inodes == 0) die("No free inodes");

    int new_id = inode_alloc();
    if (new_id < 0) die("Failed to allocate inode");

    inode_t node;
    memset(&node, 0, sizeof(inode_t));
    node.id         = new_id;
    node.parent_id  = parent_id;
    node.type       = FS_TYPE_DIRECTORY;
    node.mode       = mode;
    node.link_count = 2;    // 1 for parent's dirent + 1 for its own "." entry
    node.uid        = 0;
    node.gid        = 0;
    node.size       = 0;
    node.atime = node.mtime = node.ctime = (uint32_t)time(NULL);

    // Add "." and ".." before writing inode (dir_add_entry updates node.blocks etc.)
    dir_add_entry(&node, (uint32_t)new_id, ".");    // "."  -> self
    dir_add_entry(&node, parent_id,        "..");   // ".." -> parent

    write_inode((uint32_t)new_id, &node);

    printf("  Created directory '%s' (inode %d, mode 0%o)\n", name, new_id, mode);

    // Link into parent
    if (parent_id != 0) {
        inode_t parent;
        read_inode(parent_id, &parent);
        if (parent.type != FS_TYPE_DIRECTORY) die("Parent is not a directory");

        if (!dir_add_entry(&parent, (uint32_t)new_id, name))
            die("Parent directory is full");

        // ".." in new subdir points back to parent — so parent gets +1 link
        parent.link_count++;
        parent.mtime = (uint32_t)time(NULL);
        write_inode(parent_id, &parent);
    }

    return (uint32_t)new_id;
}

/**
 * @brief Creates a file from an in-memory buffer, with double-indirect support.
 * @return New file's inode ID.
 */
static uint32_t create_file_from_buffer(uint32_t parent_id, const char* name,
                                         const void* data, uint32_t data_len, uint32_t mode) {
    if (sb.free_inodes == 0) die("No free inodes");

    int new_id = inode_alloc();
    if (new_id < 0) die("Failed to allocate inode");

    inode_t node;
    memset(&node, 0, sizeof(inode_t));
    node.id         = (uint32_t)new_id;
    node.parent_id  = parent_id;
    node.type       = FS_TYPE_FILE;
    node.mode       = mode;
    node.link_count = 1;
    node.uid        = 0;
    node.gid        = 0;
    node.size       = data_len;
    node.atime = node.mtime = node.ctime = (uint32_t)time(NULL);

    // Maximum supported size: 12 direct + 128 indirect + 128*128 double-indirect
    uint32_t max_blocks = 12 + PTRS_PER_BLOCK + PTRS_PER_BLOCK * PTRS_PER_BLOCK;
    uint32_t max_bytes  = max_blocks * SECTOR_SIZE;
    if (data_len > max_bytes) {
        fprintf(stderr, "  WARNING: '%s' too large (%u bytes), truncating to %u bytes\n",
                name, data_len, max_bytes);
        data_len = max_bytes;
        node.size = data_len;
    }

    if (data_len > 0) {
        uint32_t  remaining = data_len;
        uint32_t  offset    = 0;
        uint8_t   block_buf[SECTOR_SIZE];
        uint32_t  block_num = 0;   // logical block index within the file

        while (remaining > 0) {
            uint32_t to_write = (remaining > SECTOR_SIZE) ? SECTOR_SIZE : remaining;
            memset(block_buf, 0, SECTOR_SIZE);
            memcpy(block_buf, (const uint8_t*)data + offset, to_write);

            uint32_t b_id = block_alloc();

            if (block_num < 12) {
                // Direct
                node.blocks[block_num] = b_id;
                node.block_count++;

            } else if (block_num < 12 + PTRS_PER_BLOCK) {
                // Single-indirect
                if (node.indirect_block == 0) {
                    node.indirect_block = block_alloc();
                    uint8_t zero[SECTOR_SIZE];
                    memset(zero, 0, SECTOR_SIZE);
                    write_sector(node.indirect_block, zero);
                }
                uint32_t indices[PTRS_PER_BLOCK];
                read_sector(node.indirect_block, indices);
                uint32_t si_idx = block_num - 12;
                indices[si_idx] = b_id;
                node.block_count++;
                write_sector(node.indirect_block, indices);

            } else {
                // Double-indirect
                uint32_t di_idx = block_num - 12 - PTRS_PER_BLOCK;
                uint32_t l1_idx = di_idx / PTRS_PER_BLOCK;
                uint32_t l2_idx = di_idx % PTRS_PER_BLOCK;
                uint8_t zero[SECTOR_SIZE];
                memset(zero, 0, SECTOR_SIZE);

                if (node.double_indirect_block == 0) {
                    node.double_indirect_block = block_alloc();
                    write_sector(node.double_indirect_block, zero);
                }
                uint32_t l1[PTRS_PER_BLOCK];
                read_sector(node.double_indirect_block, l1);

                if (l1[l1_idx] == 0) {
                    l1[l1_idx] = block_alloc();
                    write_sector(node.double_indirect_block, l1);
                    write_sector(l1[l1_idx], zero);
                }
                uint32_t l2[PTRS_PER_BLOCK];
                read_sector(l1[l1_idx], l2);
                l2[l2_idx] = b_id;
                node.block_count++;
                write_sector(l1[l1_idx], l2);
            }

            write_sector(b_id, block_buf);
            offset    += to_write;
            remaining -= to_write;
            block_num++;
        }
    }

    write_inode((uint32_t)new_id, &node);

    printf("  Created file '%s' (inode %d, %u bytes, mode 0%o)\n",
           name, new_id, data_len, mode);

    // Add to parent directory
    inode_t parent;
    read_inode(parent_id, &parent);
    if (parent.type != FS_TYPE_DIRECTORY) die("Parent is not a directory");

    if (!dir_add_entry(&parent, (uint32_t)new_id, name))
        die("Parent directory is full");

    parent.mtime = (uint32_t)time(NULL);
    write_inode(parent_id, &parent);

    return (uint32_t)new_id;
}

static uint32_t create_file(uint32_t parent_id, const char* name, const char* content) {
    uint32_t len = content ? (uint32_t)strlen(content) : 0;
    return create_file_from_buffer(parent_id, name, content, len, 0644);
}

static uint32_t copy_host_file(uint32_t parent_id, const char* dest_name,
                                const char* src_path, uint32_t mode) {
    FILE* f = fopen(src_path, "rb");
    if (!f) {
        fprintf(stderr, "  WARNING: Could not open '%s' - skipping\n", src_path);
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Hard cap at double-indirect limit
    uint32_t max_bytes = (12 + PTRS_PER_BLOCK + PTRS_PER_BLOCK * PTRS_PER_BLOCK) * SECTOR_SIZE;
    if ((uint32_t)size > max_bytes) {
        fprintf(stderr, "  WARNING: '%s' too large (%ld bytes) - truncating to %u\n",
                src_path, size, max_bytes);
        size = (long)max_bytes;
    }

    uint8_t* buffer = malloc((size_t)size);
    if (!buffer) { fclose(f); die("malloc failed"); }

    size_t bytes_read = fread(buffer, 1, (size_t)size, f);
    fclose(f);

    if (bytes_read != (size_t)size) {
        fprintf(stderr, "  WARNING: Only read %zu of %ld bytes from '%s'\n",
                bytes_read, size, src_path);
        size = (long)bytes_read;
    }

    uint32_t id = create_file_from_buffer(parent_id, dest_name, buffer, (uint32_t)size, mode);
    free(buffer);
    return id;
}

// --- Main ---

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <disk_image>\n", argv[0]);
        return 1;
    }

    printf("====================================\n");
    printf("PUNIX-FS Host Creator\n");
    printf("sizeof(inode_t) = %zu (must be 128)\n", sizeof(inode_t));
    printf("====================================\n\n");

    if (sizeof(inode_t) != 128) {
        fprintf(stderr, "FATAL: inode_t is %zu bytes, expected 128. Fix padding.\n",
                sizeof(inode_t));
        return 1;
    }

    disk_file = fopen(argv[1], "r+b");
    if (!disk_file) {
        fprintf(stderr, "ERROR: Could not open '%s'\n", argv[1]);
        return 1;
    }

    // Check for existing filesystem
    uint8_t sb_buf[SECTOR_SIZE];
    read_sector(FS_SUPERBLOCK_SECTOR, sb_buf);
    memcpy(&sb, sb_buf, sizeof(superblock_t));

    if (sb.magic == FS_MAGIC) {
        printf("WARNING: Filesystem already exists on this disk!\n");
        printf("Do you want to reformat? (yes/no): ");
        char response[10];
        if (!fgets(response, sizeof(response), stdin) ||
            strcmp(response, "yes\n") != 0) {
            printf("Aborted.\n");
            fclose(disk_file);
            return 0;
        }
    }

    format_filesystem();

    // Create root inode (written manually before using create_directory)
    printf("\nCreating root directory...\n");
    inode_t root;
    memset(&root, 0, sizeof(inode_t));
    root.id         = FS_ROOT_ID;
    root.parent_id  = FS_ROOT_ID;  // root is its own parent
    root.type       = FS_TYPE_DIRECTORY;
    root.mode       = S_IFDIR | 0755;  // BUG FIX: was bare 0755, missing S_IFDIR type bits
    root.link_count = 2;            // "." inside itself counts as a link
    root.atime = root.mtime = root.ctime = (uint32_t)time(NULL);

    // Add "." and ".." for root
    dir_add_entry(&root, FS_ROOT_ID, ".");
    dir_add_entry(&root, FS_ROOT_ID, "..");

    write_inode(FS_ROOT_ID, &root);
    bitmap_set(FS_INODE_BITMAP_SECTOR, FS_ROOT_ID, 1);
    sb.free_inodes--;
    write_superblock();

    printf("  Root directory created (inode %d, link_count=2, with '.' and '..')\n", FS_ROOT_ID);

    // Standard Unix directory structure
    printf("\nCreating Linux-style directory structure...\n");
    uint32_t bin_id    = create_directory(FS_ROOT_ID, "bin",  0755);
    uint32_t boot_id   = create_directory(FS_ROOT_ID, "boot", 0755);
    uint32_t dev_id    = create_directory(FS_ROOT_ID, "dev",  0755);
    uint32_t etc_id    = create_directory(FS_ROOT_ID, "etc",  0755);
    uint32_t home_id   = create_directory(FS_ROOT_ID, "home", 0775);
    uint32_t lib_id    = create_directory(FS_ROOT_ID, "lib",  0755);
    uint32_t mnt_id    = create_directory(FS_ROOT_ID, "mnt",  0755);
    uint32_t opt_id    = create_directory(FS_ROOT_ID, "opt",  0755);
    uint32_t proc_id   = create_directory(FS_ROOT_ID, "proc", 0755);
                         create_directory(FS_ROOT_ID, "root", 0700);
    uint32_t sbin_id   = create_directory(FS_ROOT_ID, "sbin", 0755);
                         create_directory(FS_ROOT_ID, "srv",  0755);
    uint32_t tmp_id    = create_directory(FS_ROOT_ID, "tmp",  0777);
    uint32_t usr_id    = create_directory(FS_ROOT_ID, "usr",  0755);
    uint32_t var_id    = create_directory(FS_ROOT_ID, "var",  0755);

    // Suppress unused-variable warnings for placeholder dirs
    (void)lib_id; (void)mnt_id; (void)opt_id; (void)proc_id; (void)tmp_id;

    // /usr subdirectories
    uint32_t usr_bin  = create_directory(usr_id, "bin",   0755);
                        create_directory(usr_id, "sbin",  0755);
                        create_directory(usr_id, "lib",   0755);
                        create_directory(usr_id, "local", 0755);
                        create_directory(usr_id, "share", 0755);

    // /var subdirectories
    uint32_t var_log  = create_directory(var_id, "log", 0755);
                        create_directory(var_id, "tmp", 0755);
    (void)var_log;

    // /home/user
    uint32_t user_home = create_directory(home_id, "user", 0755);

    // Binaries
    printf("\nCopying binaries...\n");
    copy_host_file(boot_id, "bootloader.bin", "boot.bin",    0644);
    copy_host_file(boot_id, "kernel.bin",     "kernel.bin",  0644);

    copy_host_file(sbin_id, "getty",        "getty.prog",       0755);
    copy_host_file(sbin_id, "login",        "login.prog",       0755);
    copy_host_file(sbin_id, "ps",           "ps.prog",          0755);
    copy_host_file(sbin_id, "kill",         "kill.prog",        0755);
    copy_host_file(sbin_id, "mem",          "mem.prog",         0755);
    copy_host_file(sbin_id, "init",         "init.prog",        0755);
    copy_host_file(sbin_id, "zombie_test",  "zombie_test.prog", 0755);

    uint32_t bash_id = copy_host_file(bin_id, "bash",    "pbash.prog", 0755);
    if (bash_id) copy_host_file(bin_id, "sh", "pbash.prog", 0755);

    copy_host_file(bin_id, "ls",      "ls.prog",      0755);
    copy_host_file(bin_id, "mkdir",   "mkdir.prog",   0755);
    copy_host_file(bin_id, "rmdir",   "rmdir.prog",   0755);
    copy_host_file(bin_id, "cat",     "cat.prog",     0755);
    copy_host_file(bin_id, "pwd",     "pwd.prog",     0755);
    copy_host_file(bin_id, "clear",   "clear.prog",   0755);
    copy_host_file(bin_id, "chmod",   "chmod.prog",   0755);
    copy_host_file(bin_id, "echo",    "echo.prog",    0755);
    copy_host_file(bin_id, "snake",   "snake.prog",   0755);
    copy_host_file(bin_id, "loadbar", "loadbar.prog", 0755);

    copy_host_file(usr_bin, "pnano",    "text.prog",      0755);
    copy_host_file(usr_bin, "clock",    "clock.prog",     0755);
    copy_host_file(usr_bin, "testvga",  "test_vga.prog",  0755);
   copy_host_file(usr_bin, "hamming_code",  "hamming_code.prog",  0755);
    copy_host_file(usr_bin, "kilo",  "kilo.prog",  0755);
    copy_host_file(usr_bin, "malloc_test",  "malloc_test.prog",  0755);
    copy_host_file(usr_bin, "malloc_test_2",  "malloc_test_2.prog",  0755);
    copy_host_file(usr_bin, "memtest",  "memtest.prog",  0755);
   copy_host_file(usr_bin, "zombie_orphan",  "zombie_orphan.prog",  0755);
   copy_host_file(usr_bin, "test_libc",  "test_libc.prog",  0755);
   copy_host_file(usr_bin, "test_env",  "test_env.prog",  0755);
   copy_host_file(usr_bin, "pf_test",  "pf_test.prog",  0755);

    // /dev/tty0-3 (character devices)
    printf("\nCreating /dev entries...\n");
    for (int i = 0; i < 4; i++) {
        char tty_name[16];
        sprintf(tty_name, "tty%d", i);

        int tid = inode_alloc();
        if (tid < 0) die("No free inodes for tty");

        inode_t tnode;
        memset(&tnode, 0, sizeof(inode_t));
        tnode.id             = (uint32_t)tid;
        tnode.parent_id      = dev_id;
        tnode.type           = FS_TYPE_CHARDEV;
        tnode.mode           = 0020666;
        tnode.link_count     = 1;
        tnode.indirect_block = (4 << 8) | (uint32_t)i;  // Major 4, Minor i
        tnode.atime = tnode.mtime = tnode.ctime = (uint32_t)time(NULL);
        write_inode((uint32_t)tid, &tnode);

        inode_t dev_inode;
        read_inode(dev_id, &dev_inode);
        if (!dir_add_entry(&dev_inode, (uint32_t)tid, tty_name))
            die("Failed to add tty to /dev");
        dev_inode.mtime = (uint32_t)time(NULL);
        write_inode(dev_id, &dev_inode);

        printf("  Created /dev/%s (Major 4, Minor %d, inode %d)\n", tty_name, i, tid);
    }

    // Config files
    printf("\nCreating /etc configuration files...\n");

    create_file(etc_id, "fstab",
        "# /etc/fstab\n"
        "/dev/hda1  /  punixfs  defaults\n");

    create_file(etc_id, "hostname", "punix-system\n");

    create_file(etc_id, "passwd",
        "root:root:0:0:root:/root:/bin/sh\n"
        );

    create_file(etc_id, "motd",
        "========================================\n"
        "Welcome to PUNIX Operating System\n"
        "========================================\n"
        "\n"
        "Type 'help' for available commands.\n\n");

    create_file(etc_id, "os-release",
        "NAME=\"PUNIX\"\n"
        "VERSION=\"0.1\"\n"
        "ID=punix\n"
        "PRETTY_NAME=\"PUNIX 0.1\"\n");

    // User files
    printf("\nCreating /home/user files...\n");

    create_file(user_home, "welcome.txt",
        "Welcome to PUNIX!\n\n"
        "This is your home directory.\n\n"
        "Commands: ls, cd, cat, edit, mkdir, rm, help\n\n"
        "Try: cd /  then  ls\n\n");

    create_file(user_home, "notes.txt",
        "My Notes\n========\n\n"
        "- Explore the filesystem\n"
        "- Try the text editor\n\n");

    create_file(FS_ROOT_ID, "README",
        "PUNIX Operating System\n"
        "======================\n\n"
        "/bin   essential user binaries\n"
        "/boot  bootloader and kernel\n"
        "/dev   device files\n"
        "/etc   configuration\n"
        "/home  user home directories\n"
        "/proc  process info (future)\n"
        "/sbin  system binaries\n"
        "/tmp   temporary files\n"
        "/usr   user programs\n"
        "/var   logs and variable data\n\n");

    // Summary
    printf("\n====================================\n");
    printf("Filesystem image created successfully!\n");
    printf("====================================\n");
    printf("Inodes  : total=%-5d  free=%-5d  used=%d\n",
           sb.total_inodes, sb.free_inodes, sb.total_inodes - sb.free_inodes);
    printf("Blocks  : total=%-5d  free=%-5d  used=%d\n",
           sb.total_blocks, sb.free_blocks, sb.total_blocks - sb.free_blocks);
    printf("Used    : %u KB  (%.2f%%)\n",
           ((sb.total_blocks - sb.free_blocks) * SECTOR_SIZE) / 1024,
           (float)(sb.total_blocks - sb.free_blocks) * 100.0f / sb.total_blocks);
    printf("State   : CLEAN (safe to boot)\n");
    printf("====================================\n");

    fclose(disk_file);
    return 0;
}
