#ifndef KOSOS_DISK_H
#define KOSOS_DISK_H

#include <stdint.h>

int disk_init(void);
uint32_t disk_total_sectors(void);
int disk_read(uint32_t lba, uint8_t *buffer);
int disk_write(uint32_t lba, const uint8_t *buffer);

#endif
