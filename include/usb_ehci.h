#ifndef KOSOS_USB_EHCI_H
#define KOSOS_USB_EHCI_H

#include "keyboard.h"

#include <stdint.h>

int ehci_init(uint8_t bus, uint8_t slot, uint8_t func, uint64_t mmio_base);
int ehci_keyboard_ready(void);
int ehci_keyboard_get_event(key_event_t *event);

/* Bulk transfer for MSC */
int ehci_submit_bulk_out(uint8_t addr, uint8_t ep, const uint8_t *data, uint32_t len);
int ehci_submit_bulk_in(uint8_t addr, uint8_t ep, uint8_t *buf, uint32_t len);

#endif
