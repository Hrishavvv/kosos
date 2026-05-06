#ifndef KOSOS_USB_H
#define KOSOS_USB_H

#include "keyboard.h"

int usb_init(void);
int usb_keyboard_ready(void);
int usb_keyboard_get_event(key_event_t *event);

#endif
