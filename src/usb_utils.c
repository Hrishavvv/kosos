#include "usb_utils.h"

#include "port.h"

void *usb_memset(void *dest, int value, size_t count) {
    uint8_t *ptr = (uint8_t *) dest;
    for (size_t i = 0; i < count; ++i) {
        ptr[i] = (uint8_t) value;
    }
    return dest;
}

void *usb_memcpy(void *dest, const void *src, size_t count) {
    uint8_t *out = (uint8_t *) dest;
    const uint8_t *in = (const uint8_t *) src;
    for (size_t i = 0; i < count; ++i) {
        out[i] = in[i];
    }
    return dest;
}

void usb_delay(uint32_t loops) {
    for (uint32_t i = 0; i < loops; ++i) {
        io_wait();
    }
}
