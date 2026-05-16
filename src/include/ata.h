#ifndef ATA_H
#define ATA_H

#include "types.h"

#define ATA_OK    0
#define ATA_ERR  -1

/* Initialise the ATA driver.  Returns true if the slave disk is present. */
bool ata_init(void);

/* Returns true if the slave disk is present (after ata_init). */
bool ata_disk_present(void);

/* Returns the total sector count of the slave disk (0 if not present). */
uint32_t ata_disk_sectors(void);

/* Read `count` sectors starting at `lba` into `buf` (512*count bytes). */
int  ata_read(uint32_t lba, uint32_t count, void *buf);

/* Write `count` sectors from `buf` to disk starting at `lba`. */
int  ata_write(uint32_t lba, uint32_t count, const void *buf);

#endif /* ATA_H */
