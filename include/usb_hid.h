#ifndef KOSOS_USB_HID_H
#define KOSOS_USB_HID_H

#include "keyboard.h"

#include <stdint.h>

int usb_hid_translate(const uint8_t *report, key_event_t *event);

#endif
