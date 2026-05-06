#ifndef KOSOS_USB_OHCI_H
#define KOSOS_USB_OHCI_H

#include "keyboard.h"

#include <stdint.h>

int ohci_init(uint8_t bus, uint8_t slot, uint8_t func, uint64_t mmio_base);
int ohci_keyboard_ready(void);
int ohci_keyboard_get_event(key_event_t *event);

#endif
