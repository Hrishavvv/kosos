#include "disk.h"

#include "port.h"
#include "usb_msc.h"

#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6

#define ATA_REG_DATA 0x00
#define ATA_REG_ERROR 0x01
#define ATA_REG_SECCOUNT0 0x02
#define ATA_REG_LBA0 0x03
#define ATA_REG_LBA1 0x04
#define ATA_REG_LBA2 0x05
#define ATA_REG_HDDEVSEL 0x06
#define ATA_REG_COMMAND 0x07
#define ATA_REG_STATUS 0x07

#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_IDENTIFY 0xEC

#define ATA_STATUS_BSY 0x80
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_ERR 0x01

#define ATA_TIMEOUT 100000

static uint32_t g_total_sectors = 0;
/* RAM disk fallback when no physical disk is present */
#define RAMDISK_SIZE_BYTES (16 * 1024 * 1024)
#define RAMDISK_SECTORS (RAMDISK_SIZE_BYTES / 512)
static uint8_t g_ramdisk[RAMDISK_SIZE_BYTES];
static int g_using_ramdisk = 0;
static int g_using_usb_msc = 0;

static void ata_select_drive(uint32_t lba) {
    uint8_t drive = 0xE0 | ((lba >> 24) & 0x0F);
    outb(ATA_PRIMARY_IO + ATA_REG_HDDEVSEL, drive);
    io_wait();
}

static int ata_wait_not_busy(void) {
    for (int i = 0; i < ATA_TIMEOUT; ++i) {
        if ((inb(ATA_PRIMARY_IO + ATA_REG_STATUS) & ATA_STATUS_BSY) == 0) {
            return 0;
        }
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (int i = 0; i < ATA_TIMEOUT; ++i) {
        uint8_t status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
        if (status & ATA_STATUS_ERR) {
            return -1;
        }
        if (status & ATA_STATUS_DRQ) {
            return 0;
        }
    }
    return -1;
}

static int ata_identify(uint32_t *total_sectors) {
    outb(ATA_PRIMARY_IO + ATA_REG_HDDEVSEL, 0xA0);
    io_wait();

    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA0, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA1, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA2, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    if (status == 0) {
        return -1;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    if (inb(ATA_PRIMARY_IO + ATA_REG_LBA1) != 0 || inb(ATA_PRIMARY_IO + ATA_REG_LBA2) != 0) {
        return -1;
    }

    if (ata_wait_drq() != 0) {
        return -1;
    }

    uint16_t data[256];
    for (int i = 0; i < 256; ++i) {
        data[i] = inw(ATA_PRIMARY_IO + ATA_REG_DATA);
    }

    uint32_t lba28 = ((uint32_t) data[61] << 16) | data[60];
    if (lba28 == 0) {
        return -1;
    }

    *total_sectors = lba28;
    return 0;
}

int disk_init(void) {
    uint32_t sectors = 0;
    if (ata_identify(&sectors) != 0) {
        /* No ATA device — try USB MSC */
        if (usb_msc_init() == 0 && usb_msc_ready()) {
            const usb_msc_info_t *info = usb_msc_get_info();
            g_total_sectors = info->capacity_bytes / 512;
            g_using_usb_msc = 1;
            return 0;
        }
        
        /* No USB MSC either — enable RAM disk fallback */
        g_total_sectors = RAMDISK_SECTORS;
        g_using_ramdisk = 1;
        /* zero the ramdisk */
        for (uint32_t i = 0; i < RAMDISK_SIZE_BYTES; ++i) {
            g_ramdisk[i] = 0;
        }
        return 0;
    }

    g_total_sectors = sectors;
    return 0;
}

uint32_t disk_total_sectors(void) {
    return g_total_sectors;
}

int disk_read(uint32_t lba, uint8_t *buffer) {
    if (g_using_usb_msc) {
        return usb_msc_read_sector(lba, buffer);
    }
    
    if (g_using_ramdisk) {
        if (lba >= g_total_sectors) return -1;
        uint32_t offset = lba * 512;
        for (int i = 0; i < 512; ++i) {
            buffer[i] = g_ramdisk[offset + i];
        }
        return 0;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    ata_select_drive(lba);
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, 1);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA0, (uint8_t) (lba & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA1, (uint8_t) ((lba >> 8) & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA2, (uint8_t) ((lba >> 16) & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    if (ata_wait_drq() != 0) {
        return -1;
    }

    for (int i = 0; i < 256; ++i) {
        uint16_t data = inw(ATA_PRIMARY_IO + ATA_REG_DATA);
        buffer[i * 2] = (uint8_t) (data & 0xFF);
        buffer[i * 2 + 1] = (uint8_t) (data >> 8);
    }

    return 0;
}

int disk_write(uint32_t lba, const uint8_t *buffer) {
    if (g_using_usb_msc) {
        return usb_msc_write_sector(lba, buffer);
    }
    
    if (g_using_ramdisk) {
        if (lba >= g_total_sectors) return -1;
        uint32_t offset = lba * 512;
        for (int i = 0; i < 512; ++i) {
            g_ramdisk[offset + i] = buffer[i];
        }
        return 0;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    ata_select_drive(lba);
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, 1);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA0, (uint8_t) (lba & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA1, (uint8_t) ((lba >> 8) & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA2, (uint8_t) ((lba >> 16) & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    if (ata_wait_drq() != 0) {
        return -1;
    }

    for (int i = 0; i < 256; ++i) {
        uint16_t data = (uint16_t) buffer[i * 2] | ((uint16_t) buffer[i * 2 + 1] << 8);
        outw(ATA_PRIMARY_IO + ATA_REG_DATA, data);
    }

    io_wait();
    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    return 0;
}
