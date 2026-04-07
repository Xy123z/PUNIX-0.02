#ifndef PUNIXFS_VFS_H
#define PUNIXFS_VFS_H

#include "vfs.h"
#include "types.h"

// Initialize and mount a PUNIX native FS image from the given ATA drive,
// populating the given vfs_superblock_t. Returns 0 on success, -1 on failure.
int punixfs_init_mount(uint8_t drive, vfs_superblock_t* sb);

#endif // PUNIXFS_VFS_H
