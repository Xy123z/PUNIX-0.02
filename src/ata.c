#include "../include/ata.h"
#include "../include/types.h"
#include "../include/interrupt.h" // For inb/outb
#include "../include/console.h"   // For console_print_colored

// --- ATA I/O Ports (Primary Bus) ---
#define ATA_PRIMARY_BASE_IO 0x1F0
#define ATA_PRIMARY_DCR_AS  0x3F6  // Device Control Register / Alternate Status

// Register offsets relative to ATA_PRIMARY_BASE_IO
#define ATA_REG_DATA         0x00  // Data Register (16-bit)
#define ATA_REG_ERROR        0x01  // Error Register (R)
#define ATA_REG_SECTOR_COUNT 0x02  // Sector Count Register (R/W)
#define ATA_REG_LBA_LOW      0x03  // LBA bits  0– 7 (R/W)
#define ATA_REG_LBA_MID      0x04  // LBA bits  8–15 (R/W)
#define ATA_REG_LBA_HIGH     0x05  // LBA bits 16–23 (R/W)
#define ATA_REG_DRIVE_SEL    0x06  // Drive/Head Register (R/W)
#define ATA_REG_STATUS       0x07  // Status Register (R)
#define ATA_REG_COMMAND      0x07  // Command Register (W)

// ATA Commands
#define ATA_CMD_READ_PIO     0x20
#define ATA_CMD_WRITE_PIO    0x30
#define ATA_CMD_IDENTIFY     0xEC
#define ATA_CMD_CACHE_FLUSH  0xE7

// Drive/Head register base values (LBA mode bit 6 + always-1 bit 7)
// Bit 4: 0 = master, 1 = slave
#define ATA_SEL_MASTER 0xE0   // 1110_0000
#define ATA_SEL_SLAVE  0xF0   // 1111_0000

// --- Drive presence table (public, declared in ata.h) ---
uint8_t ata_drive_present[2] = {0, 0};

// ---------------------------------------------------------------------------
// 16-bit I/O helpers (ATA data transfers are 16-bit)
// ---------------------------------------------------------------------------

static inline uint16_t inw(uint16_t port) {
    uint16_t result;
    __asm__ volatile("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/**
 * @brief Sends 4 status reads to the Alternate Status register, giving the
 *        drive at least 400 ns to update its status lines after a command.
 */
static inline void ata_400ns_delay() {
    for (int i = 0; i < 4; i++) {
        inb(ATA_PRIMARY_DCR_AS);
    }
}

/**
 * @brief Waits for BSY to clear on the primary bus, then checks DRDY.
 *
 * The caller is responsible for having already selected the desired drive
 * before calling this function.
 *
 * @return  0  Drive is not busy and DRDY is set (ready for a command or transfer).
 * @return -1  Timeout, or the drive reported ERR / DF.
 */
static int ata_wait_for_ready() {
    ata_400ns_delay();

    // Phase 1: Wait for BSY to clear (up to ~500ms)
    int timeout = 30000000;
    while (timeout-- > 0) {
        uint8_t status = inb(ATA_PRIMARY_BASE_IO + ATA_REG_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return -1;
        }
        if (!(status & ATA_SR_BSY)) break;
    }
    if (timeout <= 0) {
        console_print_colored("ATA: Timeout waiting for BSY clear.\n", COLOR_LIGHT_RED);
        return -1;
    }

    // Phase 2: Wait for DRDY to assert (slave may take extra time)
    timeout = 30000000;
    while (timeout-- > 0) {
        uint8_t status = inb(ATA_PRIMARY_BASE_IO + ATA_REG_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return -1;
        }
        if (status & ATA_SR_DRDY) {
            return 0;
        }
    }

    console_print_colored("ATA: Timeout waiting for DRDY.\n", COLOR_LIGHT_RED);
    return -1;
}

/**
 * @brief Selects a drive and waits for DRQ (data request ready).
 *        Used between sectors during PIO read/write loops.
 *
 * @return  0 on success, -1 on timeout/error.
 */
static int ata_wait_for_drq() {
    ata_400ns_delay();

    int timeout = 10000000;
    while (timeout-- > 0) {
        uint8_t status = inb(ATA_PRIMARY_BASE_IO + ATA_REG_STATUS);

        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return -1;
        }
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) {
            return 0;
        }
    }

    console_print_colored("ATA: Timeout waiting for DRQ.\n", COLOR_LIGHT_RED);
    return -1;
}

/**
 * @brief Selects a drive (master or slave) on the primary bus and waits for
 *        the bus to settle.  Must be called before ata_wait_for_ready().
 *
 * @param drive  ATA_DRIVE_MASTER or ATA_DRIVE_SLAVE.
 */
static void ata_select_drive(uint8_t drive) {
    uint8_t sel = (drive == ATA_DRIVE_SLAVE) ? ATA_SEL_SLAVE : ATA_SEL_MASTER;
    outb(ATA_PRIMARY_BASE_IO + ATA_REG_DRIVE_SEL, sel);
    
    // 400ns delay for the mux to settle
    ata_400ns_delay();

    // Extra settle time for slave drives as some controllers take time to respond
    if (drive == ATA_DRIVE_SLAVE) {
        int settle = 1000000;
        while (settle-- > 0) {
            uint8_t status = inb(ATA_PRIMARY_BASE_IO + ATA_REG_STATUS);
            if (!(status & ATA_SR_BSY)) break;
        }
    }
}

/**
 * @brief Sets up the sector count, LBA address, drive selection, and command
 *        registers for a PIO read or write operation (LBA28 mode).
 *
 * @param drive    ATA_DRIVE_MASTER or ATA_DRIVE_SLAVE.
 * @param lba      28-bit starting LBA.
 * @param count    Number of sectors.
 * @param command  ATA_CMD_READ_PIO or ATA_CMD_WRITE_PIO.
 * @return 0 on success, -1 if the drive is not ready.
 */
static int ata_setup_command(uint8_t drive, uint32_t lba, uint8_t count, uint8_t command) {
    // Write Drive/Head register FIRST with drive selection and top 4 bits of LBA
    uint8_t drive_head = ((drive == ATA_DRIVE_SLAVE) ? ATA_SEL_SLAVE : ATA_SEL_MASTER)
                         | ((lba >> 24) & 0x0F);
    
    outb(ATA_PRIMARY_BASE_IO + ATA_REG_DRIVE_SEL, drive_head);
    
    // Selection takes 400ns to propagate
    ata_400ns_delay();

    if (ata_wait_for_ready() != 0) {
        console_print_colored("ATA: Drive not ready before command.\n", COLOR_LIGHT_RED);
        return -1;
    }

    outb(ATA_PRIMARY_BASE_IO + ATA_REG_SECTOR_COUNT, count);
    outb(ATA_PRIMARY_BASE_IO + ATA_REG_LBA_LOW,      (uint8_t)( lba        & 0xFF));
    outb(ATA_PRIMARY_BASE_IO + ATA_REG_LBA_MID,      (uint8_t)((lba >>  8) & 0xFF));
    outb(ATA_PRIMARY_BASE_IO + ATA_REG_LBA_HIGH,     (uint8_t)((lba >> 16) & 0xFF));

    // Finally send the command
    outb(ATA_PRIMARY_BASE_IO + ATA_REG_COMMAND, command);
    
    return 0;
}

/**
 * @brief Sends an IDENTIFY command to a drive and checks whether it responds
 *        correctly.  This is the standard way to detect drive presence without
 *        relying on the mere absence of a floating status byte.
 *
 * Algorithm:
 *   1. Select the drive and wait for bus to settle.
 *   2. Send IDENTIFY (0xEC).
 *   3. If status is 0x00 the drive is absent.
 *   4. Wait for BSY to clear.
 *   5. If LBA_MID or LBA_HIGH are non-zero the device is ATAPI — skip it.
 *   6. Wait for DRQ (drive has 256 words of ID data ready).
 *   7. Drain the 256 words so the bus is clean for subsequent commands.
 *
 * @param drive  ATA_DRIVE_MASTER or ATA_DRIVE_SLAVE.
 * @return 1 if a plain ATA (non-ATAPI) drive is present, 0 otherwise.
 */
static int ata_probe_drive(uint8_t drive) {
    ata_select_drive(drive);

    // Zero the address registers before IDENTIFY so we can detect ATAPI
    outb(ATA_PRIMARY_BASE_IO + ATA_REG_SECTOR_COUNT, 0);
    outb(ATA_PRIMARY_BASE_IO + ATA_REG_LBA_LOW,      0);
    outb(ATA_PRIMARY_BASE_IO + ATA_REG_LBA_MID,      0);
    outb(ATA_PRIMARY_BASE_IO + ATA_REG_LBA_HIGH,     0);

    outb(ATA_PRIMARY_BASE_IO + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_400ns_delay();

    // If status is 0x00 immediately after the command, nothing is connected
    uint8_t status = inb(ATA_PRIMARY_BASE_IO + ATA_REG_STATUS);
    if (status == 0x00 || status == 0xFF) {
        return 0;
    }

    // Wait for BSY to clear (drive is processing IDENTIFY)
    int timeout = 1000000;
    while (timeout-- > 0) {
        status = inb(ATA_PRIMARY_BASE_IO + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY)) break;
    }
    if (timeout <= 0) return 0;

    // Non-zero LBA_MID / LBA_HIGH after IDENTIFY means ATAPI, not ATA HDD
    uint8_t lba_mid  = inb(ATA_PRIMARY_BASE_IO + ATA_REG_LBA_MID);
    uint8_t lba_high = inb(ATA_PRIMARY_BASE_IO + ATA_REG_LBA_HIGH);
    if (lba_mid != 0 || lba_high != 0) {
        return 0;  // ATAPI device — not handled here
    }

    // Wait for DRQ (drive has identification data ready to read)
    timeout = 1000000;
    while (timeout-- > 0) {
        status = inb(ATA_PRIMARY_BASE_IO + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return 0;
        if (status & ATA_SR_DRQ) break;
    }
    if (timeout <= 0) return 0;

    // Drain the 256-word IDENTIFY data sector to clean up the bus
    for (int i = 0; i < 256; i++) {
        inw(ATA_PRIMARY_BASE_IO + ATA_REG_DATA);
    }

    return 1;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ata_init() {
    // Software reset: assert SRST, then de-assert
    outb(ATA_PRIMARY_DCR_AS, 0x04);
    outb(ATA_PRIMARY_DCR_AS, 0x00);

    // After a software reset both drives need time to come ready
    // Wait for the master (always present if any drive is there) to stop being busy
    ata_select_drive(ATA_DRIVE_MASTER);
    int timeout = 10000000;
    while (timeout-- > 0) {
        if (!(inb(ATA_PRIMARY_BASE_IO + ATA_REG_STATUS) & ATA_SR_BSY)) break;
    }

    // --- Probe master ---
    if (ata_probe_drive(ATA_DRIVE_MASTER)) {
        ata_drive_present[ATA_DRIVE_MASTER] = 1;
        console_print_colored("ATA: Primary Master detected.\n", COLOR_GREEN_ON_BLACK);
    } else {
        console_print_colored("ATA: Primary Master NOT found.\n", COLOR_LIGHT_RED);
    }

    // --- Probe slave ---
    if (ata_probe_drive(ATA_DRIVE_SLAVE)) {
        ata_drive_present[ATA_DRIVE_SLAVE] = 1;
        console_print_colored("ATA: Primary Slave detected.\n", COLOR_GREEN_ON_BLACK);
    } else {
        // Not an error — most systems only have a master
        console_print_colored("ATA: Primary Slave not present.\n", COLOR_LIGHT_GREY);
    }

    // Leave the master selected so the bus is in a known state
    ata_select_drive(ATA_DRIVE_MASTER);
}

// ---------------------------------------------------------------------------
// Core (drive-specific) read / write
// ---------------------------------------------------------------------------

int ata_read_sectors_ex(uint8_t drive, uint32_t lba, uint8_t count, void* buffer) {
    if (!ata_drive_present[drive]) {
        console_print_colored("ATA: Read on absent drive.\n", COLOR_LIGHT_RED);
        return -1;
    }

    if (ata_setup_command(drive, lba, count, ATA_CMD_READ_PIO) != 0) {
        return -1;
    }

    uint16_t* buf = (uint16_t*)buffer;
    for (int j = 0; j < count; j++) {
        // Each sector requires DRQ to be set before we can read its 256 words
        if (ata_wait_for_drq() != 0) {
            console_print_colored("ATA: Read sector failed - DRQ timeout.\n", COLOR_LIGHT_RED);
            return -1;
        }

        for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
            buf[i] = inw(ATA_PRIMARY_BASE_IO + ATA_REG_DATA);
        }

        buf += ATA_SECTOR_SIZE / 2;
    }

    // Final idle check: BSY and DRQ should both be clear
    ata_400ns_delay();
    if (inb(ATA_PRIMARY_BASE_IO + ATA_REG_STATUS) & (ATA_SR_BSY | ATA_SR_ERR | ATA_SR_DF)) {
        return -1;
    }

    return 0;
}

int ata_write_sectors_ex(uint8_t drive, uint32_t lba, uint8_t count, void* buffer) {
    if (!ata_drive_present[drive]) {
        console_print_colored("ATA: Write on absent drive.\n", COLOR_LIGHT_RED);
        return -1;
    }

    if (ata_setup_command(drive, lba, count, ATA_CMD_WRITE_PIO) != 0) {
        return -1;
    }

    uint16_t* buf = (uint16_t*)buffer;
    for (int j = 0; j < count; j++) {
        // Drive asserts DRQ when it is ready to accept another sector of data
        if (ata_wait_for_drq() != 0) {
            console_print_colored("ATA: Write sector failed - DRQ timeout.\n", COLOR_LIGHT_RED);
            return -1;
        }

        for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
            outw(ATA_PRIMARY_BASE_IO + ATA_REG_DATA, buf[i]);
        }

        buf += ATA_SECTOR_SIZE / 2;
    }

    // Flush write cache to physical media
    outb(ATA_PRIMARY_BASE_IO + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);

    if (ata_wait_for_ready() != 0) {
        console_print_colored("ATA: Write cache flush failed.\n", COLOR_LIGHT_RED);
        return -1;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Legacy wrappers — master-only, keep fs.c call sites untouched
// ---------------------------------------------------------------------------

int ata_read_sectors(uint32_t lba, uint8_t count, void* buffer) {
    return ata_read_sectors_ex(ATA_DRIVE_MASTER, lba, count, buffer);
}

int ata_write_sectors(uint32_t lba, uint8_t count, void* buffer) {
    return ata_write_sectors_ex(ATA_DRIVE_MASTER, lba, count, buffer);
}
