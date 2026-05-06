# Kosos

Kosos is a tiny teaching OS that boots with GRUB, draws to the VGA text buffer, reads the keyboard by polling the PS/2 controller, and exposes a slang-friendly CLI with a simple disk filesystem.

## What it can do

- Boot from a GRUB-based ISO image
- Show text on the screen in 80x25 VGA mode
- Accept keyboard input without interrupts
- Run slang commands like `peek`, `slide`, `spawn`, `tag`, `spill`, and `where`
- Create directories and files on a persistent IDE disk image

## Build

```bash
make
```

## Build a bootable image

```bash
make iso
```

The ISO will be written to `build/kosos.iso`.

## Create a persistent disk image

```bash
make disk
```

This creates `build/kosos-disk.img` (16 MB raw IDE disk).

## Run in QEMU

```bash
make run
```

This attaches the ISO and the raw disk image to an IDE controller.

## Run in VirtualBox

1. Create a VM: Type "Other", Version "Other/Unknown (32-bit)", disable EFI.
2. Attach the ISO: Storage -> Optical Drive -> choose `build/kosos.iso`.
3. Convert the raw disk image to a VDI:

```bash
VBoxManage convertfromraw build/kosos-disk.img build/kosos-disk.vdi --format VDI
```

4. Attach `build/kosos-disk.vdi` as an IDE Primary Master disk.
5. Boot the VM.

## Flash

Write the ISO to a USB device with `dd`:

```bash
sudo make flash DEVICE=/dev/sdX
```

Replace `/dev/sdX` with the target device. For persistent storage on real hardware, use a second disk or a dedicated partition attached as an IDE device.

## Notes

- The project is freestanding and does not use libc.
- The current build uses GCC and GNU ld; NASM is not required.
- If you want a hand-written bootloader instead of GRUB, that can be done next, but this version keeps the boot path simple and reliable.
