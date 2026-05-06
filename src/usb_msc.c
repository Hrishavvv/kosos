#include "usb_msc.h"
#include "console.h"
#include "usb_utils.h"

#include <stdint.h>

/* MSC BOT (Bulk-Only Transfer) constants */
#define USB_CLASS_MSC 0x08
#define USB_SUBCLASS_SCSI 0x06
#define USB_PROTOCOL_BOT 0x50

/* SCSI commands */
#define SCSI_INQUIRY 0x12
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_READ10 0x28
#define SCSI_WRITE10 0x2A

/* CBW (Command Block Wrapper) */
typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_len;
    uint8_t flags;
    uint8_t lun;
    uint8_t cmd_len;
    uint8_t cmd[16];
} __attribute__((packed)) msc_cbw_t;

/* CSW (Command Status Wrapper) */
typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __attribute__((packed)) msc_csw_t;

static usb_msc_info_t g_msc_info = {0};
static int g_msc_ready = 0;
static uint8_t g_msc_addr __attribute__((unused)) = 0;
static uint8_t g_msc_bulk_out_ep __attribute__((unused)) = 0;
static uint8_t g_msc_bulk_in_ep __attribute__((unused)) = 0;

static uint8_t g_msc_buffer[512] __attribute__((aligned(64)));
static uint32_t g_msc_tag = 0x12345678;

static int msc_send_cbw(uint8_t cmd_code, uint8_t dir, const void *cmd_data, uint8_t cmd_len,
                         uint32_t data_len) {
    msc_cbw_t cbw;
    usb_memset(&cbw, 0, sizeof(cbw));
    cbw.signature = 0x43425355;
    cbw.tag = g_msc_tag++;
    cbw.data_len = data_len;
    cbw.flags = dir ? 0x80 : 0x00;
    cbw.lun = 0;
    cbw.cmd_len = cmd_len;
    cbw.cmd[0] = cmd_code;
    if (cmd_data) {
        usb_memcpy(cbw.cmd + 1, cmd_data, cmd_len - 1);
    }
    /* TODO: Submit CBW via bulk OUT */
    (void) cbw;
    return -1;
}

static int msc_read_csw(void) {
    /* TODO: Read CSW via bulk IN */
    return -1;
}

static int __attribute__((unused)) msc_inquiry(void) {
    uint8_t cmd[6] = {0, 0, 0, 0, 36, 0};
    if (msc_send_cbw(SCSI_INQUIRY, 1, cmd + 1, 5, 36) != 0) {
        return -1;
    }
    
    /* TODO: Read inquiry response */
    
    if (msc_read_csw() != 0) {
        return -1;
    }
    
    usb_memcpy(g_msc_info.vendor, g_msc_buffer + 8, 8);
    g_msc_info.vendor[8] = '\0';
    usb_memcpy(g_msc_info.model, g_msc_buffer + 16, 16);
    g_msc_info.model[16] = '\0';
    
    return 0;
}

static int __attribute__((unused)) msc_capacity(void) {
    uint8_t cmd[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (msc_send_cbw(0x25, 1, cmd + 1, 9, 8) != 0) {
        return -1;
    }
    
    /* TODO: Read capacity response */
    
    if (msc_read_csw() != 0) {
        return -1;
    }
    
    uint32_t last_lba = ((uint32_t)g_msc_buffer[0] << 24) |
                        ((uint32_t)g_msc_buffer[1] << 16) |
                        ((uint32_t)g_msc_buffer[2] << 8) |
                        g_msc_buffer[3];
    uint32_t block_size = ((uint32_t)g_msc_buffer[4] << 24) |
                          ((uint32_t)g_msc_buffer[5] << 16) |
                          ((uint32_t)g_msc_buffer[6] << 8) |
                          g_msc_buffer[7];
    
    g_msc_info.block_size = block_size;
    g_msc_info.capacity_bytes = (uint64_t)(last_lba + 1) * block_size;
    
    return 0;
}

int usb_msc_init(void) {
    console_writeln("USB MSC: Scanning for storage devices...");
    /* TODO: Scan USB devices for MSC class */
    return -1;
}

int usb_msc_ready(void) {
    return g_msc_ready;
}

int usb_msc_read_sector(uint32_t lba, uint8_t *buffer) {
    if (!g_msc_ready) return -1;
    /* TODO: Implement READ10 command */
    (void) lba;
    (void) buffer;
    return -1;
}

int usb_msc_write_sector(uint32_t lba, const uint8_t *buffer) {
    if (!g_msc_ready) return -1;
    /* TODO: Implement WRITE10 command */
    (void) lba;
    (void) buffer;
    return -1;
}

const usb_msc_info_t *usb_msc_get_info(void) {
    return &g_msc_info;
}
