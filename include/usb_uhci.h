#ifndef USB_UHCI_H
#define USB_UHCI_H

#include <stdint.h>
#include "keyboard.h"

int uhci_init(uint8_t bus, uint8_t slot, uint8_t func, uint64_t mmio_base);
int uhci_keyboard_ready(void);
int uhci_keyboard_get_event(key_event_t *event);

#endif
