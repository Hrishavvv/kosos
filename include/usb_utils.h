#ifndef KOSOS_USB_UTILS_H
#define KOSOS_USB_UTILS_H

#include <stddef.h>
#include <stdint.h>

void *usb_memset(void *dest, int value, size_t count);
void *usb_memcpy(void *dest, const void *src, size_t count);
void usb_delay(uint32_t loops);

#endif
