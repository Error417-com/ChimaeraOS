/*
 * ChimaeraOS - ATA PIO Driver
 * drivers/ata.c
 *
 * Drives the primary ATA bus.  The OS disk (ISO) is on the MASTER (index 0).
 * The persistent data disk is on the SLAVE (index 1).
 */
#include "../include/ata.h"
#include "../include/serial.h"
#include "../include/types.h"

/* Primary ATA bus ports */
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01

#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_IDENTIFY      0xEC

/* Drive select: 0xE0 = master LBA, 0xF0 = slave LBA */
#define ATA_DRIVE_SLAVE  0xF0
#define ATA_DRIVE_MASTER 0xE0

static inline void outb(uint16_t port, uint8_t val)
{ __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port)); }

static inline uint8_t inb(uint16_t port)
{ uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

static inline uint16_t inw(uint16_t port)
{ uint16_t v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }

static inline void outw(uint16_t port, uint16_t val)
{ __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port)); }

static bool     disk_present  = false;
static uint32_t disk_sectors  = 0;

static void ata_wait_bsy(void)
{
    while (inb(ATA_STATUS) & ATA_STATUS_BSY) {}
}

static bool ata_wait_drq(void)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ATA_STATUS_ERR) return false;
        if (s & ATA_STATUS_DRQ) return true;
    }
    return false;
}

bool ata_init(void)
{
    /* Select slave drive */
    outb(ATA_DRIVE_HEAD, ATA_DRIVE_SLAVE);
    ata_wait_bsy();

    /* Send IDENTIFY */
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_wait_bsy();

    uint8_t status = inb(ATA_STATUS);
    if (status == 0x00 || status == 0xFF) {
        serial_puts("[ATA] No slave disk detected\n");
        disk_present = false;
        return false;
    }

    if (!ata_wait_drq()) {
        serial_puts("[ATA] Slave IDENTIFY failed\n");
        disk_present = false;
        return false;
    }

    /* Read IDENTIFY data — word 60-61 = total LBA28 sectors */
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);
    disk_sectors = ((uint32_t)id[61] << 16) | id[60];

    disk_present = true;
    serial_puts("[ATA] Slave disk detected\n");
    return true;
}

bool ata_disk_present(void)
{
    return disk_present;
}

uint32_t ata_disk_sectors(void)
{
    return disk_sectors;
}

int ata_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!disk_present) return ATA_ERR;
    uint16_t *p = (uint16_t *)buf;

    for (uint32_t s = 0; s < count; s++) {
        uint32_t cur = lba + s;
        ata_wait_bsy();
        outb(ATA_DRIVE_HEAD, (uint8_t)(ATA_DRIVE_SLAVE | ((cur >> 24) & 0x0F)));
        outb(ATA_SECTOR_CNT, 1);
        outb(ATA_LBA_LO,  (uint8_t)(cur >>  0));
        outb(ATA_LBA_MID, (uint8_t)(cur >>  8));
        outb(ATA_LBA_HI,  (uint8_t)(cur >> 16));
        outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);
        ata_wait_bsy();
        if (!ata_wait_drq()) return ATA_ERR;
        for (int i = 0; i < 256; i++) p[s * 256 + i] = inw(ATA_DATA);
    }
    return ATA_OK;
}

int ata_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!disk_present) return ATA_ERR;
    const uint16_t *p = (const uint16_t *)buf;

    for (uint32_t s = 0; s < count; s++) {
        uint32_t cur = lba + s;
        ata_wait_bsy();
        outb(ATA_DRIVE_HEAD, (uint8_t)(ATA_DRIVE_SLAVE | ((cur >> 24) & 0x0F)));
        outb(ATA_SECTOR_CNT, 1);
        outb(ATA_LBA_LO,  (uint8_t)(cur >>  0));
        outb(ATA_LBA_MID, (uint8_t)(cur >>  8));
        outb(ATA_LBA_HI,  (uint8_t)(cur >> 16));
        outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);
        ata_wait_bsy();
        if (!ata_wait_drq()) return ATA_ERR;
        for (int i = 0; i < 256; i++) outw(ATA_DATA, p[s * 256 + i]);
        /* Flush cache */
        ata_wait_bsy();
    }
    return ATA_OK;
}
