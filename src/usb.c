#include "usb.h"

#include "console.h"
#include "pci.h"
#include "usb_ehci.h"
#include "usb_ohci.h"
#include "usb_uhci.h"
#include "usb_xhci.h"

#define PCI_CLASS_SERIAL 0x0Cu
#define PCI_SUBCLASS_USB 0x03u

#define USB_PROGIF_UHCI 0x00u
#define USB_PROGIF_OHCI 0x10u
#define USB_PROGIF_EHCI 0x20u
#define USB_PROGIF_XHCI 0x30u

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint8_t prog_if;
    uint64_t mmio_base;
} usb_controller_t;

static int usb_backend = 0;

static int controller_mmio_valid(uint64_t base) {
    return base != 0 && base <= 0xFFFFFFFFu;
}

static int scan_usb_controllers(usb_controller_t *xhci, usb_controller_t *ehci, usb_controller_t *ohci, usb_controller_t *uhci) {
    int found = 0;
    xhci->prog_if = 0xFFu;
    ehci->prog_if = 0xFFu;
    ohci->prog_if = 0xFFu;
    uhci->prog_if = 0xFFu;

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t func = 0; func < 8; ++func) {
                uint16_t vendor = pci_read_vendor((uint8_t) bus, slot, func);
                if (vendor == 0xFFFFu) {
                    if (func == 0) {
                        break;
                    }
                    continue;
                }

                uint8_t class_code = pci_read_class((uint8_t) bus, slot, func);
                uint8_t subclass = pci_read_subclass((uint8_t) bus, slot, func);
                if (class_code != PCI_CLASS_SERIAL || subclass != PCI_SUBCLASS_USB) {
                    continue;
                }

                uint8_t prog_if = pci_read_prog_if((uint8_t) bus, slot, func);
                int is_64 = 0;
                uint64_t bar = pci_read_bar((uint8_t) bus, slot, func, 0, &is_64);

                if (prog_if == USB_PROGIF_XHCI && xhci->prog_if == 0xFFu) {
                    xhci->bus = (uint8_t) bus;
                    xhci->slot = slot;
                    xhci->func = func;
                    xhci->prog_if = prog_if;
                    xhci->mmio_base = bar;
                    found = 1;
                } else if (prog_if == USB_PROGIF_EHCI && ehci->prog_if == 0xFFu) {
                    ehci->bus = (uint8_t) bus;
                    ehci->slot = slot;
                    ehci->func = func;
                    ehci->prog_if = prog_if;
                    ehci->mmio_base = bar;
                    found = 1;
                } else if (prog_if == USB_PROGIF_OHCI && ohci->prog_if == 0xFFu) {
                    ohci->bus = (uint8_t) bus;
                    ohci->slot = slot;
                    ohci->func = func;
                    ohci->prog_if = prog_if;
                    ohci->mmio_base = bar;
                    found = 1;
                } else if (prog_if == USB_PROGIF_UHCI && uhci->prog_if == 0xFFu) {
                    uhci->bus = (uint8_t) bus;
                    uhci->slot = slot;
                    uhci->func = func;
                    uhci->prog_if = prog_if;
                    uhci->mmio_base = bar;
                    found = 1;
                }
            }
        }
    }

    return found;
}

int usb_init(void) {
    usb_backend = 0;

    usb_controller_t xhci = {0};
    usb_controller_t ehci = {0};
    usb_controller_t ohci = {0};
    usb_controller_t uhci = {0};
    if (!scan_usb_controllers(&xhci, &ehci, &ohci, &uhci)) {
        console_writeln("USB controller not found.");
        return -1;
    }

    console_write("USB: xHCI found=");
    console_write_dec(xhci.prog_if != 0xFFu ? 1 : 0);
    console_write(" EHCI found=");
    console_write_dec(ehci.prog_if != 0xFFu ? 1 : 0);
    console_write(" OHCI found=");
    console_write_dec(ohci.prog_if != 0xFFu ? 1 : 0);
    console_write(" UHCI found=");
    console_write_dec(uhci.prog_if != 0xFFu ? 1 : 0);
    console_putc('\n');

    if (xhci.prog_if == USB_PROGIF_XHCI) {
        console_writeln("USB: Trying xHCI...");
        if (!controller_mmio_valid(xhci.mmio_base)) {
            console_writeln("xHCI MMIO above 4GB not supported.");
        } else {
            pci_enable_bus_master(xhci.bus, xhci.slot, xhci.func);
            int xhci_result = xhci_init(xhci.bus, xhci.slot, xhci.func, xhci.mmio_base);
            console_write("xHCI init returned: ");
            console_write_dec(xhci_result);
            console_putc('\n');
            if (xhci_result == 0) {
                usb_backend = 1;
                console_writeln("USB: Using xHCI backend.");
                return 0;
            }
        }
    }

    if (ehci.prog_if == USB_PROGIF_EHCI) {
        console_writeln("USB: Trying EHCI...");
        if (!controller_mmio_valid(ehci.mmio_base)) {
            console_writeln("EHCI MMIO above 4GB not supported.");
        } else {
            pci_enable_bus_master(ehci.bus, ehci.slot, ehci.func);
            int ehci_result = ehci_init(ehci.bus, ehci.slot, ehci.func, ehci.mmio_base);
            console_write("EHCI init returned: ");
            console_write_dec(ehci_result);
            console_putc('\n');
            if (ehci_result == 0) {
                usb_backend = 2;
                console_writeln("USB: Using EHCI backend.");
                return 0;
            }
        }
    }

    if (ohci.prog_if == USB_PROGIF_OHCI) {
        console_writeln("USB: Trying OHCI...");
        if (!controller_mmio_valid(ohci.mmio_base)) {
            console_writeln("OHCI MMIO above 4GB not supported.");
        } else {
            pci_enable_bus_master(ohci.bus, ohci.slot, ohci.func);
            int ohci_result = ohci_init(ohci.bus, ohci.slot, ohci.func, ohci.mmio_base);
            console_write("OHCI init returned: ");
            console_write_dec(ohci_result);
            console_putc('\n');
            if (ohci_result == 0) {
                usb_backend = 3;
                console_writeln("USB: Using OHCI backend.");
                return 0;
            }
        }
    }

    if (uhci.prog_if == USB_PROGIF_UHCI) {
        console_writeln("USB: Trying UHCI...");
        if (!controller_mmio_valid(uhci.mmio_base)) {
            console_writeln("UHCI MMIO above 4GB not supported.");
        } else {
            pci_enable_bus_master(uhci.bus, uhci.slot, uhci.func);
            int uhci_result = uhci_init(uhci.bus, uhci.slot, uhci.func, uhci.mmio_base);
            console_write("UHCI init returned: ");
            console_write_dec(uhci_result);
            console_putc('\n');
            if (uhci_result == 0) {
                usb_backend = 4;
                console_writeln("USB: Using UHCI backend.");
                return 0;
            }
        }
    }

    console_writeln("USB init failed.");
    return -1;
}

int usb_keyboard_ready(void) {
    return usb_backend != 0;
}

int usb_keyboard_get_event(key_event_t *event) {
    if (usb_backend == 1) {
        return xhci_keyboard_get_event(event);
    }
    if (usb_backend == 2) {
        return ehci_keyboard_get_event(event);
    }
    if (usb_backend == 3) {
        return ohci_keyboard_get_event(event);
    }
    if (usb_backend == 4) {
        return uhci_keyboard_get_event(event);
    }
    return 0;
}
