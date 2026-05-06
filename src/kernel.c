#include "console.h"
#include "fs.h"
#include "keyboard.h"
#include "os.h"
#include "shell.h"
#include "usb.h"

#include <stdint.h>

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t type_attr;
    uint16_t reserved;
};

static const struct mb2_tag_framebuffer *find_framebuffer_tag(void *mb2_info) {
    if (!mb2_info) {
        return NULL;
    }

    uint32_t total_size = *(uint32_t *) mb2_info;
    uint8_t *ptr = (uint8_t *) mb2_info + 8;
    uint8_t *end = (uint8_t *) mb2_info + total_size;

    while (ptr + sizeof(struct mb2_tag) <= end) {
        struct mb2_tag *tag = (struct mb2_tag *) ptr;
        if (tag->type == 8) {
            return (struct mb2_tag_framebuffer *) tag;
        }
        if (tag->type == 0) {
            break;
        }
        uint32_t size = tag->size;
        if (size == 0) {
            break;
        }
        ptr += (size + 7) & ~7U;
    }

    return NULL;
}

void kmain64(void *mb2_info) {
    const struct mb2_tag_framebuffer *fb = find_framebuffer_tag(mb2_info);
    if (fb) {
        framebuffer_info_t info;
        info.address = fb->addr;
        info.width = fb->width;
        info.height = fb->height;
        info.pitch = fb->pitch;
        info.bpp = fb->bpp;
        console_init_framebuffer(&info);
    } else {
        console_init();
    }

    console_write(OS_NAME);
    console_write(" v");
    console_write(OS_VERSION);
    console_writeln(" - Made w/<3 by Hrishav.");
    console_writeln("Booting...");

    keyboard_init();
    usb_init();

    if (fs_init() == 0) {
        console_writeln("Storage: online (IDE primary master)");
    } else {
        console_writeln("Storage: offline");
    }

    shell_run();
}
