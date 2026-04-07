#ifndef ATA_H
#define ATA_H

#include "types.h"

// Standard sector size
#define ATA_SECTOR_SIZE 512

// --- Drive Selection ---
#define ATA_DRIVE_MASTER 0
#define ATA_DRIVE_SLAVE  1

// --- Status Register Bits ---
#define ATA_SR_BSY  0x80  // Busy
#define ATA_SR_DRDY 0x40  // Drive Ready
#define ATA_SR_DF   0x20  // Drive Write Fault
#define ATA_SR_DRQ  0x08  // Data Request Ready
#define ATA_SR_ERR  0x01  // Error

// --- Drive presence flags (set after ata_init()) ---
// ata_drive_present[ATA_DRIVE_MASTER] and ata_drive_present[ATA_DRIVE_SLAVE]
// are non-zero if the corresponding drive was detected.
extern uint8_t ata_drive_present[2];

// ---------------------------------------------------------------------------
// Public Interface
// ---------------------------------------------------------------------------

/**
 * @brief Probes the primary ATA bus for master and slave drives.
 *        Populates ata_drive_present[]. Must be called once at boot.
 */
void ata_init();

// --- Legacy (master-only) wrappers — kept for full fs.c compatibility ---

/**
 * @brief Reads sectors from the PRIMARY MASTER drive.
 *        All existing fs.c call sites continue to work unchanged.
 * @param lba   Starting Logical Block Address (28-bit).
 * @param count Number of sectors to read (1–256).
 * @param buffer Destination buffer (must be >= count * ATA_SECTOR_SIZE bytes).
 * @return 0 on success, -1 on failure.
 */
int ata_read_sectors(uint32_t lba, uint8_t count, void* buffer);

/**
 * @brief Writes sectors to the PRIMARY MASTER drive.
 * @param lba   Starting Logical Block Address (28-bit).
 * @param count Number of sectors to write (1–256).
 * @param buffer Source buffer.
 * @return 0 on success, -1 on failure.
 */
int ata_write_sectors(uint32_t lba, uint8_t count, void* buffer);

// --- Drive-specific variants (use these for slave / Minix FS image) ---

/**
 * @brief Reads sectors from the specified drive on the primary ATA bus.
 * @param drive  ATA_DRIVE_MASTER (0) or ATA_DRIVE_SLAVE (1).
 * @param lba    Starting Logical Block Address (28-bit).
 * @param count  Number of sectors to read (1–256).
 * @param buffer Destination buffer.
 * @return 0 on success, -1 on failure.
 */
int ata_read_sectors_ex(uint8_t drive, uint32_t lba, uint8_t count, void* buffer);

/**
 * @brief Writes sectors to the specified drive on the primary ATA bus.
 * @param drive  ATA_DRIVE_MASTER (0) or ATA_DRIVE_SLAVE (1).
 * @param lba    Starting Logical Block Address (28-bit).
 * @param count  Number of sectors to write (1–256).
 * @param buffer Source buffer.
 * @return 0 on success, -1 on failure.
 */
int ata_write_sectors_ex(uint8_t drive, uint32_t lba, uint8_t count, void* buffer);

#endif // ATA_H
