#ifndef KOSOS_PCI_H
#define KOSOS_PCI_H

#include <stdint.h>

uint32_t pci_read_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint64_t pci_read_bar(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_index, int *is_64);
void pci_enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func);

uint16_t pci_read_vendor(uint8_t bus, uint8_t slot, uint8_t func);
uint16_t pci_read_device(uint8_t bus, uint8_t slot, uint8_t func);
uint8_t pci_read_class(uint8_t bus, uint8_t slot, uint8_t func);
uint8_t pci_read_subclass(uint8_t bus, uint8_t slot, uint8_t func);
uint8_t pci_read_prog_if(uint8_t bus, uint8_t slot, uint8_t func);

#endif
