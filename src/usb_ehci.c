#include "usb_ehci.h"

#include "console.h"
#include "pci.h"
#include "usb_utils.h"
#include "usb_hid.h"

#define EHCI_USBCMD 0x00u
#define EHCI_USBSTS 0x04u
#define EHCI_CONFIGFLAG 0x40u
#define EHCI_PORTSC_BASE 0x44u

#define EHCI_USBCMD_RUN 0x00000001u
#define EHCI_USBCMD_RESET 0x00000002u
#define EHCI_USBSTS_HALTED 0x00001000u

static volatile uint8_t *ehci_mmio = 0;
static uint8_t ehci_cap_len = 0;
static uint8_t ehci_ports = 0;

/* EHCI data structures */
typedef struct {
    uint32_t next_qtd;
    uint32_t alt_next_qtd;
    uint32_t token;
    uint32_t buffer[5];
} ehci_qtd_t;

typedef struct {
    uint32_t horiz_link;
    uint32_t ep_char;
    uint32_t ep_caps;
    uint32_t curr_qtd;
    uint32_t next_qtd;
    uint32_t alt_next_qtd;
    uint32_t token;
    uint32_t buffer[5];
} ehci_qh_t;

static ehci_qh_t ehci_qh_mem __attribute__((aligned(64)));
static ehci_qtd_t ehci_qtds[8] __attribute__((aligned(64)));
static uint8_t ehci_ctrl_buffer[512] __attribute__((aligned(64)));
static volatile int ehci_kbd_ready = 0;
static volatile int ehci_kbd_pending = 0;
static uint8_t ehci_kbd_report[8];
static uint8_t ehci_kbd_ep = 0;
static uint8_t ehci_kbd_addr = 0;
static uint16_t ehci_kbd_pkt = 8;

static uint32_t ehci_cap_read32(uint32_t offset) {
    return *(volatile uint32_t *) (ehci_mmio + offset);
}

static uint32_t ehci_op_read32(uint32_t offset) {
    return *(volatile uint32_t *) (ehci_mmio + ehci_cap_len + offset);
}

static void ehci_op_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *) (ehci_mmio + ehci_cap_len + offset) = value;
}

static int ehci_wait_usbsts(uint32_t mask, uint32_t value) {
    for (uint32_t i = 0; i < 200000; ++i) {
        if ((ehci_op_read32(EHCI_USBSTS) & mask) == value) {
            return 0;
        }
    }
    return -1;
}

static void ehci_legacy_handoff(uint8_t bus, uint8_t slot, uint8_t func, uint8_t eecp) {
    if (eecp == 0) {
        return;
    }

    uint32_t legsup = pci_read_config32(bus, slot, func, eecp);
    legsup |= (1u << 24);
    pci_write_config32(bus, slot, func, eecp, legsup);

    for (uint32_t i = 0; i < 100000; ++i) {
        uint32_t cur = pci_read_config32(bus, slot, func, eecp);
        if ((cur & (1u << 16)) == 0) {
            break;
        }
    }

    pci_write_config32(bus, slot, func, (uint8_t) (eecp + 4u), 0);
}

static int ehci_reset_controller(void) {
    uint32_t cmd = ehci_op_read32(EHCI_USBCMD);
    cmd &= ~EHCI_USBCMD_RUN;
    ehci_op_write32(EHCI_USBCMD, cmd);
    if (ehci_wait_usbsts(EHCI_USBSTS_HALTED, EHCI_USBSTS_HALTED) != 0) {
        return -1;
    }

    cmd = ehci_op_read32(EHCI_USBCMD);
    cmd |= EHCI_USBCMD_RESET;
    ehci_op_write32(EHCI_USBCMD, cmd);

    for (uint32_t i = 0; i < 200000; ++i) {
        if ((ehci_op_read32(EHCI_USBCMD) & EHCI_USBCMD_RESET) == 0) {
            return 0;
        }
    }

    return -1;
}

/* Transfer helpers (will be implemented below) */


static uint32_t phys_of(void *p) {
    return (uint32_t)(uintptr_t)p;
}

static int ehci_submit_control_sync(uint8_t addr, uint8_t *setup, uint8_t *data, uint32_t len) {
    (void) addr;
    /* Build simple chain: setup -> data (if any) -> status */
    ehci_qtd_t *q_setup = &ehci_qtds[0];
    ehci_qtd_t *q_data = &ehci_qtds[1];
    ehci_qtd_t *q_status = &ehci_qtds[2];

    usb_memset(q_setup, 0, sizeof(*q_setup));
    usb_memset(q_data, 0, sizeof(*q_data));
    usb_memset(q_status, 0, sizeof(*q_status));

    /* Setup QH for EP0 control transfer */
    usb_memset(&ehci_qh_mem, 0, sizeof(ehci_qh_mem));
    ehci_qh_mem.horiz_link = 1u;
    ehci_qh_mem.ep_char = (addr << 0) | (0u << 8) | (2u << 12) | (ehci_kbd_pkt << 16) | (1u << 28);
    ehci_qh_mem.ep_caps = (1u << 30);

    /* setup packet (8 bytes) */
    usb_memcpy(ehci_ctrl_buffer, setup, 8);
    q_setup->buffer[0] = phys_of(ehci_ctrl_buffer);
    q_setup->next_qtd = phys_of(&q_data->next_qtd);
    q_setup->alt_next_qtd = 1u;
    q_setup->token = (8u << 16) | (1u << 31) | (2u << 2);

    if (len > 0 && data) {
        /* data stage */
        usb_memcpy(ehci_ctrl_buffer + 16, data, len > 496 ? 496 : len);
        q_data->buffer[0] = phys_of(ehci_ctrl_buffer + 16);
        q_data->next_qtd = phys_of(&q_status->next_qtd);
        q_data->alt_next_qtd = 1u;
        q_data->token = (len << 16) | (1u << 31) | (1u << 30) | (1u << 2);
    } else {
        /* no data: point setup to status */
        q_setup->next_qtd = phys_of(&q_status->next_qtd);
    }

    /* status stage (zero-length) */
    q_status->next_qtd = 1u;
    q_status->alt_next_qtd = 1u;
    q_status->token = (1u << 31) | (1u << 30);

    /* link QH to setup qTD */
    ehci_qh_mem.next_qtd = phys_of(&q_setup->next_qtd);

    /* poke controller by writing ASYNCLISTADDR to QH address */
    ehci_op_write32(0x18u, phys_of(&ehci_qh_mem));

    /* wait for completion */
    for (uint32_t i = 0; i < 2000000; ++i) {
        if ((q_setup->token & (1u << 31)) == 0 && (q_status->token & (1u << 31)) == 0) {
            if (len > 0 && data) {
                /* copy returned data from buffer to user */
                usb_memcpy(data, ehci_ctrl_buffer + 16, len > 496 ? 496 : len);
            }
            return 0;
        }
        usb_delay(100);
    }

    return -1;
}

static int ehci_submit_interrupt_in_sync(uint8_t addr, uint8_t ep, uint8_t *buf, uint32_t len) {
    (void) addr;
    (void) ep;
    ehci_qtd_t *q = &ehci_qtds[3];
    usb_memset(q, 0, sizeof(*q));
    q->buffer[0] = phys_of(ehci_ctrl_buffer);
    q->next_qtd = 1u;
    q->alt_next_qtd = 1u;
    q->token = (len << 16) | (1u << 31) | (1u << 30) | (1u << 2);

    ehci_qh_mem.next_qtd = phys_of(&q->next_qtd);
    ehci_op_write32(0x18u, phys_of(&ehci_qh_mem));

    for (uint32_t i = 0; i < 2000000; ++i) {
        if ((q->token & (1u << 31)) == 0) {
            usb_memcpy(buf, ehci_ctrl_buffer, len > 496 ? 496 : len);
            return 0;
        }
        usb_delay(100);
    }
    return -1;
}

/* Redirect stubs to sync implementations for now */
static int __attribute__((unused)) ehci_submit_control(uint8_t addr, uint8_t *setup, uint8_t *data, uint32_t len) {
    return ehci_submit_control_sync(addr, setup, data, len);
}

static int ehci_submit_interrupt_in(uint8_t addr, uint8_t ep, uint8_t *buf, uint32_t len) {
    return ehci_submit_interrupt_in_sync(addr, ep, buf, len);
}

int ehci_init(uint8_t bus, uint8_t slot, uint8_t func, uint64_t mmio_base) {
    ehci_mmio = (volatile uint8_t *) (uintptr_t) mmio_base;
    ehci_cap_len = *(volatile uint8_t *) ehci_mmio;

    uint32_t hcsparams = ehci_cap_read32(0x04u);
    ehci_ports = (uint8_t) (hcsparams & 0x0Fu);

    uint32_t hccparams = ehci_cap_read32(0x08u);
    uint8_t eecp = (uint8_t) ((hccparams >> 8) & 0xFFu);
    ehci_legacy_handoff(bus, slot, func, eecp);

    if (ehci_reset_controller() != 0) {
        console_writeln("EHCI reset failed.");
        return -1;
    }

    /* Enable asynchronous schedule */
    ehci_op_write32(0x14u, 0u); /* PERIODICLISTBASE = 0 */
    ehci_op_write32(0x18u, 0u); /* ASYNCLISTADDR will be set later */

    ehci_op_write32(EHCI_CONFIGFLAG, 1u);

    uint32_t cmd = ehci_op_read32(EHCI_USBCMD);
    cmd |= EHCI_USBCMD_RUN;
    ehci_op_write32(EHCI_USBCMD, cmd);

    if (ehci_wait_usbsts(EHCI_USBSTS_HALTED, 0) != 0) {
        console_writeln("EHCI failed to start.");
        return -1;
    }

    console_write("EHCI ports: ");
    console_write_dec(ehci_ports);
    console_putc('\n');

    /* Prepare QH/qTD memory */
    usb_memset(&ehci_qh_mem, 0, sizeof(ehci_qh_mem));
    usb_memset(ehci_qtds, 0, sizeof(ehci_qtds));

    /* set horiz link to terminate */
    ehci_qh_mem.horiz_link = 1u;

    /* install async list */
    uint32_t qh_addr = (uint32_t) ((uint64_t)(uintptr_t)&ehci_qh_mem & 0xFFFFFFFFu);
    *(volatile uint32_t *) (ehci_mmio + ehci_cap_len + 0x18u) = qh_addr;
    usb_memset(ehci_ctrl_buffer, 0, sizeof(ehci_ctrl_buffer));

    /* scan ports and try enumerate devices */
    console_writeln("EHCI: Scanning ports...");
    for (uint8_t port = 1; port <= ehci_ports; ++port) {
        uint32_t portsc = ehci_op_read32(EHCI_PORTSC_BASE + (uint32_t)(port - 1) * 4u);
        console_write("EHCI port ");
        console_write_dec(port);
        console_write(" PORTSC=0x");
        console_write_hex(portsc);
        console_putc('\n');
        /* Decode common bits for easier debugging */
        console_write("  Flags:");
        if (portsc & (1u << 0)) console_write(" CCS");
        if (portsc & (1u << 1)) console_write(" PE");
        if (portsc & (1u << 2)) console_write(" PS");
        if (portsc & (1u << 3)) console_write(" POCI");
        if (portsc & (1u << 4)) console_write(" PR");
        if (portsc & (1u << 8)) console_write(" PP");
        if (portsc & (1u << 12)) console_write(" BIT12");
        if (portsc & (1u << 13)) console_write(" COMPANION_OWNER");
        console_putc('\n');
        
        if ((portsc & 0x01u) == 0) {
            continue;
        }

        console_write("EHCI device on port ");
        console_write_dec(port);
        console_putc('\n');

        /* reset port */
        console_writeln("EHCI: Resetting port...");
        ehci_op_write32(EHCI_PORTSC_BASE + (uint32_t)(port - 1) * 4u, portsc | 0x00000100u);
        usb_delay(100000);
        for (int i = 0; i < 100000; ++i) {
            uint32_t s = ehci_op_read32(EHCI_PORTSC_BASE + (uint32_t)(port - 1) * 4u);
            if ((s & 0x00000100u) == 0) break;
            usb_delay(100);
        }
        console_writeln("EHCI: Port reset complete.");
        usb_delay(100000);

        /* try to enumerate device via control transfers */
        uint8_t dev_desc[18];
        usb_memset(dev_desc, 0, sizeof(dev_desc));
        console_writeln("EHCI: Attempting GET_DEVICE_DESCRIPTOR...");
        uint8_t setup_pkt[8] = {0x80, 6, 0x00, 1, 0x00, 0x00, 18, 0x00};
        if (ehci_submit_control_sync(0, setup_pkt, dev_desc, 18) != 0) {
            console_writeln("EHCI: GET_DEVICE_DESCRIPTOR failed");
            continue;
        }
        console_writeln("EHCI: GET_DEVICE_DESCRIPTOR OK");

        /* assign address (choose 5) */
        uint8_t addr = 5;
        console_writeln("EHCI: Attempting SET_ADDRESS...");
        uint8_t set_addr_pkt[8] = {0x00, 5, addr, 0x00, 0x00, 0x00, 0x00, 0x00};
        if (ehci_submit_control_sync(0, set_addr_pkt, NULL, 0) != 0) {
            console_writeln("EHCI: SET_ADDRESS failed");
            continue;
        }
        console_writeln("EHCI: SET_ADDRESS OK");

        ehci_kbd_addr = addr;

        /* get full configuration descriptor */
        uint8_t cfg[256];
        usb_memset(cfg, 0, sizeof(cfg));
        console_writeln("EHCI: Attempting GET_CONFIG...");
        uint8_t get_cfg_pkt[8] = {0x80, 6, 0x00, 2, 0x00, 0x00, 0x00, 0x01};
        get_cfg_pkt[6] = 0x00;
        get_cfg_pkt[7] = 0x01;
        if (ehci_submit_control_sync(ehci_kbd_addr, get_cfg_pkt, cfg, 256) != 0) {
            get_cfg_pkt[6] = 64 & 0xFF;
            get_cfg_pkt[7] = (64 >> 8) & 0xFF;
            if (ehci_submit_control_sync(ehci_kbd_addr, get_cfg_pkt, cfg, 64) != 0) {
                console_writeln("EHCI: GET_CONFIG failed");
                continue;
            }
        }
        console_writeln("EHCI: GET_CONFIG OK");

        /* parse config to find HID interface and interrupt IN endpoint */
        uint16_t idx = 0;
        uint8_t found_iface = 0;
        uint8_t conf_value = 0;
        while ((size_t) idx + 2 <= sizeof(cfg)) {
            uint8_t len = cfg[idx];
            uint8_t type = cfg[idx + 1];
            if (len == 0) break;
            if (type == 2) { /* CONFIG */
                conf_value = cfg[idx + 5];
            } else if (type == 4) { /* INTERFACE */
                uint8_t iface_class = cfg[idx + 5];
                if (iface_class == 0x03) { /* HID */
                    found_iface = 1;
                }
            } else if (type == 5 && found_iface) { /* ENDPOINT */
                uint8_t epaddr = cfg[idx + 2];
                uint8_t attr = cfg[idx + 3];
                uint16_t mps = cfg[idx + 4] | (cfg[idx + 5] << 8);
                if ((attr & 0x03) == 0x03 && (epaddr & 0x80)) {
                    ehci_kbd_ep = epaddr;
                    ehci_kbd_pkt = mps ? mps : 8;
                    break;
                }
            }
            idx += len;
        }

        if (!ehci_kbd_ep) {
            console_writeln("EHCI: HID endpoint not found");
            continue;
        }
        console_write("EHCI: HID EP=0x");
        console_write_hex(ehci_kbd_ep);
        console_write(" MPS=");
        console_write_dec(ehci_kbd_pkt);
        console_putc('\n');

        /* set configuration */
        console_writeln("EHCI: Attempting SET_CONFIGURATION...");
        uint8_t set_cfg_pkt[8] = {0x00, 9, conf_value, 0x00, 0x00, 0x00, 0x00, 0x00};
        if (ehci_submit_control_sync(ehci_kbd_addr, set_cfg_pkt, NULL, 0) != 0) {
            console_writeln("EHCI: SET_CONFIG failed");
            continue;
        }
        console_writeln("EHCI: SET_CONFIGURATION OK");

        /* optional: set protocol to boot */
        console_writeln("EHCI: Attempting SET_PROTOCOL...");
        uint8_t set_proto[8] = {0x21, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        ehci_submit_control_sync(ehci_kbd_addr, set_proto, NULL, 0);
        console_writeln("EHCI: SET_PROTOCOL done.");

        ehci_kbd_ready = 1;
        console_writeln("EHCI keyboard ready.");
        return 0;
    }

    return -1;
}

int ehci_keyboard_ready(void) {
    return ehci_kbd_ready;
}

int ehci_keyboard_get_event(key_event_t *event) {
    (void) event;
    if (!ehci_kbd_ready) return 0;
    /* Try a single interrupt IN poll (placeholder) */
    if (ehci_submit_interrupt_in(ehci_kbd_addr, ehci_kbd_ep, (uint8_t *) &ehci_kbd_report, sizeof(ehci_kbd_report)) == 0) {
        key_event_t ev;
        if (usb_hid_translate(ehci_kbd_report, &ev)) {
            *event = ev;
            return 1;
        }
    }
    return 0;
}

int ehci_submit_bulk_out(uint8_t addr, uint8_t ep, const uint8_t *data, uint32_t len) {
    (void) addr;
    (void) ep;
    (void) data;
    (void) len;
    /* TODO: Implement bulk OUT (similar to control transfer) */
    return -1;
}

int ehci_submit_bulk_in(uint8_t addr, uint8_t ep, uint8_t *buf, uint32_t len) {
    (void) addr;
    (void) ep;
    (void) buf;
    (void) len;
    /* TODO: Implement bulk IN (similar to interrupt IN) */
    return -1;
}
