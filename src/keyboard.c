#include "keyboard.h"

#include "port.h"
#include "usb.h"

static int shift_down = 0;
static int ctrl_down = 0;
static int extended_prefix = 0;
static const int io_timeout = 100000;

static int keyboard_wait_read(void) {
    for (int i = 0; i < io_timeout; ++i) {
        if (inb(0x64) & 0x01U) {
            return 0;
        }
    }
    return -1;
}

static int keyboard_wait_write(void) {
    for (int i = 0; i < io_timeout; ++i) {
        if ((inb(0x64) & 0x02U) == 0U) {
            return 0;
        }
    }
    return -1;
}

void keyboard_init(void) {
    if (keyboard_wait_write() == 0) {
        outb(0x64, 0xAE);
    }

    for (int i = 0; i < 16; ++i) {
        if ((inb(0x64) & 0x01U) == 0U) {
            break;
        }
        (void) inb(0x60);
    }

    if (keyboard_wait_write() == 0) {
        outb(0x60, 0xF4);
        if (keyboard_wait_read() == 0) {
            (void) inb(0x60);
        }
    }
}

static char translate_scancode(uint8_t scancode) {
    static const char unshifted[] = {
        0,
        0,
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
        '-', '=',
        '\b',
        '\t',
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
        '[', ']', '\n',
        0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
        0,
        '\\',
        'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
        0,
        '*',
        0,
        ' '
    };

    static const char shifted[] = {
        0,
        0,
        '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
        '_', '+',
        '\b',
        '\t',
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
        '{', '}', '\n',
        0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
        0,
        '|',
        'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
        0,
        '*',
        0,
        ' '
    };

    if (scancode >= sizeof(unshifted) / sizeof(unshifted[0])) {
        return 0;
    }

    return shift_down ? shifted[scancode] : unshifted[scancode];
}

static uint8_t keyboard_read_scancode(void) {
    while (keyboard_wait_read() != 0) {
    }

    return inb(0x60);
}

int keyboard_get_event(key_event_t *event) {
    if (usb_keyboard_ready()) {
        return usb_keyboard_get_event(event);
    }
    for (;;) {
        uint8_t scancode = keyboard_read_scancode();

        if (scancode == 0xE0) {
            extended_prefix = 1;
            continue;
        }

        uint8_t released = (scancode & 0x80U);
        uint8_t code = (uint8_t) (scancode & 0x7FU);

        if (extended_prefix) {
            extended_prefix = 0;
            if (released) {
                continue;
            }
            event->ch = 0;
            event->ctrl = (uint8_t) ctrl_down;
            event->code = KEY_NONE;
            if (code == 0x48) {
                event->code = KEY_UP;
                return 1;
            }
            if (code == 0x50) {
                event->code = KEY_DOWN;
                return 1;
            }
            if (code == 0x4B) {
                event->code = KEY_LEFT;
                return 1;
            }
            if (code == 0x4D) {
                event->code = KEY_RIGHT;
                return 1;
            }
            continue;
        }

        if (code == 0x2A || code == 0x36) {
            shift_down = released ? 0 : 1;
            continue;
        }

        if (code == 0x1D) {
            ctrl_down = released ? 0 : 1;
            continue;
        }

        if (released) {
            continue;
        }

        char translated = translate_scancode(code);
        if (translated != 0) {
            event->ch = translated;
            event->code = KEY_NONE;
            event->ctrl = (uint8_t) ctrl_down;
            return 1;
        }
    }
}

char keyboard_getch(void) {
    key_event_t event;
    for (;;) {
        if (keyboard_get_event(&event) && event.code == KEY_NONE && event.ch != 0) {
            return event.ch;
        }
    }
}
