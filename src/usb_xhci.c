#include "usb_xhci.h"

#include "console.h"
#include "usb_hid.h"
#include "usb_utils.h"

#include <stddef.h>
#include <stdint.h>

#define XHCI_USBCMD 0x00u
#define XHCI_USBSTS 0x04u
#define XHCI_CRCR 0x18u
#define XHCI_DCBAAP 0x30u
#define XHCI_CONFIG 0x38u

#define XHCI_USBCMD_RUN 0x00000001u
#define XHCI_USBCMD_RESET 0x00000002u

#define XHCI_USBSTS_HALTED 0x00000001u

#define XHCI_DBOFF 0x14u
#define XHCI_RTSOFF 0x18u

#define XHCI_PORTSC_BASE 0x400u

#define XHCI_PORTSC_CCS 0x00000001u
#define XHCI_PORTSC_PED 0x00000002u
#define XHCI_PORTSC_PR 0x00000010u
#define XHCI_PORTSC_PP 0x00000200u
#define XHCI_PORTSC_SPEED_MASK 0x00003C00u
#define XHCI_PORTSC_SPEED_SHIFT 10

#define XHCI_TRB_TYPE_SHIFT 10
#define XHCI_TRB_CYCLE 0x00000001u
#define XHCI_TRB_TC 0x00000002u
#define XHCI_TRB_CHAIN 0x00000010u
#define XHCI_TRB_IOC 0x00000020u
#define XHCI_TRB_IDT 0x00000040u
#define XHCI_TRB_DIR_IN 0x00010000u
#define XHCI_TRB_TRT_SHIFT 16

#define XHCI_TRB_TYPE_NORMAL 1u
#define XHCI_TRB_TYPE_SETUP_STAGE 2u
#define XHCI_TRB_TYPE_DATA_STAGE 3u
#define XHCI_TRB_TYPE_STATUS_STAGE 4u
#define XHCI_TRB_TYPE_LINK 6u
#define XHCI_TRB_TYPE_ENABLE_SLOT 9u
#define XHCI_TRB_TYPE_ADDRESS_DEVICE 11u
#define XHCI_TRB_TYPE_CONFIGURE_ENDPOINT 12u
#define XHCI_TRB_TYPE_CMD_COMPLETION 33u
#define XHCI_TRB_TYPE_TRANSFER_EVENT 32u
#define XHCI_TRB_TYPE_PORT_STATUS_EVENT 34u

#define XHCI_COMPLETION_SUCCESS 1u

#define XHCI_CMD_RING_SIZE 64u
#define XHCI_EVT_RING_SIZE 64u
#define XHCI_CTRL_RING_SIZE 32u
#define XHCI_INTR_RING_SIZE 32u

#define XHCI_CTX_STRIDE 64u
#define XHCI_CTX_COUNT 33u

#define XHCI_SCRATCHPAD_MAX 1024u

#define USB_DESC_DEVICE 1u
#define USB_DESC_CONFIGURATION 2u
#define USB_DESC_INTERFACE 4u
#define USB_DESC_ENDPOINT 5u

#define USB_REQ_GET_DESCRIPTOR 6u
#define USB_REQ_SET_CONFIGURATION 9u

#define USB_REQ_SET_IDLE 0x0Au
#define USB_REQ_SET_PROTOCOL 0x0Bu

#define USB_CLASS_HID 0x03u
#define USB_SUBCLASS_BOOT 0x01u
#define USB_PROTOCOL_KEYBOARD 0x01u

typedef struct {
    uint32_t dword0;
    uint32_t dword1;
    uint32_t dword2;
    uint32_t dword3;
} xhci_trb_t;

typedef struct {
    uint64_t addr;
    uint32_t size;
    uint32_t reserved;
} xhci_erst_t;

typedef struct {
    xhci_trb_t *trbs;
    uint32_t size;
    uint32_t enqueue;
    uint8_t cycle;
} xhci_ring_t;

typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_packet_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
} __attribute__((packed)) usb_desc_header_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed)) usb_interface_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed)) usb_endpoint_desc_t;

static volatile uint8_t *xhci_mmio = 0;
static volatile uint32_t *xhci_doorbells = 0;
static volatile uint8_t *xhci_runtime = 0;
static uint32_t xhci_dboff = 0;
static uint32_t xhci_rtsoff = 0;
static uint8_t xhci_cap_len = 0;
static uint8_t xhci_max_ports = 0;
static uint8_t xhci_max_slots = 0;
static uint8_t xhci_ctx_size = 32;

static xhci_trb_t xhci_cmd_ring_mem[XHCI_CMD_RING_SIZE] __attribute__((aligned(64)));
static volatile xhci_trb_t xhci_evt_ring_mem[XHCI_EVT_RING_SIZE] __attribute__((aligned(64)));
static xhci_trb_t xhci_ctrl_ring_mem[XHCI_CTRL_RING_SIZE] __attribute__((aligned(64)));
static xhci_trb_t xhci_intr_ring_mem[XHCI_INTR_RING_SIZE] __attribute__((aligned(64)));
static xhci_erst_t xhci_erst_mem[1] __attribute__((aligned(64)));

static uint64_t xhci_dcbaa[256] __attribute__((aligned(64)));
static uint64_t xhci_scratchpad_ptrs[XHCI_SCRATCHPAD_MAX] __attribute__((aligned(64)));
static uint8_t xhci_scratchpad_bufs[XHCI_SCRATCHPAD_MAX][4096] __attribute__((aligned(4096)));

static uint8_t xhci_input_ctx[XHCI_CTX_STRIDE * XHCI_CTX_COUNT] __attribute__((aligned(64)));
static uint8_t xhci_device_ctx[XHCI_CTX_STRIDE * XHCI_CTX_COUNT] __attribute__((aligned(64)));

static xhci_ring_t xhci_cmd_ring = {0};
static xhci_ring_t xhci_evt_ring = {0};
static xhci_ring_t xhci_ctrl_ring = {0};
static xhci_ring_t xhci_intr_ring = {0};

static uint8_t xhci_kbd_ready = 0;
static uint8_t xhci_kbd_slot = 0;
static uint8_t xhci_kbd_ep_id = 0;
static uint16_t xhci_kbd_report_len = 8;
static uint8_t xhci_kbd_report[8];
static int xhci_kbd_pending = 0;

static uint32_t xhci_cap_read32(uint32_t offset) {
    return *(volatile uint32_t *) (xhci_mmio + offset);
}

static uint32_t xhci_op_read32(uint32_t offset) {
    return *(volatile uint32_t *) (xhci_mmio + xhci_cap_len + offset);
}

static void xhci_op_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *) (xhci_mmio + xhci_cap_len + offset) = value;
}

static void xhci_rt_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *) (xhci_runtime + offset) = value;
}

static uint64_t xhci_phys(const volatile void *ptr) {
    return (uint64_t) (uintptr_t) ptr;
}

static int xhci_wait_usbsts(uint32_t mask, uint32_t value) {
    for (uint32_t i = 0; i < 200000; ++i) {
        if ((xhci_op_read32(XHCI_USBSTS) & mask) == value) {
            return 0;
        }
    }
    return -1;
}

static void xhci_legacy_handoff(void) {
    uint32_t hccparams1 = xhci_cap_read32(0x10u);
    uint32_t ext_off = (hccparams1 >> 16) & 0xFFFFu;
    while (ext_off) {
        uint32_t cap = xhci_cap_read32(ext_off * 4u);
        uint8_t cap_id = (uint8_t) (cap & 0xFFu);
        uint8_t next = (uint8_t) ((cap >> 8) & 0xFFu);
        if (cap_id == 1u) {
            uint32_t legsup = xhci_cap_read32(ext_off * 4u);
            legsup |= (1u << 24);
            *(volatile uint32_t *) (xhci_mmio + ext_off * 4u) = legsup;

            for (uint32_t i = 0; i < 100000; ++i) {
                uint32_t cur = xhci_cap_read32(ext_off * 4u);
                if ((cur & (1u << 16)) == 0) {
                    break;
                }
            }

            *(volatile uint32_t *) (xhci_mmio + ext_off * 4u + 4u) = 0;
            break;
        }
        ext_off = next;
    }
}

static int xhci_reset_controller(void) {
    uint32_t cmd = xhci_op_read32(XHCI_USBCMD);
    cmd &= ~XHCI_USBCMD_RUN;
    xhci_op_write32(XHCI_USBCMD, cmd);
    if (xhci_wait_usbsts(XHCI_USBSTS_HALTED, XHCI_USBSTS_HALTED) != 0) {
        return -1;
    }

    cmd = xhci_op_read32(XHCI_USBCMD);
    cmd |= XHCI_USBCMD_RESET;
    xhci_op_write32(XHCI_USBCMD, cmd);

    for (uint32_t i = 0; i < 200000; ++i) {
        if ((xhci_op_read32(XHCI_USBCMD) & XHCI_USBCMD_RESET) == 0) {
            return 0;
        }
    }

    return -1;
}

static void xhci_ring_init(xhci_ring_t *ring, xhci_trb_t *trbs, uint32_t size) {
    usb_memset(trbs, 0, sizeof(xhci_trb_t) * size);
    ring->trbs = trbs;
    ring->size = size;
    ring->enqueue = 0;
    ring->cycle = 1;

    uint64_t link_addr = xhci_phys(trbs);
    trbs[size - 1].dword0 = (uint32_t) (link_addr & 0xFFFFFFFFu);
    trbs[size - 1].dword1 = (uint32_t) (link_addr >> 32);
    trbs[size - 1].dword2 = 0;
    trbs[size - 1].dword3 = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC | ring->cycle;
}

static void xhci_event_ring_init(xhci_ring_t *ring, volatile xhci_trb_t *trbs, uint32_t size) {
    usb_memset((void *) trbs, 0, sizeof(xhci_trb_t) * size);
    ring->trbs = (xhci_trb_t *) trbs;
    ring->size = size;
    ring->enqueue = 0;
    ring->cycle = 1;
}

static void xhci_ring_enqueue(xhci_ring_t *ring, const xhci_trb_t *trb) {
    xhci_trb_t *dst = &ring->trbs[ring->enqueue];
    *dst = *trb;
    dst->dword3 |= ring->cycle;

    ring->enqueue++;
    if (ring->enqueue == ring->size - 1) {
        ring->trbs[ring->size - 1].dword3 =
            (ring->trbs[ring->size - 1].dword3 & ~XHCI_TRB_CYCLE) | ring->cycle;
        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
}

static void xhci_update_erdp(void) {
    uint64_t addr = xhci_phys(&xhci_evt_ring.trbs[xhci_evt_ring.enqueue]);
    volatile uint64_t *erdp = (volatile uint64_t *) (xhci_runtime + 0x38u);
    *erdp = addr | (1ull << 3);
}

static int xhci_poll_event(xhci_trb_t *out) {
    volatile xhci_trb_t *trb = (volatile xhci_trb_t *) &xhci_evt_ring.trbs[xhci_evt_ring.enqueue];
    if ((trb->dword3 & XHCI_TRB_CYCLE) != xhci_evt_ring.cycle) {
        return 0;
    }

    *out = *(const xhci_trb_t *) trb;
    xhci_evt_ring.enqueue++;
    if (xhci_evt_ring.enqueue == xhci_evt_ring.size) {
        xhci_evt_ring.enqueue = 0;
        xhci_evt_ring.cycle ^= 1;
    }
    xhci_update_erdp();
    return 1;
}

static int xhci_wait_event(uint8_t type, xhci_trb_t *out) {
    for (uint32_t i = 0; i < 500000; ++i) {
        xhci_trb_t trb;
        if (!xhci_poll_event(&trb)) {
            continue;
        }
        uint8_t trb_type = (uint8_t) ((trb.dword3 >> XHCI_TRB_TYPE_SHIFT) & 0x3Fu);
        if (trb_type == type) {
            if (out) {
                *out = trb;
            }
            return 0;
        }
    }
    return -1;
}

static uint8_t xhci_completion_code(const xhci_trb_t *trb) {
    return (uint8_t) ((trb->dword2 >> 24) & 0xFFu);
}

static uint8_t xhci_event_slot_id(const xhci_trb_t *trb) {
    return (uint8_t) ((trb->dword3 >> 24) & 0xFFu);
}

static int xhci_cmd_enable_slot(void) {
    xhci_trb_t trb = {0};
    trb.dword3 = (XHCI_TRB_TYPE_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT);
    xhci_ring_enqueue(&xhci_cmd_ring, &trb);
    xhci_doorbells[0] = 0;

    xhci_trb_t event;
    if (xhci_wait_event(XHCI_TRB_TYPE_CMD_COMPLETION, &event) != 0) {
        return -1;
    }
    if (xhci_completion_code(&event) != XHCI_COMPLETION_SUCCESS) {
        return -1;
    }
    return (int) xhci_event_slot_id(&event);
}

static uint32_t *xhci_ctx_ptr(uint8_t *base, uint32_t index) {
    return (uint32_t *) (base + index * xhci_ctx_size);
}

static void xhci_setup_slot_context(uint8_t port, uint8_t speed, uint8_t context_entries) {
    uint32_t *slot_ctx = xhci_ctx_ptr(xhci_input_ctx, 1);
    slot_ctx[0] = ((uint32_t) context_entries << 27) | ((uint32_t) speed << 20);
    slot_ctx[1] = ((uint32_t) port << 16);
    slot_ctx[2] = 0;
    slot_ctx[3] = 0;
}

static void xhci_setup_ep_context(uint32_t *ep_ctx, uint8_t ep_type, uint16_t mps, uint8_t interval,
                                 uint64_t dequeue, uint8_t dcs) {
    ep_ctx[0] = ((uint32_t) interval << 16);
    ep_ctx[1] = (3u << 1) | ((uint32_t) ep_type << 3) | ((uint32_t) mps << 16);
    ep_ctx[2] = (uint32_t) (dequeue & 0xFFFFFFFFu);
    ep_ctx[3] = (uint32_t) (dequeue >> 32);
    ep_ctx[3] |= dcs ? 1u : 0u;
    ep_ctx[4] = mps;
}

static uint16_t xhci_ep0_mps_from_speed(uint8_t speed) {
    switch (speed) {
        case 3u:
            return 64u;
        case 4u:
        case 5u:
            return 512u;
        default:
            return 8u;
    }
}

static uint8_t xhci_keyboard_interval(uint8_t b_interval) {
    (void) b_interval;
    return 1u;
}

static int xhci_cmd_address_device(uint8_t slot_id, uint8_t port, uint8_t speed, uint16_t ep0_mps) {
    usb_memset(xhci_input_ctx, 0, sizeof(xhci_input_ctx));
    usb_memset(xhci_device_ctx, 0, sizeof(xhci_device_ctx));

    xhci_dcbaa[slot_id] = xhci_phys(xhci_device_ctx);

    uint32_t *ctrl = xhci_ctx_ptr(xhci_input_ctx, 0);
    ctrl[0] = 0;
    ctrl[1] = (1u << 0) | (1u << 1);

    xhci_setup_slot_context(port, speed, 1);

    xhci_ring_init(&xhci_ctrl_ring, xhci_ctrl_ring_mem, XHCI_CTRL_RING_SIZE);
    uint64_t ctrl_ring_addr = xhci_phys(xhci_ctrl_ring_mem);
    uint32_t *ep0_ctx = xhci_ctx_ptr(xhci_input_ctx, 2);
    xhci_setup_ep_context(ep0_ctx, 4u, ep0_mps, 0, ctrl_ring_addr, xhci_ctrl_ring.cycle);

    xhci_trb_t trb = {0};
    uint64_t addr = xhci_phys(xhci_input_ctx);
    trb.dword0 = (uint32_t) (addr & 0xFFFFFFFFu);
    trb.dword1 = (uint32_t) (addr >> 32);
    trb.dword3 = ((uint32_t) slot_id << 24) | (XHCI_TRB_TYPE_ADDRESS_DEVICE << XHCI_TRB_TYPE_SHIFT);

    xhci_ring_enqueue(&xhci_cmd_ring, &trb);
    xhci_doorbells[0] = 0;

    xhci_trb_t event;
    if (xhci_wait_event(XHCI_TRB_TYPE_CMD_COMPLETION, &event) != 0) {
        return -1;
    }
    if (xhci_completion_code(&event) != XHCI_COMPLETION_SUCCESS) {
        return -1;
    }
    return 0;
}

static int xhci_cmd_configure_endpoint(uint8_t slot_id) {
    xhci_trb_t trb = {0};
    uint64_t addr = xhci_phys(xhci_input_ctx);
    trb.dword0 = (uint32_t) (addr & 0xFFFFFFFFu);
    trb.dword1 = (uint32_t) (addr >> 32);
    trb.dword3 = ((uint32_t) slot_id << 24) | (XHCI_TRB_TYPE_CONFIGURE_ENDPOINT << XHCI_TRB_TYPE_SHIFT);

    xhci_ring_enqueue(&xhci_cmd_ring, &trb);
    xhci_doorbells[0] = 0;

    xhci_trb_t event;
    if (xhci_wait_event(XHCI_TRB_TYPE_CMD_COMPLETION, &event) != 0) {
        return -1;
    }
    if (xhci_completion_code(&event) != XHCI_COMPLETION_SUCCESS) {
        return -1;
    }
    return 0;
}

static int xhci_reset_port(uint8_t port, uint8_t *speed_out) {
    uint32_t portsc_offset = XHCI_PORTSC_BASE + (uint32_t) (port - 1) * 0x10u;
    uint32_t portsc = xhci_op_read32(portsc_offset);
    if ((portsc & XHCI_PORTSC_CCS) == 0) {
        return -1;
    }

    if ((portsc & XHCI_PORTSC_PP) == 0) {
        xhci_op_write32(portsc_offset, portsc | XHCI_PORTSC_PP);
        usb_delay(1000);
    }

    portsc = xhci_op_read32(portsc_offset);
    xhci_op_write32(portsc_offset, portsc | XHCI_PORTSC_PR);

    for (uint32_t i = 0; i < 200000; ++i) {
        portsc = xhci_op_read32(portsc_offset);
        if ((portsc & XHCI_PORTSC_PR) == 0) {
            break;
        }
    }

    portsc = xhci_op_read32(portsc_offset);
    if ((portsc & XHCI_PORTSC_PED) == 0) {
        return -1;
    }

    if (speed_out) {
        *speed_out = (uint8_t) ((portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT);
    }
    return 0;
}

static int xhci_control_transfer(const usb_setup_packet_t *setup, void *data, uint16_t length) {
    uint8_t dir_in = (setup->bmRequestType & 0x80u) != 0;
    uint8_t trt = 0;
    if (length > 0) {
        trt = dir_in ? 3u : 2u;
    }

    xhci_trb_t setup_trb = {0};
    setup_trb.dword0 = (uint32_t) setup->bmRequestType | ((uint32_t) setup->bRequest << 8)
        | ((uint32_t) setup->wValue << 16);
    setup_trb.dword1 = (uint32_t) setup->wIndex | ((uint32_t) setup->wLength << 16);
    setup_trb.dword2 = 8;
    setup_trb.dword3 = (XHCI_TRB_TYPE_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT)
        | XHCI_TRB_IDT | XHCI_TRB_CHAIN | ((uint32_t) trt << XHCI_TRB_TRT_SHIFT);

    xhci_ring_enqueue(&xhci_ctrl_ring, &setup_trb);

    if (length > 0) {
        xhci_trb_t data_trb = {0};
        uint64_t addr = xhci_phys(data);
        data_trb.dword0 = (uint32_t) (addr & 0xFFFFFFFFu);
        data_trb.dword1 = (uint32_t) (addr >> 32);
        data_trb.dword2 = length;
        data_trb.dword3 = (XHCI_TRB_TYPE_DATA_STAGE << XHCI_TRB_TYPE_SHIFT)
            | XHCI_TRB_CHAIN | (dir_in ? XHCI_TRB_DIR_IN : 0);
        xhci_ring_enqueue(&xhci_ctrl_ring, &data_trb);
    }

    xhci_trb_t status_trb = {0};
    status_trb.dword3 = (XHCI_TRB_TYPE_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT)
        | XHCI_TRB_IOC | (dir_in ? 0 : XHCI_TRB_DIR_IN);
    xhci_ring_enqueue(&xhci_ctrl_ring, &status_trb);

    xhci_doorbells[xhci_kbd_slot] = 1;

    xhci_trb_t event;
    if (xhci_wait_event(XHCI_TRB_TYPE_TRANSFER_EVENT, &event) != 0) {
        return -1;
    }
    if (xhci_completion_code(&event) != XHCI_COMPLETION_SUCCESS) {
        return -1;
    }
    return 0;
}

static int xhci_get_descriptor(uint8_t type, uint8_t index, void *buffer, uint16_t length) {
    usb_setup_packet_t setup = {0x80u, USB_REQ_GET_DESCRIPTOR,
        (uint16_t) ((uint16_t) type << 8) | index, 0, length};
    return xhci_control_transfer(&setup, buffer, length);
}

static int xhci_set_configuration(uint8_t config_value) {
    usb_setup_packet_t setup = {0x00u, USB_REQ_SET_CONFIGURATION,
        config_value, 0, 0};
    return xhci_control_transfer(&setup, NULL, 0);
}

static int xhci_set_protocol(uint8_t interface_num) {
    usb_setup_packet_t setup = {0x21u, USB_REQ_SET_PROTOCOL,
        0, interface_num, 0};
    return xhci_control_transfer(&setup, NULL, 0);
}

static int xhci_set_idle(uint8_t interface_num) {
    usb_setup_packet_t setup = {0x21u, USB_REQ_SET_IDLE,
        0, interface_num, 0};
    return xhci_control_transfer(&setup, NULL, 0);
}

static int xhci_parse_hid_keyboard(uint8_t *buffer, uint16_t total_len,
                                   uint8_t *config_value, uint8_t *interface_num,
                                   uint8_t *ep_addr, uint16_t *ep_mps, uint8_t *ep_interval) {
    uint16_t offset = 0;
    uint8_t found_interface = 0;

    while (offset + sizeof(usb_desc_header_t) <= total_len) {
        usb_desc_header_t *hdr = (usb_desc_header_t *) (buffer + offset);
        if (hdr->bLength == 0) {
            break;
        }

        if (hdr->bDescriptorType == USB_DESC_CONFIGURATION) {
            usb_config_desc_t *cfg = (usb_config_desc_t *) hdr;
            *config_value = cfg->bConfigurationValue;
        } else if (hdr->bDescriptorType == USB_DESC_INTERFACE) {
            usb_interface_desc_t *iface = (usb_interface_desc_t *) hdr;
            if (iface->bInterfaceClass == USB_CLASS_HID
                    && iface->bInterfaceSubClass == USB_SUBCLASS_BOOT
                    && iface->bInterfaceProtocol == USB_PROTOCOL_KEYBOARD) {
                *interface_num = iface->bInterfaceNumber;
                found_interface = 1;
            } else {
                found_interface = 0;
            }
        } else if (hdr->bDescriptorType == USB_DESC_ENDPOINT && found_interface) {
            usb_endpoint_desc_t *ep = (usb_endpoint_desc_t *) hdr;
            if ((ep->bEndpointAddress & 0x80u) != 0 && (ep->bmAttributes & 0x03u) == 0x03u) {
                *ep_addr = ep->bEndpointAddress;
                *ep_mps = (uint16_t) (ep->wMaxPacketSize & 0x07FFu);
                *ep_interval = ep->bInterval;
                return 0;
            }
        }

        offset = (uint16_t) (offset + hdr->bLength);
    }

    return -1;
}

static int xhci_configure_keyboard_endpoint(uint8_t port, uint8_t speed, uint8_t slot_id,
                                            uint8_t ep_addr, uint16_t ep_mps, uint8_t ep_interval) {
    uint8_t ep_num = (uint8_t) (ep_addr & 0x0Fu);
    uint8_t ep_id = (uint8_t) (ep_num * 2u + 1u);
    uint8_t interval = xhci_keyboard_interval(ep_interval);

    usb_memset(xhci_input_ctx, 0, sizeof(xhci_input_ctx));

    uint32_t *ctrl = xhci_ctx_ptr(xhci_input_ctx, 0);
    ctrl[0] = 0;
    ctrl[1] = (1u << 0) | (1u << ep_id);

    xhci_setup_slot_context(port, speed, ep_id);

    xhci_ring_init(&xhci_intr_ring, xhci_intr_ring_mem, XHCI_INTR_RING_SIZE);
    uint64_t intr_ring_addr = xhci_phys(xhci_intr_ring_mem);
    uint32_t *ep_ctx = xhci_ctx_ptr(xhci_input_ctx, 2 + ep_id - 1);
    xhci_setup_ep_context(ep_ctx, 7u, ep_mps, interval, intr_ring_addr, xhci_intr_ring.cycle);

    if (xhci_cmd_configure_endpoint(slot_id) != 0) {
        return -1;
    }

    xhci_kbd_ep_id = ep_id;
    xhci_kbd_report_len = (ep_mps < 8u) ? ep_mps : 8u;
    return 0;
}

static int xhci_enumerate_keyboard(uint8_t port, uint8_t speed) {
    int slot_id = xhci_cmd_enable_slot();
    if (slot_id <= 0) {
        return -1;
    }

    uint16_t ep0_mps = xhci_ep0_mps_from_speed(speed);
    if (xhci_cmd_address_device((uint8_t) slot_id, port, speed, ep0_mps) != 0) {
        return -1;
    }

    xhci_kbd_slot = (uint8_t) slot_id;

    uint8_t config_value = 0;
    uint8_t interface_num = 0;
    uint8_t ep_addr = 0;
    uint16_t ep_mps = 0;
    uint8_t ep_interval = 0;

    uint8_t config_head[9];
    if (xhci_get_descriptor(USB_DESC_CONFIGURATION, 0, config_head, sizeof(config_head)) != 0) {
        return -1;
    }

    usb_config_desc_t *cfg = (usb_config_desc_t *) config_head;
    uint16_t total_len = cfg->wTotalLength;
    if (total_len > 512u) {
        return -1;
    }

    uint8_t config_full[512];
    if (xhci_get_descriptor(USB_DESC_CONFIGURATION, 0, config_full, total_len) != 0) {
        return -1;
    }

    if (xhci_parse_hid_keyboard(config_full, total_len, &config_value, &interface_num,
            &ep_addr, &ep_mps, &ep_interval) != 0) {
        return -1;
    }

    if (xhci_set_configuration(config_value) != 0) {
        return -1;
    }

    if (xhci_set_protocol(interface_num) != 0) {
        return -1;
    }

    if (xhci_set_idle(interface_num) != 0) {
        return -1;
    }

    if (xhci_configure_keyboard_endpoint(port, speed, (uint8_t) slot_id,
            ep_addr, ep_mps, ep_interval) != 0) {
        return -1;
    }

    return 0;
}

static int xhci_submit_kbd_in(void) {
    xhci_trb_t trb = {0};
    uint64_t addr = xhci_phys(xhci_kbd_report);
    trb.dword0 = (uint32_t) (addr & 0xFFFFFFFFu);
    trb.dword1 = (uint32_t) (addr >> 32);
    trb.dword2 = xhci_kbd_report_len;
    trb.dword3 = (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
    xhci_ring_enqueue(&xhci_intr_ring, &trb);
    xhci_doorbells[xhci_kbd_slot] = xhci_kbd_ep_id;
    xhci_kbd_pending = 1;
    return 0;
}

static int xhci_poll_kbd_report(key_event_t *event) {
    uint32_t port_index = 0;
    for (uint8_t port = 1; port <= xhci_max_ports; ++port) {
        uint32_t portsc = xhci_op_read32(XHCI_PORTSC_BASE + (uint32_t) (port - 1) * 0x10u);
        if ((portsc & XHCI_PORTSC_CCS) != 0 && (portsc & XHCI_PORTSC_PED) != 0) {
            port_index = port;
            break;
        }
    }

    if (port_index == 0) {
        return 0;
    }

    if (!xhci_kbd_pending) {
        xhci_submit_kbd_in();
        return 0;
    }

    xhci_trb_t trb;
    if (!xhci_poll_event(&trb)) {
        return 0;
    }

    if (((trb.dword3 >> XHCI_TRB_TYPE_SHIFT) & 0x3Fu) != XHCI_TRB_TYPE_TRANSFER_EVENT) {
        return 0;
    }

    if (xhci_completion_code(&trb) != XHCI_COMPLETION_SUCCESS) {
        xhci_kbd_pending = 0;
        return 0;
    }

    xhci_kbd_pending = 0;
    return usb_hid_translate(xhci_kbd_report, event);
}

int xhci_init(uint8_t bus, uint8_t slot, uint8_t func, uint64_t mmio_base) {
    (void) bus;
    (void) slot;
    (void) func;

    xhci_kbd_ready = 0;
    xhci_kbd_pending = 0;

    xhci_mmio = (volatile uint8_t *) (uintptr_t) mmio_base;
    xhci_cap_len = *(volatile uint8_t *) xhci_mmio;

    uint32_t hcsparams1 = xhci_cap_read32(0x04u);
    xhci_max_slots = (uint8_t) (hcsparams1 & 0xFFu);
    xhci_max_ports = (uint8_t) ((hcsparams1 >> 24) & 0xFFu);

    uint32_t hccparams1 = xhci_cap_read32(0x10u);
    xhci_ctx_size = (hccparams1 & (1u << 2)) ? 64u : 32u;

    xhci_dboff = xhci_cap_read32(XHCI_DBOFF) & ~0x3u;
    xhci_rtsoff = xhci_cap_read32(XHCI_RTSOFF) & ~0x1Fu;

    xhci_doorbells = (volatile uint32_t *) (xhci_mmio + xhci_dboff);
    xhci_runtime = xhci_mmio + xhci_rtsoff;

    console_write("xHCI ports: ");
    console_write_dec(xhci_max_ports);
    console_putc('\n');

    xhci_legacy_handoff();

    if (xhci_reset_controller() != 0) {
        console_writeln("xHCI reset failed.");
        return -1;
    }

    usb_memset(xhci_dcbaa, 0, sizeof(xhci_dcbaa));
    uint32_t hcsparams2 = xhci_cap_read32(0x08u);
    uint32_t spb_hi = (hcsparams2 >> 27) & 0x1Fu;
    uint32_t spb_lo = (hcsparams2 >> 21) & 0x1Fu;
    uint32_t spb_count = (spb_hi << 5) | spb_lo;
    console_write("xHCI scratchpads: ");
    console_write_dec((int) spb_count);
    console_putc('\n');
    if (spb_count > XHCI_SCRATCHPAD_MAX) {
        console_writeln("xHCI scratchpad overflow.");
        return -1;
    }

    if (spb_count > 0) {
        for (uint32_t i = 0; i < spb_count; ++i) {
            xhci_scratchpad_ptrs[i] = xhci_phys(xhci_scratchpad_bufs[i]);
        }
        xhci_dcbaa[0] = xhci_phys(xhci_scratchpad_ptrs);
    }

    *(volatile uint64_t *) (xhci_mmio + xhci_cap_len + XHCI_DCBAAP) = xhci_phys(xhci_dcbaa);

    xhci_ring_init(&xhci_cmd_ring, xhci_cmd_ring_mem, XHCI_CMD_RING_SIZE);
    uint64_t cmd_ring_addr = xhci_phys(xhci_cmd_ring_mem);
    *(volatile uint64_t *) (xhci_mmio + xhci_cap_len + XHCI_CRCR) = cmd_ring_addr | xhci_cmd_ring.cycle;

    xhci_event_ring_init(&xhci_evt_ring, xhci_evt_ring_mem, XHCI_EVT_RING_SIZE);
    xhci_erst_mem[0].addr = xhci_phys(xhci_evt_ring_mem);
    xhci_erst_mem[0].size = XHCI_EVT_RING_SIZE;
    xhci_erst_mem[0].reserved = 0;

    xhci_rt_write32(0x20u, 0x3u);
    xhci_rt_write32(0x28u, 1);
    *(volatile uint64_t *) (xhci_runtime + 0x30u) = xhci_phys(xhci_erst_mem);
    *(volatile uint64_t *) (xhci_runtime + 0x38u) = xhci_phys(xhci_evt_ring_mem) | (1ull << 3);

    xhci_op_write32(XHCI_CONFIG, xhci_max_slots > 0 ? 1u : 0u);

    uint32_t cmd = xhci_op_read32(XHCI_USBCMD);
    cmd |= XHCI_USBCMD_RUN;
    xhci_op_write32(XHCI_USBCMD, cmd);

    if (xhci_wait_usbsts(XHCI_USBSTS_HALTED, 0) != 0) {
        console_writeln("xHCI failed to start.");
        return -1;
    }

    for (uint8_t port = 1; port <= xhci_max_ports; ++port) {
        uint32_t portsc = xhci_op_read32(XHCI_PORTSC_BASE + (uint32_t) (port - 1) * 0x10u);
        if (portsc & XHCI_PORTSC_CCS) {
            console_write("xHCI device on port ");
            console_write_dec(port);
            console_putc('\n');

            uint8_t speed = 0;
            if (xhci_reset_port(port, &speed) == 0) {
                if (xhci_enumerate_keyboard(port, speed) == 0) {
                    xhci_kbd_ready = 1;
                    console_writeln("USB keyboard ready (xHCI).");
                    return 0;
                }
            }
        }
    }

    console_writeln("No USB keyboard found on xHCI.");
    return -1;
}

int xhci_keyboard_ready(void) {
    return xhci_kbd_ready;
}

int xhci_keyboard_get_event(key_event_t *event) {
    if (!xhci_kbd_ready) {
        return 0;
    }

    if (xhci_poll_kbd_report(event)) {
        return 1;
    }

    if (!xhci_kbd_pending) {
        xhci_submit_kbd_in();
    }

    xhci_trb_t trb;
    if (!xhci_poll_event(&trb)) {
        return 0;
    }

    if (((trb.dword3 >> XHCI_TRB_TYPE_SHIFT) & 0x3Fu) != XHCI_TRB_TYPE_TRANSFER_EVENT) {
        return 0;
    }

    if (xhci_completion_code(&trb) != XHCI_COMPLETION_SUCCESS) {
        xhci_kbd_pending = 0;
        return 0;
    }

    xhci_kbd_pending = 0;
    return usb_hid_translate(xhci_kbd_report, event);
}
