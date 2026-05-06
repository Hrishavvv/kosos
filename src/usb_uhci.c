#include "usb_uhci.h"

#include "console.h"
#include "pci.h"
#include "usb_hid.h"
#include "usb_utils.h"

/* UHCI registers (I/O port based) */
#define UHCI_USBCMD     0x00
#define UHCI_USBSTS     0x02
#define UHCI_USBINTR    0x04
#define UHCI_FRNUM      0x06
#define UHCI_FLBASEADD  0x08
#define UHCI_SOFMOD     0x0C
#define UHCI_PORTSC1    0x10
#define UHCI_PORTSC2    0x12

/* USB request types and codes */
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_SET_IDLE          0x0A

/* TD token PID codes */
#define UHCI_PID_SETUP  0xD0
#define UHCI_PID_IN     0x69
#define UHCI_PID_OUT    0xE1

typedef struct {
    uint32_t link;      /* Next QH/TD link */
    uint32_t reserved;
    uint32_t td_head;   /* First TD in queue */
    uint32_t td_tail;   /* Next TD to execute */
} uhci_qh_t;

typedef struct {
    uint32_t link;      /* Next TD link */
    uint32_t status;    /* Status and control */
    uint32_t token;     /* PID, address, endpoint, data toggle, length */
    uint32_t buffer;    /* Data buffer pointer */
} uhci_td_t;

static volatile uint16_t *uhci_iobase = NULL;
static volatile int uhci_kbd_ready = 0;
static volatile int uhci_kbd_pending = 0;
static uint8_t uhci_kbd_report[8] __attribute__((unused));
static uint8_t uhci_kbd_addr = 0;
static uint8_t uhci_kbd_ep = 0;
static uint16_t uhci_kbd_pkt = 8;

static uhci_qh_t uhci_qh_pool[16] __attribute__((aligned(16)));
static uhci_td_t uhci_td_pool[64] __attribute__((aligned(32)));
static uint32_t uhci_frame_list[1024] __attribute__((aligned(4096)));

static uint32_t uhci_phys(void *p) {
    return (uint32_t)(uintptr_t)p;
}

static void uhci_write_io(uint16_t reg, uint16_t val) {
    *(volatile uint16_t *)(uhci_iobase + reg / 2) = val;
}

static uint16_t uhci_read_io(uint16_t reg) {
    return *(volatile uint16_t *)(uhci_iobase + reg / 2);
}

static void uhci_reset(void) {
    uhci_write_io(UHCI_USBCMD, 0x0004);  /* Global reset */
    usb_delay(100000);
    uhci_write_io(UHCI_USBCMD, 0x0000);
    usb_delay(100000);
}

static void uhci_init_frame_list(void) {
    usb_memset(uhci_frame_list, 0, sizeof(uhci_frame_list));
    
    /* Link frame list to QH pool */
    for (int i = 0; i < 1024; ++i) {
        uhci_frame_list[i] = uhci_phys(&uhci_qh_pool[0]);
    }
    
    uhci_write_io(UHCI_FLBASEADD, uhci_phys(uhci_frame_list));
}

static int uhci_submit_control(uint8_t addr, uint8_t endpoint, uint8_t *setup, 
                               uint8_t *data, uint32_t len, int in) {
    static uint8_t ctrl_buffer[512] __attribute__((aligned(64)));
    
    /* Allocate QH and TDs */
    uhci_qh_t *qh = &uhci_qh_pool[1];
    uhci_td_t *td_setup = &uhci_td_pool[0];
    uhci_td_t *td_data = &uhci_td_pool[1];
    uhci_td_t *td_status = &uhci_td_pool[2];
    
    usb_memset(qh, 0, sizeof(*qh));
    usb_memset(td_setup, 0, sizeof(*td_setup));
    usb_memset(td_data, 0, sizeof(*td_data));
    usb_memset(td_status, 0, sizeof(*td_status));
    
    /* Copy setup packet */
    usb_memcpy(ctrl_buffer, setup, 8);
    
    /* SETUP stage */
    td_setup->token = (8 << 21) | (endpoint << 15) | (addr << 8) | UHCI_PID_SETUP;
    td_setup->buffer = uhci_phys(ctrl_buffer);
    td_setup->link = uhci_phys(td_data);
    td_setup->status = (1u << 31) | (1u << 23);  /* Active, error count=3 */
    
    /* DATA stage */
    if (len > 0) {
        if (data) usb_memcpy(ctrl_buffer + 16, data, len > 496 ? 496 : len);
        uint8_t pid = in ? UHCI_PID_IN : UHCI_PID_OUT;
        td_data->token = (len << 21) | (endpoint << 15) | (addr << 8) | pid | (1 << 19);
        td_data->buffer = uhci_phys(ctrl_buffer + 16);
        td_data->link = uhci_phys(td_status);
        td_data->status = (1u << 31) | (1u << 23);
    } else {
        td_setup->link = uhci_phys(td_status);
    }
    
    /* STATUS stage (opposite direction, zero length) */
    uint8_t status_pid = in ? UHCI_PID_OUT : UHCI_PID_IN;
    td_status->token = (0 << 21) | (endpoint << 15) | (addr << 8) | status_pid;
    td_status->buffer = 0;
    td_status->link = 1;  /* Terminate */
    td_status->status = (1u << 31) | (1u << 23);
    
    /* Link QH to frame list */
    qh->td_head = uhci_phys(td_setup);
    qh->link = 1;
    uhci_frame_list[0] = uhci_phys(qh);
    
    /* Wait for completion */
    for (int i = 0; i < 10000000; ++i) {
        if ((td_setup->status & (1u << 31)) == 0 && (td_status->status & (1u << 31)) == 0) {
            if (in && len > 0 && data) {
                usb_memcpy(data, ctrl_buffer + 16, len > 496 ? 496 : len);
            }
            return 0;
        }
        usb_delay(10);
    }
    
    console_writeln("UHCI: Control transfer timeout");
    return -1;
}

static int uhci_get_descriptor(uint8_t addr, uint8_t type, uint16_t index, 
                                uint8_t *buf, uint16_t len) {
    uint8_t setup[8] = {
        0x80,           /* bmRequestType: device-to-host */
        USB_REQ_GET_DESCRIPTOR,
        (uint8_t)index, (uint8_t)(index >> 8) | (type << 4),
        0x00,           /* wIndex */
        0x00,
        (uint8_t)len,   /* wLength */
        (uint8_t)(len >> 8)
    };
    return uhci_submit_control(addr, 0, setup, buf, len, 1);
}

static int uhci_set_address(uint8_t addr) {
    uint8_t setup[8] = {
        0x00,           /* bmRequestType */
        USB_REQ_SET_ADDRESS,
        addr,           /* wValue = new address */
        0x00,
        0x00,           /* wIndex */
        0x00,
        0x00,           /* wLength = 0 */
        0x00
    };
    return uhci_submit_control(0, 0, setup, NULL, 0, 0);
}

static int uhci_set_configuration(uint8_t addr, uint8_t cfg) {
    uint8_t setup[8] = {
        0x00,           /* bmRequestType */
        USB_REQ_SET_CONFIGURATION,
        cfg,            /* wValue = config value */
        0x00,
        0x00,           /* wIndex */
        0x00,
        0x00,           /* wLength = 0 */
        0x00
    };
    return uhci_submit_control(addr, 0, setup, NULL, 0, 0);
}

static int uhci_set_idle(uint8_t addr, uint8_t iface, uint8_t duration) {
    uint8_t setup[8] = {
        0x21,           /* bmRequestType: class, interface */
        USB_REQ_SET_IDLE,
        duration,       /* wValue = idle duration */
        0x00,
        iface,          /* wIndex = interface */
        0x00,
        0x00,           /* wLength = 0 */
        0x00
    };
    return uhci_submit_control(addr, 0, setup, NULL, 0, 0);
}

static int uhci_enumerate_keyboard(void) {
    console_writeln("UHCI: Scanning for keyboard...");
    
    /* Check port 1 */
    uint16_t portsc = uhci_read_io(UHCI_PORTSC1);
    console_write("UHCI port 1 status: 0x");
    console_write_hex(portsc);
    console_putc('\n');
    
    if ((portsc & 0x0001) == 0) {
        console_writeln("UHCI: No device on port 1");
        return -1;
    }
    
    console_writeln("UHCI: Device detected, resetting port...");
    uhci_write_io(UHCI_PORTSC1, portsc | 0x0200);  /* Reset */
    usb_delay(100000);
    uhci_write_io(UHCI_PORTSC1, portsc & ~0x0200);
    usb_delay(100000);
    
    console_writeln("UHCI: GET_DEVICE_DESCRIPTOR");
    uint8_t dev_desc[18];
    usb_memset(dev_desc, 0, sizeof(dev_desc));
    if (uhci_get_descriptor(0, 1, 0, dev_desc, 18) != 0) {
        console_writeln("UHCI: GET_DEVICE_DESCRIPTOR failed");
        return -1;
    }
    console_writeln("UHCI: GET_DEVICE_DESCRIPTOR OK");
    
    console_writeln("UHCI: SET_ADDRESS(5)");
    uhci_kbd_addr = 5;
    if (uhci_set_address(uhci_kbd_addr) != 0) {
        console_writeln("UHCI: SET_ADDRESS failed");
        return -1;
    }
    console_writeln("UHCI: SET_ADDRESS OK");
    
    console_writeln("UHCI: GET_CONFIG_DESCRIPTOR");
    uint8_t cfg[256];
    usb_memset(cfg, 0, sizeof(cfg));
    if (uhci_get_descriptor(uhci_kbd_addr, 2, 0, cfg, 256) != 0) {
        console_writeln("UHCI: GET_CONFIG_DESCRIPTOR failed");
        return -1;
    }
    console_writeln("UHCI: GET_CONFIG_DESCRIPTOR OK");
    
    /* Parse config to find HID interface and interrupt IN endpoint */
    uint16_t idx = 0;
    uint8_t found_iface = 0;
    uint8_t conf_value = 0;
    uint8_t interface_num = 0;
    
    while ((size_t)idx + 2 <= sizeof(cfg)) {
        uint8_t len = cfg[idx];
        uint8_t type = cfg[idx + 1];
        if (len == 0) break;
        
        if (type == 2) {  /* CONFIG */
            conf_value = cfg[idx + 5];
        } else if (type == 4) {  /* INTERFACE */
            uint8_t iface_class = cfg[idx + 5];
            interface_num = cfg[idx + 2];
            if (iface_class == 0x03) {  /* HID */
                found_iface = 1;
            }
        } else if (type == 5 && found_iface) {  /* ENDPOINT */
            uint8_t epaddr = cfg[idx + 2];
            uint8_t attr = cfg[idx + 3];
            uint16_t mps = cfg[idx + 4] | (cfg[idx + 5] << 8);
            
            if ((attr & 0x03) == 0x03 && (epaddr & 0x80)) {  /* Interrupt IN */
                uhci_kbd_ep = epaddr & 0x0F;
                uhci_kbd_pkt = mps ? mps : 8;
                found_iface = 2;  /* Mark as found */
                break;
            }
        }
        
        idx += len;
    }
    
    if (found_iface != 2) {
        console_writeln("UHCI: No HID interrupt endpoint found");
        return -1;
    }
    
    console_writeln("UHCI: SET_CONFIGURATION");
    if (uhci_set_configuration(uhci_kbd_addr, conf_value) != 0) {
        console_writeln("UHCI: SET_CONFIGURATION failed");
        return -1;
    }
    console_writeln("UHCI: SET_CONFIGURATION OK");
    
    console_writeln("UHCI: SET_IDLE");
    uhci_set_idle(uhci_kbd_addr, interface_num, 0);
    
    console_writeln("UHCI keyboard enumerated successfully.");
    uhci_kbd_ready = 1;
    return 0;
}

int uhci_init(uint8_t bus, uint8_t slot, uint8_t func, uint64_t mmio_base) {
    (void)bus;
    (void)slot;
    (void)func;
    
    console_writeln("UHCI: Initializing...");
    
    uhci_iobase = (volatile uint16_t *)(uintptr_t)mmio_base;
    
    uhci_reset();
    
    /* Set up frame list */
    uhci_init_frame_list();
    
    /* Enable controller */
    uhci_write_io(UHCI_USBCMD, 0x0001);  /* Run */
    usb_delay(100000);
    
    console_write("UHCI port status: ");
    console_write_hex(uhci_read_io(UHCI_PORTSC1));
    console_putc('\n');
    
    /* Try to enumerate keyboard */
    if (uhci_enumerate_keyboard() == 0) {
        return 0;
    }
    
    console_writeln("UHCI: No keyboard found");
    return -1;
}

int uhci_keyboard_ready(void) {
    return uhci_kbd_ready;
}

int uhci_keyboard_get_event(key_event_t *event) {
    if (!uhci_kbd_ready) {
        return 0;
    }
    
    /* TODO: Implement interrupt transfer polling */
    (void)event;
    return 0;
}
