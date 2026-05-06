#ifndef KOSOS_USB_MSC_H
#define KOSOS_USB_MSC_H

#include <stdint.h>

/* USB MSC device info */
typedef struct {
    char vendor[9];
    char model[17];
    char serial[21];
    uint64_t capacity_bytes;
    uint32_t block_size;
} usb_msc_info_t;

/* MSC functions */
int usb_msc_init(void);
int usb_msc_ready(void);
int usb_msc_read_sector(uint32_t lba, uint8_t *buffer);
int usb_msc_write_sector(uint32_t lba, const uint8_t *buffer);
const usb_msc_info_t *usb_msc_get_info(void);

#endif
