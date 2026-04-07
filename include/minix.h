#ifndef MINIX_H
#define MINIX_H

#include "types.h"
#include "vfs.h"

// MINIX V1 Superblock
typedef struct {
    uint16_t s_ninodes;
    uint16_t s_nzones;
    uint16_t s_imap_blocks;
    uint16_t s_zmap_blocks;
    uint16_t s_firstdatazone;
    uint16_t s_log_zone_size;
    uint32_t s_max_size;
    uint16_t s_magic;
    uint16_t s_state;
    // 32 bytes max? It's typically smaller, so padding is not strictly needed here but matching layout is important
} __attribute__((packed)) minix_super_block_t;

#define MINIX_V1_MAGIC 0x137F
#define MINIX_V1_MAGIC_30C 0x138F

// MINIX V1 Inode
typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_time;
    uint8_t  i_gid;
    uint8_t  i_nlinks;
    uint16_t i_zone[9]; // 7 direct, 1 indirect, 1 double indirect
} __attribute__((packed)) minix_inode_t;

#define MINIX_ROOT_INODE 1

// MINIX V1 Dir Entry
typedef struct {
    uint16_t inode;
    char name[14];
} __attribute__((packed)) minix_dir_entry_t;

// Extern mount init function
int minix_init_mount(uint8_t drive, vfs_superblock_t* sb);

#endif // MINIX_H
