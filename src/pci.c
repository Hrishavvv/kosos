#include "pci.h"

#include "port.h"

uint32_t pci_read_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (1u << 31)
        | ((uint32_t) bus << 16)
        | ((uint32_t) slot << 11)
        | ((uint32_t) func << 8)
        | (offset & 0xFCu);

    outl(0xCF8, address);
    return inl(0xCFC);
}

void pci_write_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (1u << 31)
        | ((uint32_t) bus << 16)
        | ((uint32_t) slot << 11)
        | ((uint32_t) func << 8)
        | (offset & 0xFCu);

    outl(0xCF8, address);
    outl(0xCFC, value);
}

uint64_t pci_read_bar(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_index, int *is_64) {
    uint8_t offset = (uint8_t) (0x10 + bar_index * 4);
    uint32_t low = pci_read_config32(bus, slot, func, offset);
    if (low & 0x1u) {
        if (is_64) {
            *is_64 = 0;
        }
        return 0;
    }

    uint32_t type = (low >> 1) & 0x3u;
    uint64_t base = (uint64_t) (low & 0xFFFFFFF0u);
    if (type == 0x2u) {
        uint32_t high = pci_read_config32(bus, slot, func, (uint8_t) (offset + 4));
        base |= ((uint64_t) high << 32);
        if (is_64) {
            *is_64 = 1;
        }
    } else if (is_64) {
        *is_64 = 0;
    }

    return base;
}

void pci_enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t command = pci_read_config32(bus, slot, func, 0x04);
    command |= (1u << 1) | (1u << 2);
    pci_write_config32(bus, slot, func, 0x04, command);
}

uint16_t pci_read_vendor(uint8_t bus, uint8_t slot, uint8_t func) {
    return (uint16_t) (pci_read_config32(bus, slot, func, 0x00) & 0xFFFFu);
}

uint16_t pci_read_device(uint8_t bus, uint8_t slot, uint8_t func) {
    return (uint16_t) ((pci_read_config32(bus, slot, func, 0x00) >> 16) & 0xFFFFu);
}

uint8_t pci_read_class(uint8_t bus, uint8_t slot, uint8_t func) {
    return (uint8_t) ((pci_read_config32(bus, slot, func, 0x08) >> 24) & 0xFFu);
}

uint8_t pci_read_subclass(uint8_t bus, uint8_t slot, uint8_t func) {
    return (uint8_t) ((pci_read_config32(bus, slot, func, 0x08) >> 16) & 0xFFu);
}

uint8_t pci_read_prog_if(uint8_t bus, uint8_t slot, uint8_t func) {
    return (uint8_t) ((pci_read_config32(bus, slot, func, 0x08) >> 8) & 0xFFu);
}
