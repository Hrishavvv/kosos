CC := gcc
LD := ld
GRUB_MKRESCUE := grub-mkrescue

BUILD_DIR := build
ISO_DIR := $(BUILD_DIR)/iso
KERNEL_ELF := $(BUILD_DIR)/kosos.elf
ISO_IMAGE := $(BUILD_DIR)/kosos.iso
DISK_IMAGE := $(BUILD_DIR)/kosos-disk.img
DISK_SIZE_MB := 16

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -fno-pic -fno-pie -m64 -mno-red-zone -Iinclude
ASFLAGS := -ffreestanding -fno-pic -m64
LDFLAGS := -m elf_x86_64 -T linker.ld

SRC_C := $(wildcard src/*.c)
SRC_S := $(wildcard src/*.S)
OBJ := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC_C)) $(patsubst src/%.S,$(BUILD_DIR)/%.o,$(SRC_S))

.PHONY: all clean iso run flash disk

all: $(KERNEL_ELF)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/%.S | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJ) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

iso: $(ISO_IMAGE)

$(ISO_IMAGE): $(KERNEL_ELF)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_ELF) $(ISO_DIR)/boot/kosos.elf
	cp grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_DIR) >/dev/null

run: $(ISO_IMAGE) $(DISK_IMAGE)
	qemu-system-i386 -cdrom $(ISO_IMAGE) -drive file=$(DISK_IMAGE),format=raw,if=ide

disk: $(DISK_IMAGE)

$(DISK_IMAGE): | $(BUILD_DIR)
	dd if=/dev/zero of=$(DISK_IMAGE) bs=1M count=$(DISK_SIZE_MB)

flash: $(ISO_IMAGE)
	@echo "Usage: sudo make flash DEVICE=/dev/sdX"
	@test -n "$(DEVICE)"
	sudo dd if=$(ISO_IMAGE) of=$(DEVICE) bs=4M status=progress conv=fsync

clean:
	rm -rf $(BUILD_DIR)
