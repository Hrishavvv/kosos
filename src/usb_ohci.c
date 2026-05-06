#include "usb_ohci.h"

#include "console.h"
#include "pci.h"
#include "usb_hid.h"
#include "usb_utils.h"

#include <stdint.h>
#include <stddef.h>

/* OHCI Registers (all offsets from MMIO base) */
#define OHCI_HCREVISION 0x00
#define OHCI_HCCONTROL 0x04
#define OHCI_HCCOMMANDSTATUS 0x08
#define OHCI_HCINTERRUPTSTATUS 0x0C
#define OHCI_HCINTERRUPTENABLE 0x10
#define OHCI_HCINTERRUPTDISABLE 0x14
#define OHCI_HCHCCA 0x18
#define OHCI_HCPERIODCURRENTED 0x1C
#define OHCI_HCCONTROLHEADED 0x20
#define OHCI_HCCONTROLCURRENTED 0x24
#define OHCI_HCBULKHEADED 0x28
#define OHCI_HCBULKCURRENTED 0x2C
#define OHCI_HCDONEHEAD 0x30
#define OHCI_HCFMINTERVAL 0x34
#define OHCI_HCFMREMAINING 0x38
#define OHCI_HCFMNUMBER 0x3C
#define OHCI_HCPERIODICSTART 0x40
#define OHCI_HCLSTHRESHOLD 0x44
#define OHCI_HCRHDESCRIPTORA 0x48
#define OHCI_HCRHDESCRIPTORB 0x4C
#define OHCI_HCRHSTATUS 0x50
#define OHCI_HCRHPORTSTATUS(n) (0x54 + 4*(n))

#define OHCI_HCCONTROL_USB_RESET 0x00000001
#define OHCI_HCCONTROL_USB_RESUME 0x00000002
#define OHCI_HCCONTROL_USB_OPER 0x00000004
#define OHCI_HCCONTROL_USB_SUSPEND 0x00000008
#define OHCI_HCCONTROL_IR 0x00000100
#define OHCI_HCCONTROL_PLE 0x00000004
#define OHCI_HCCONTROL_IE 0x00000008

/* OHCI TD flags */
#define OHCI_TD_CC_NOERROR 0
#define OHCI_TD_PID_SETUP 0x0000u
#define OHCI_TD_PID_OUT 0x0008u
#define OHCI_TD_PID_IN 0x0010u
#define OHCI_TD_DT_DATA0 0x00000000
#define OHCI_TD_DT_DATA1 0x00200000

/* OHCI ED/TD structures */
typedef struct {
    uint32_t flags;
    uint32_t tail_td;
    uint32_t head_td;
    uint32_t next_ed;
} __attribute__((packed)) ohci_ed_t;

typedef struct {
    uint32_t flags;
    uint32_t cbp;
    uint32_t next_td;
    uint32_t be;
} __attribute__((packed)) ohci_td_t;

static volatile uint8_t *ohci_mmio = 0;
static uint8_t ohci_ports = 0;
static volatile int ohci_kbd_ready = 0;
static uint8_t ohci_kbd_ep = 0;
static uint8_t ohci_kbd_addr = 0;

/* Control transfer buffers */
static uint8_t ohci_ctrl_buffer[512] __attribute__((aligned(64)));
static ohci_ed_t ohci_ctrl_ed __attribute__((aligned(64)));
static ohci_td_t ohci_ctrl_tds[3] __attribute__((aligned(64)));
static uint8_t ohci_kbd_report[8] __attribute__((aligned(64)));
static uint16_t ohci_kbd_pkt = 8;

/* Interrupt transfer for HID keyboard */
static ohci_ed_t ohci_intr_ed __attribute__((aligned(64)));
static ohci_td_t ohci_intr_td __attribute__((aligned(64)));
static int ohci_kbd_pending = 0;

static uint32_t ohci_read32(uint32_t offset) {
    return *(volatile uint32_t *)(ohci_mmio + offset);
}

static void ohci_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(ohci_mmio + offset) = value;
}

static int ohci_wait_bits(uint32_t reg, uint32_t mask, uint32_t value) {
    for (uint32_t i = 0; i < 100000; ++i) {
        if ((ohci_read32(reg) & mask) == value) {
            return 0;
        }
        usb_delay(100);
    }
    return -1;
}

static uint32_t ohci_phys(void *p) {
    return (uint32_t)(uintptr_t)p;
}

static int ohci_submit_interrupt_in(void);  /* Forward declaration */

static int ohci_submit_control_sync(uint8_t addr, uint8_t *setup, uint8_t *data, uint32_t len) {
    /* Build ED for control endpoint 0 */
    usb_memset(&ohci_ctrl_ed, 0, sizeof(ohci_ctrl_ed));
    ohci_ctrl_ed.flags = (addr << 7) | (0 << 11) | (ohci_kbd_pkt << 16);
    ohci_ctrl_ed.tail_td = ohci_phys(&ohci_ctrl_tds[2]);
    
    /* Setup stage TD */
    usb_memset(&ohci_ctrl_tds[0], 0, sizeof(ohci_td_t));
    ohci_ctrl_tds[0].flags = (OHCI_TD_PID_SETUP << 20) | (0 << 24) | (1u << 25) | 0x0E000000;
    ohci_ctrl_tds[0].cbp = ohci_phys(ohci_ctrl_buffer);
    ohci_ctrl_tds[0].be = ohci_phys(ohci_ctrl_buffer) + 7;
    ohci_ctrl_tds[0].next_td = ohci_phys(&ohci_ctrl_tds[1]);
    usb_memcpy(ohci_ctrl_buffer, setup, 8);
    
    /* Data stage TD (if needed) */
    usb_memset(&ohci_ctrl_tds[1], 0, sizeof(ohci_td_t));
    if (len > 0 && data) {
        ohci_ctrl_tds[1].flags = (OHCI_TD_PID_IN << 20) | (1 << 24) | (1u << 25) | 0x0E000000;
        ohci_ctrl_tds[1].cbp = ohci_phys(ohci_ctrl_buffer + 16);
        ohci_ctrl_tds[1].be = ohci_phys(ohci_ctrl_buffer + 16) + (len > 496 ? 496 : len) - 1;
        usb_memcpy(ohci_ctrl_buffer + 16, data, len > 496 ? 496 : len);
    } else {
        ohci_ctrl_tds[1].flags = (OHCI_TD_PID_OUT << 20) | (1 << 24) | (1u << 25) | 0x0E000000;
    }
    ohci_ctrl_tds[1].next_td = ohci_phys(&ohci_ctrl_tds[2]);
    
    /* Status stage TD */
    usb_memset(&ohci_ctrl_tds[2], 0, sizeof(ohci_td_t));
    ohci_ctrl_tds[2].flags = ((len > 0 ? OHCI_TD_PID_OUT : OHCI_TD_PID_IN) << 20) | (1 << 24) | (1u << 25) | 0x0E000000;
    ohci_ctrl_tds[2].next_td = 0;
    
    /* Install ED on control list */
    ohci_ctrl_ed.head_td = ohci_phys(&ohci_ctrl_tds[0]);
    ohci_write32(OHCI_HCCONTROLHEADED, ohci_phys(&ohci_ctrl_ed));
    ohci_write32(OHCI_HCCOMMANDSTATUS, 0x00000001);
    
    /* Wait for completion */
    for (uint32_t i = 0; i < 2000000; ++i) {
        uint32_t flags = ohci_ctrl_tds[2].flags;
        if ((flags & (1u << 25)) == 0) {
            if (len > 0 && data) {
                usb_memcpy(data, ohci_ctrl_buffer + 16, len > 496 ? 496 : len);
            }
            return 0;
        }
        usb_delay(100);
    }
    
    return -1;
}

static int ohci_get_descriptor(uint8_t addr, uint8_t type, uint8_t index, void *buf, uint16_t len) {
    uint8_t setup[8] = {0x80, 0x06, index, type, 0, 0, len & 0xFF, (len >> 8) & 0xFF};
    return ohci_submit_control_sync(addr, setup, buf, len);
}

static int ohci_set_address(uint8_t new_addr) {
    uint8_t setup[8] = {0, 0x05, new_addr, 0, 0, 0, 0, 0};
    return ohci_submit_control_sync(0, setup, NULL, 0);
}

static int ohci_set_configuration(uint8_t addr, uint8_t config_value) {
    uint8_t setup[8] = {0, 0x09, config_value, 0, 0, 0, 0, 0};
    return ohci_submit_control_sync(addr, setup, NULL, 0);
}

static int ohci_set_protocol(uint8_t addr, uint8_t interface, uint8_t protocol) {
    uint8_t setup[8] = {0x21, 0x0B, protocol, 0, interface, 0, 0, 0};
    return ohci_submit_control_sync(addr, setup, NULL, 0);
}

static int ohci_enumerate_keyboard(void) {
    uint8_t desc[64];
    
    console_writeln("OHCI: Enumerate - GET_DEVICE_DESCRIPTOR");
    if (ohci_get_descriptor(0, 1, 0, desc, 8) != 0) {
        console_writeln("OHCI: Failed to get device descriptor");
        return -1;
    }
    
    console_writeln("OHCI: Enumerate - SET_ADDRESS");
    ohci_kbd_addr = 5;
    if (ohci_set_address(ohci_kbd_addr) != 0) {
        console_writeln("OHCI: Failed to set address");
        return -1;
    }
    usb_delay(50000);
    
    console_writeln("OHCI: Enumerate - GET_CONFIG_DESCRIPTOR");
    if (ohci_get_descriptor(ohci_kbd_addr, 2, 0, desc, 64) != 0) {
        console_writeln("OHCI: Failed to get config descriptor");
        return -1;
    }
    
    uint8_t config_value = desc[5];
    uint8_t interface_num = desc[14];
    uint8_t ep_addr = desc[19];
    (void) desc[20];  /* ep_attr */
    uint16_t mps = ((uint16_t)desc[23] << 8) | desc[22];
    
    ohci_kbd_ep = ep_addr & 0x0F;
    ohci_kbd_pkt = mps ? mps : 8;
    
    console_writeln("OHCI: Enumerate - SET_CONFIGURATION");
    if (ohci_set_configuration(ohci_kbd_addr, config_value) != 0) {
        console_writeln("OHCI: Failed to set configuration");
        return -1;
    }
    
    console_writeln("OHCI: Enumerate - SET_PROTOCOL");
    ohci_set_protocol(ohci_kbd_addr, interface_num, 0);
    
    console_writeln("OHCI keyboard enumerated successfully.");
    
    /* Start interrupt polling */
    ohci_submit_interrupt_in();
    
    return 0;
}

static int ohci_submit_interrupt_in(void) {
    if (!ohci_kbd_ready || ohci_kbd_pending) {
        return 0;
    }
    
    /* Build interrupt ED for keyboard endpoint */
    usb_memset(&ohci_intr_ed, 0, sizeof(ohci_intr_ed));
    ohci_intr_ed.flags = (ohci_kbd_addr << 7) | (ohci_kbd_ep << 11) | (ohci_kbd_pkt << 16) | (1u << 30);
    ohci_intr_ed.tail_td = ohci_phys(&ohci_intr_td) + sizeof(ohci_td_t);
    
    /* Build interrupt IN TD */
    usb_memset(&ohci_intr_td, 0, sizeof(ohci_td_t));
    ohci_intr_td.flags = (OHCI_TD_PID_IN << 20) | (0 << 24) | (1u << 25) | 0x0E000000;
    ohci_intr_td.cbp = ohci_phys(ohci_kbd_report);
    ohci_intr_td.be = ohci_phys(ohci_kbd_report) + 7;
    ohci_intr_td.next_td = 0;
    
    ohci_intr_ed.head_td = ohci_phys(&ohci_intr_td);
    
    /* Install on periodic list (simplified: just set head) */
    ohci_write32(OHCI_HCPERIODCURRENTED, ohci_phys(&ohci_intr_ed));
    
    ohci_kbd_pending = 1;
    return 0;
}

static int ohci_poll_keyboard_report(key_event_t *event) {
    if (!ohci_kbd_ready || !ohci_kbd_pending) {
        return 0;
    }
    
    /* Check if TD completed */
    if (ohci_intr_td.flags & (1u << 25)) {
        /* Still active, not done */
        return 0;
    }
    
    /* Transfer complete, parse HID report */
    ohci_kbd_pending = 0;
    int result = usb_hid_translate(ohci_kbd_report, event);
    
    /* Re-submit for next report */
    ohci_submit_interrupt_in();
    
    return result;
}

int ohci_init(uint8_t bus, uint8_t slot, uint8_t func, uint64_t mmio_base) {
    (void)bus;
    (void)slot;
    (void)func;
    ohci_mmio = (volatile uint8_t *)(uintptr_t)mmio_base;

    console_writeln("OHCI: Initializing...");

    /* Reset controller */
    console_writeln("OHCI: Resetting controller...");
    ohci_write32(OHCI_HCCOMMANDSTATUS, 0x00000001);
    if (ohci_wait_bits(OHCI_HCCOMMANDSTATUS, 0x00000001, 0) != 0) {
        console_writeln("OHCI: Reset timeout");
        return -1;
    }

    /* Set operational mode */
    uint32_t ctrl = ohci_read32(OHCI_HCCONTROL);
    ctrl &= ~0x00000003;
    ctrl |= OHCI_HCCONTROL_USB_OPER;
    ohci_write32(OHCI_HCCONTROL, ctrl);

    /* Get number of ports */
    uint32_t rhdesc = ohci_read32(OHCI_HCRHDESCRIPTORA);
    ohci_ports = (uint8_t)(rhdesc & 0xFF);

    console_write("OHCI ports: ");
    console_write_dec(ohci_ports);
    console_putc('\n');

    /* Power on all ports */
    console_writeln("OHCI: Powering ports...");
    ohci_write32(OHCI_HCRHSTATUS, 0x00010000);
    usb_delay(100000);

    /* Scan ports for devices */
    console_writeln("OHCI: Scanning ports...");
    for (uint8_t port = 1; port <= ohci_ports; ++port) {
        uint32_t portsc = ohci_read32(OHCI_HCRHPORTSTATUS(port - 1));
        console_write("OHCI port ");
        console_write_dec(port);
        console_write(" PORTSC=0x");
        console_write_hex(portsc);
        console_putc('\n');

        /* Check if device connected (bit 0 = connection status) */
        if ((portsc & 0x01) == 0) {
            continue;
        }

        console_write("OHCI device on port ");
        console_write_dec(port);
        console_putc('\n');

        /* Reset port (set bit 4) */
        console_writeln("OHCI: Resetting port...");
        ohci_write32(OHCI_HCRHPORTSTATUS(port - 1), 0x00000010);
        usb_delay(100000);
        for (int i = 0; i < 100000; ++i) {
            uint32_t s = ohci_read32(OHCI_HCRHPORTSTATUS(port - 1));
            if ((s & 0x00000010) == 0) break;
            usb_delay(100);
        }
        console_writeln("OHCI: Port reset complete.");
        usb_delay(100000);

        /* Attempt enumeration */
        if (ohci_enumerate_keyboard() == 0) {
            ohci_kbd_ready = 1;
            return 0;
        }
    }

    return -1;
}

int ohci_keyboard_ready(void) {
    return ohci_kbd_ready;
}

int ohci_keyboard_get_event(key_event_t *event) {
    if (!ohci_kbd_ready) {
        return 0;
    }
    
    /* Try polling for a pending report */
    if (ohci_poll_keyboard_report(event)) {
        return 1;
    }
    
    /* Submit initial interrupt request if not pending */
    if (!ohci_kbd_pending) {
        ohci_submit_interrupt_in();
    }
    
    return 0;
}
