#ifndef KOSOS_USB_XHCI_H
#define KOSOS_USB_XHCI_H

#include "keyboard.h"

#include <stdint.h>

int xhci_init(uint8_t bus, uint8_t slot, uint8_t func, uint64_t mmio_base);
int xhci_keyboard_ready(void);
int xhci_keyboard_get_event(key_event_t *event);

#endif
