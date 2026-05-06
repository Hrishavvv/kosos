#include "usb_hid.h"

static char hid_usage_to_ascii(uint8_t usage, int shift) {
    if (usage >= 0x04 && usage <= 0x1D) {
        return (char) ((shift ? 'A' : 'a') + (usage - 0x04));
    }

    if (usage >= 0x1E && usage <= 0x27) {
        static const char unshifted[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        uint8_t index = (uint8_t) (usage - 0x1E);
        return shift ? shifted[index] : unshifted[index];
    }

    switch (usage) {
        case 0x28:
            return '\n';
        case 0x2A:
            return '\b';
        case 0x2B:
            return '\t';
        case 0x2C:
            return ' ';
        case 0x2D:
            return shift ? '_' : '-';
        case 0x2E:
            return shift ? '+' : '=';
        case 0x2F:
            return shift ? '{' : '[';
        case 0x30:
            return shift ? '}' : ']';
        case 0x31:
            return shift ? '|' : '\\';
        case 0x33:
            return shift ? ':' : ';';
        case 0x34:
            return shift ? '"' : '\'';
        case 0x35:
            return shift ? '~' : '`';
        case 0x36:
            return shift ? '<' : ',';
        case 0x37:
            return shift ? '>' : '.';
        case 0x38:
            return shift ? '?' : '/';
        default:
            return 0;
    }
}

static uint8_t hid_usage_to_keycode(uint8_t usage) {
    switch (usage) {
        case 0x4F:
            return KEY_RIGHT;
        case 0x50:
            return KEY_LEFT;
        case 0x51:
            return KEY_DOWN;
        case 0x52:
            return KEY_UP;
        default:
            return KEY_NONE;
    }
}

static uint8_t hid_report_key(const uint8_t *report) {
    for (int i = 2; i < 8; ++i) {
        if (report[i] != 0) {
            return report[i];
        }
    }
    return 0;
}

int usb_hid_translate(const uint8_t *report, key_event_t *event) {
    uint8_t mod = report[0];
    uint8_t key = hid_report_key(report);
    if (key == 0) {
        return 0;
    }

    int shift = (mod & 0x22u) != 0;
    int ctrl = (mod & 0x11u) != 0;

    uint8_t code = hid_usage_to_keycode(key);
    if (code != KEY_NONE) {
        event->ch = 0;
        event->code = code;
        event->ctrl = (uint8_t) ctrl;
        return 1;
    }

    char ch = hid_usage_to_ascii(key, shift);
    if (ch == 0) {
        return 0;
    }

    event->ch = ch;
    event->code = KEY_NONE;
    event->ctrl = (uint8_t) ctrl;
    return 1;
}
