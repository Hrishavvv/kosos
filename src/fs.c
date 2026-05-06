#include "fs.h"

#include "console.h"
#include "disk.h"
#include "os.h"

#include <stdint.h>

#define FS_MAGIC 0x4F534F4B
#define FS_VERSION 1
#define FS_SECTOR_SIZE 512
#define FS_CLUSTER_SECTORS 1
#define FS_MAX_CLUSTERS 65536

#define FS_FAT_FREE 0x00000000
#define FS_FAT_EOC 0xFFFFFFFF
#define FS_FAT_RSVD 0xFFFFFFFE

#define FS_ENTRY_FREE 0

#define FS_NAME_MAX 15

struct fs_superblock {
    uint32_t magic;
    uint32_t version;
    uint32_t total_sectors;
    uint32_t fat_start;
    uint32_t fat_sectors;
    uint32_t data_start;
    uint32_t cluster_count;
    uint32_t root_dir_cluster;
} __attribute__((packed));

struct fs_dir_entry {
    char name[16];
    uint8_t type;
    uint8_t reserved1[3];
    uint32_t size;
    uint32_t first_cluster;
    uint8_t reserved2[4];
} __attribute__((packed));

static int fs_split_path(const char *path, uint32_t *parent_out, char *name_out,
                         size_t name_size, char *parent_path, size_t parent_path_size);

static struct fs_superblock g_sb;
static uint32_t g_fat[FS_MAX_CLUSTERS + 2];
static uint32_t g_fat_entries = 0;
static uint32_t g_cwd_cluster = 0;
static char g_cwd_path[128];
static int g_ready = 0;

static size_t k_strlen(const char *text) {
    size_t len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    return len;
}

static int k_strcmp(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) {
            return (int) ((unsigned char) *left - (unsigned char) *right);
        }
        ++left;
        ++right;
    }
    return (int) ((unsigned char) *left - (unsigned char) *right);
}

static void k_memset(void *dest, int value, size_t count) {
    uint8_t *ptr = (uint8_t *) dest;
    for (size_t i = 0; i < count; ++i) {
        ptr[i] = (uint8_t) value;
    }
}

static void k_memcpy(void *dest, const void *src, size_t count) {
    uint8_t *dst = (uint8_t *) dest;
    const uint8_t *source = (const uint8_t *) src;
    for (size_t i = 0; i < count; ++i) {
        dst[i] = source[i];
    }
}

static void fs_error(const char *message) {
    console_write(OS_NAME);
    console_write(": ");
    console_writeln(message);
}

static int fs_write_sector(uint32_t lba, const uint8_t *buffer) {
    return disk_write(lba, buffer);
}

static int fs_read_sector(uint32_t lba, uint8_t *buffer) {
    return disk_read(lba, buffer);
}

static int fs_read_cluster(uint32_t cluster, uint8_t *buffer) {
    if (cluster < 2 || cluster >= g_fat_entries) {
        return -1;
    }

    uint32_t lba = g_sb.data_start + (cluster - 2) * FS_CLUSTER_SECTORS;
    return fs_read_sector(lba, buffer);
}

static int fs_write_cluster(uint32_t cluster, const uint8_t *buffer) {
    if (cluster < 2 || cluster >= g_fat_entries) {
        return -1;
    }

    uint32_t lba = g_sb.data_start + (cluster - 2) * FS_CLUSTER_SECTORS;
    return fs_write_sector(lba, buffer);
}

static int fs_flush_fat(void) {
    uint8_t sector[FS_SECTOR_SIZE];
    uint32_t fat_bytes = g_fat_entries * sizeof(uint32_t);
    uint32_t offset = 0;

    for (uint32_t i = 0; i < g_sb.fat_sectors; ++i) {
        k_memset(sector, 0, sizeof(sector));
        uint32_t chunk = FS_SECTOR_SIZE;
        if (offset + chunk > fat_bytes) {
            chunk = fat_bytes - offset;
        }
        k_memcpy(sector, ((uint8_t *) g_fat) + offset, chunk);
        if (fs_write_sector(g_sb.fat_start + i, sector) != 0) {
            return -1;
        }
        offset += chunk;
    }

    return 0;
}

static int fs_load_fat(void) {
    uint8_t sector[FS_SECTOR_SIZE];
    uint32_t fat_bytes = g_fat_entries * sizeof(uint32_t);
    uint32_t offset = 0;

    for (uint32_t i = 0; i < g_sb.fat_sectors; ++i) {
        if (fs_read_sector(g_sb.fat_start + i, sector) != 0) {
            return -1;
        }
        uint32_t chunk = FS_SECTOR_SIZE;
        if (offset + chunk > fat_bytes) {
            chunk = fat_bytes - offset;
        }
        k_memcpy(((uint8_t *) g_fat) + offset, sector, chunk);
        offset += chunk;
    }

    return 0;
}

static int fs_write_superblock(void) {
    uint8_t sector[FS_SECTOR_SIZE];
    k_memset(sector, 0, sizeof(sector));
    k_memcpy(sector, &g_sb, sizeof(g_sb));
    return fs_write_sector(0, sector);
}

static uint32_t fs_alloc_cluster(void) {
    for (uint32_t i = 2; i < g_fat_entries; ++i) {
        if (g_fat[i] == FS_FAT_FREE) {
            g_fat[i] = FS_FAT_EOC;
            return i;
        }
    }
    return 0;
}

static int fs_init_dir_cluster(uint32_t cluster, uint32_t parent) {
    uint8_t sector[FS_SECTOR_SIZE];
    k_memset(sector, 0, sizeof(sector));

    struct fs_dir_entry *entries = (struct fs_dir_entry *) sector;
    k_memset(entries, 0, sizeof(struct fs_dir_entry) * 2);

    k_memcpy(entries[0].name, ".", 2);
    entries[0].type = FS_ENTRY_DIR;
    entries[0].first_cluster = cluster;

    k_memcpy(entries[1].name, "..", 3);
    entries[1].type = FS_ENTRY_DIR;
    entries[1].first_cluster = parent;

    return fs_write_cluster(cluster, sector);
}

static int fs_format(void) {
    uint32_t total = disk_total_sectors();
    if (total < 128) {
        fs_error("Disk too small for filesystem.");
        return -1;
    }

    uint32_t fat_sectors = 1;
    uint32_t cluster_count = 0;

    for (int i = 0; i < 8; ++i) {
        uint32_t data_sectors = total - 1 - fat_sectors;
        cluster_count = data_sectors / FS_CLUSTER_SECTORS;
        if (cluster_count > FS_MAX_CLUSTERS) {
            cluster_count = FS_MAX_CLUSTERS;
        }
        uint32_t fat_bytes = (cluster_count + 2) * sizeof(uint32_t);
        uint32_t needed = (fat_bytes + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
        if (needed == fat_sectors) {
            break;
        }
        fat_sectors = needed;
    }

    g_sb.magic = FS_MAGIC;
    g_sb.version = FS_VERSION;
    g_sb.total_sectors = total;
    g_sb.fat_start = 1;
    g_sb.fat_sectors = fat_sectors;
    g_sb.data_start = 1 + fat_sectors;
    g_sb.cluster_count = cluster_count;
    g_sb.root_dir_cluster = 2;

    g_fat_entries = cluster_count + 2;
    for (uint32_t i = 0; i < g_fat_entries; ++i) {
        g_fat[i] = FS_FAT_FREE;
    }
    g_fat[0] = FS_FAT_RSVD;
    g_fat[1] = FS_FAT_RSVD;
    g_fat[g_sb.root_dir_cluster] = FS_FAT_EOC;

    if (fs_write_superblock() != 0) {
        fs_error("Failed to write superblock.");
        return -1;
    }

    if (fs_flush_fat() != 0) {
        fs_error("Failed to write FAT.");
        return -1;
    }

    if (fs_init_dir_cluster(g_sb.root_dir_cluster, g_sb.root_dir_cluster) != 0) {
        fs_error("Failed to initialize root directory.");
        return -1;
    }

    return 0;
}

static const char *fs_next_component(const char *path, char *out, size_t out_size) {
    while (*path == '/') {
        ++path;
    }

    if (*path == '\0') {
        return NULL;
    }

    size_t len = 0;
    while (path[len] != '\0' && path[len] != '/') {
        if (len + 1 >= out_size) {
            return NULL;
        }
        out[len] = path[len];
        ++len;
    }
    out[len] = '\0';

    return path + len;
}

static int fs_has_more(const char *path) {
    while (*path == '/') {
        ++path;
    }
    return *path != '\0';
}

static void fs_path_pop(char *path) {
    size_t len = k_strlen(path);
    if (len <= 1) {
        path[0] = '/';
        path[1] = '\0';
        return;
    }

    if (path[len - 1] == '/') {
        path[len - 1] = '\0';
        len -= 1;
    }

    while (len > 1 && path[len - 1] != '/') {
        path[len - 1] = '\0';
        --len;
    }

    if (len == 1) {
        path[1] = '\0';
    }
}

static int fs_path_append(char *path, size_t path_size, const char *name) {
    size_t len = k_strlen(path);
    size_t name_len = k_strlen(name);

    if (len == 0) {
        if (path_size < 2) {
            return -1;
        }
        path[0] = '/';
        path[1] = '\0';
        len = 1;
    }

    if (len > 1 && path[len - 1] != '/') {
        if (len + 1 >= path_size) {
            return -1;
        }
        path[len++] = '/';
        path[len] = '\0';
    }

    if (len + name_len >= path_size) {
        return -1;
    }

    k_memcpy(path + len, name, name_len + 1);
    return 0;
}

static int fs_find_entry(uint32_t dir_cluster, const char *name,
                         struct fs_dir_entry *out_entry,
                         uint32_t *out_cluster,
                         uint32_t *out_index) {
    uint8_t sector[FS_SECTOR_SIZE];
    uint32_t cluster = dir_cluster;

    while (cluster != FS_FAT_EOC) {
        if (fs_read_cluster(cluster, sector) != 0) {
            return -1;
        }

        struct fs_dir_entry *entries = (struct fs_dir_entry *) sector;
        for (uint32_t i = 0; i < FS_SECTOR_SIZE / sizeof(struct fs_dir_entry); ++i) {
            if (entries[i].name[0] == '\0') {
                continue;
            }
            if (k_strcmp(entries[i].name, name) == 0) {
                if (out_entry) {
                    *out_entry = entries[i];
                }
                if (out_cluster) {
                    *out_cluster = cluster;
                }
                if (out_index) {
                    *out_index = i;
                }
                return 0;
            }
        }

        uint32_t next = g_fat[cluster];
        if (next == FS_FAT_EOC || next == FS_FAT_FREE) {
            break;
        }
        cluster = next;
    }

    return 1;
}

static int fs_add_entry(uint32_t dir_cluster, const struct fs_dir_entry *entry) {
    uint8_t sector[FS_SECTOR_SIZE];
    uint32_t cluster = dir_cluster;

    while (cluster != FS_FAT_EOC) {
        if (fs_read_cluster(cluster, sector) != 0) {
            return -1;
        }

        struct fs_dir_entry *entries = (struct fs_dir_entry *) sector;
        for (uint32_t i = 0; i < FS_SECTOR_SIZE / sizeof(struct fs_dir_entry); ++i) {
            if (entries[i].name[0] == '\0') {
                entries[i] = *entry;
                return fs_write_cluster(cluster, sector);
            }
        }

        uint32_t next = g_fat[cluster];
        if (next == FS_FAT_EOC || next == FS_FAT_FREE) {
            break;
        }
        cluster = next;
    }

    uint32_t new_cluster = fs_alloc_cluster();
    if (new_cluster == 0) {
        return -1;
    }

    g_fat[cluster] = new_cluster;
    g_fat[new_cluster] = FS_FAT_EOC;
    if (fs_flush_fat() != 0) {
        return -1;
    }

    k_memset(sector, 0, sizeof(sector));
    struct fs_dir_entry *entries = (struct fs_dir_entry *) sector;
    entries[0] = *entry;
    return fs_write_cluster(new_cluster, sector);
}

static int fs_write_entry_at(uint32_t cluster, uint32_t index, const struct fs_dir_entry *entry) {
    uint8_t sector[FS_SECTOR_SIZE];
    if (fs_read_cluster(cluster, sector) != 0) {
        return -1;
    }

    struct fs_dir_entry *entries = (struct fs_dir_entry *) sector;
    entries[index] = *entry;
    return fs_write_cluster(cluster, sector);
}

static void fs_clear_entry(struct fs_dir_entry *entry) {
    k_memset(entry, 0, sizeof(*entry));
}

static void fs_free_chain(uint32_t start) {
    uint32_t cluster = start;
    while (cluster >= 2 && cluster < g_fat_entries && cluster != FS_FAT_FREE) {
        uint32_t next = g_fat[cluster];
        g_fat[cluster] = FS_FAT_FREE;
        if (next == FS_FAT_EOC) {
            break;
        }
        cluster = next;
    }
}

static int fs_lookup_entry(const char *path, struct fs_dir_entry *entry,
                           uint32_t *parent_cluster,
                           uint32_t *entry_cluster,
                           uint32_t *entry_index) {
    if (path == NULL || *path == '\0') {
        return -1;
    }

    if (path[0] == '/' && path[1] == '\0') {
        if (entry) {
            fs_clear_entry(entry);
            entry->type = FS_ENTRY_DIR;
            entry->first_cluster = g_sb.root_dir_cluster;
        }
        if (parent_cluster) {
            *parent_cluster = 0;
        }
        if (entry_cluster) {
            *entry_cluster = 0;
        }
        if (entry_index) {
            *entry_index = 0;
        }
        return 0;
    }

    char name[16];
    uint32_t parent = 0;
    if (fs_split_path(path, &parent, name, sizeof(name), NULL, 0) != 0) {
        return -1;
    }

    if (fs_find_entry(parent, name, entry, entry_cluster, entry_index) != 0) {
        return -1;
    }

    if (parent_cluster) {
        *parent_cluster = parent;
    }

    return 0;
}

int fs_stat(const char *path, int *type, unsigned int *size) {
    if (!g_ready) {
        fs_error("Storage is offline.");
        return -1;
    }

    struct fs_dir_entry entry;
    if (fs_lookup_entry(path, &entry, NULL, NULL, NULL) != 0) {
        return -1;
    }

    if (type) {
        *type = (int) entry.type;
    }
    if (size) {
        *size = entry.size;
    }
    return 0;
}

int fs_read_file(const char *path, unsigned char *buffer, unsigned int max_size, unsigned int *out_size) {
    if (!g_ready) {
        fs_error("Storage is offline.");
        return -1;
    }

    struct fs_dir_entry entry;
    if (fs_lookup_entry(path, &entry, NULL, NULL, NULL) != 0) {
        return -1;
    }

    if (entry.type != FS_ENTRY_FILE) {
        return -1;
    }

    if (entry.size > max_size) {
        return -1;
    }

    if (out_size) {
        *out_size = entry.size;
    }

    if (entry.size == 0 || entry.first_cluster == 0) {
        return 0;
    }

    uint8_t sector[FS_SECTOR_SIZE];
    uint32_t remaining = entry.size;
    uint32_t cluster = entry.first_cluster;
    unsigned int offset = 0;

    while (cluster != FS_FAT_EOC && remaining > 0) {
        if (fs_read_cluster(cluster, sector) != 0) {
            return -1;
        }

        uint32_t chunk = remaining;
        if (chunk > FS_SECTOR_SIZE) {
            chunk = FS_SECTOR_SIZE;
        }

        k_memcpy(buffer + offset, sector, chunk);
        offset += chunk;
        remaining -= chunk;
        cluster = g_fat[cluster];
    }

    return 0;
}

int fs_write_file(const char *path, const unsigned char *data, unsigned int size, int overwrite) {
    if (!g_ready) {
        fs_error("Storage is offline.");
        return -1;
    }

    if (path == NULL || *path == '\0' || (path[0] == '/' && path[1] == '\0')) {
        fs_error("Invalid path.");
        return -1;
    }

    struct fs_dir_entry entry;
    uint32_t parent = 0;
    uint32_t entry_cluster = 0;
    uint32_t entry_index = 0;
    int exists = (fs_lookup_entry(path, &entry, &parent, &entry_cluster, &entry_index) == 0);

    if (exists) {
        if (entry.type != FS_ENTRY_FILE) {
            fs_error("Not a file.");
            return -1;
        }
        if (!overwrite) {
            fs_error("File exists.");
            return -1;
        }
        if (entry.first_cluster != 0) {
            fs_free_chain(entry.first_cluster);
        }
    } else {
        char name[16];
        if (fs_split_path(path, &parent, name, sizeof(name), NULL, 0) != 0) {
            fs_error("Invalid path.");
            return -1;
        }
        fs_clear_entry(&entry);
        k_memcpy(entry.name, name, k_strlen(name) + 1);
        entry.type = FS_ENTRY_FILE;
    }

    uint32_t first_cluster = 0;
    if (size > 0) {
        unsigned int needed = (size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
        uint32_t prev = 0;

        for (unsigned int i = 0; i < needed; ++i) {
            uint32_t cluster = fs_alloc_cluster();
            if (cluster == 0) {
                if (first_cluster != 0) {
                    fs_free_chain(first_cluster);
                }
                fs_error("No free space.");
                return -1;
            }
            if (first_cluster == 0) {
                first_cluster = cluster;
            }
            if (prev != 0) {
                g_fat[prev] = cluster;
            }
            prev = cluster;
        }
        if (prev != 0) {
            g_fat[prev] = FS_FAT_EOC;
        }

        uint8_t sector[FS_SECTOR_SIZE];
        unsigned int offset = 0;
        uint32_t cluster = first_cluster;
        while (cluster != FS_FAT_EOC && offset < size) {
            k_memset(sector, 0, sizeof(sector));
            unsigned int chunk = size - offset;
            if (chunk > FS_SECTOR_SIZE) {
                chunk = FS_SECTOR_SIZE;
            }
            k_memcpy(sector, data + offset, chunk);
            if (fs_write_cluster(cluster, sector) != 0) {
                fs_error("Write failed.");
                return -1;
            }
            offset += chunk;
            cluster = g_fat[cluster];
        }
    }

    entry.first_cluster = first_cluster;
    entry.size = size;

    if (fs_flush_fat() != 0) {
        fs_error("Failed to update FAT.");
        return -1;
    }

    if (exists) {
        if (fs_write_entry_at(entry_cluster, entry_index, &entry) != 0) {
            fs_error("Failed to update file entry.");
            return -1;
        }
    } else {
        if (fs_add_entry(parent, &entry) != 0) {
            fs_error("Failed to add file entry.");
            return -1;
        }
    }

    return 0;
}

int fs_remove_file(const char *path) {
    if (!g_ready) {
        fs_error("Storage is offline.");
        return -1;
    }

    struct fs_dir_entry entry;
    uint32_t parent = 0;
    uint32_t entry_cluster = 0;
    uint32_t entry_index = 0;
    if (fs_lookup_entry(path, &entry, &parent, &entry_cluster, &entry_index) != 0) {
        fs_error("File not found.");
        return -1;
    }

    if (entry.type != FS_ENTRY_FILE) {
        fs_error("Not a file.");
        return -1;
    }

    if (entry.first_cluster != 0) {
        fs_free_chain(entry.first_cluster);
    }

    fs_clear_entry(&entry);
    if (fs_write_entry_at(entry_cluster, entry_index, &entry) != 0) {
        fs_error("Failed to remove file.");
        return -1;
    }

    if (fs_flush_fat() != 0) {
        fs_error("Failed to update FAT.");
        return -1;
    }

    return 0;
}

static int fs_dir_is_empty(uint32_t dir_cluster) {
    uint8_t sector[FS_SECTOR_SIZE];
    uint32_t cluster = dir_cluster;

    while (cluster != FS_FAT_EOC) {
        if (fs_read_cluster(cluster, sector) != 0) {
            return 0;
        }

        struct fs_dir_entry *entries = (struct fs_dir_entry *) sector;
        for (uint32_t i = 0; i < FS_SECTOR_SIZE / sizeof(struct fs_dir_entry); ++i) {
            if (entries[i].name[0] == '\0') {
                continue;
            }
            if (k_strcmp(entries[i].name, ".") == 0 || k_strcmp(entries[i].name, "..") == 0) {
                continue;
            }
            return 0;
        }

        uint32_t next = g_fat[cluster];
        if (next == FS_FAT_EOC || next == FS_FAT_FREE) {
            break;
        }
        cluster = next;
    }

    return 1;
}

static int fs_remove_dir_contents(uint32_t dir_cluster) {
    uint8_t sector[FS_SECTOR_SIZE];
    uint32_t cluster = dir_cluster;

    while (cluster != FS_FAT_EOC) {
        if (fs_read_cluster(cluster, sector) != 0) {
            return -1;
        }

        struct fs_dir_entry *entries = (struct fs_dir_entry *) sector;
        int changed = 0;

        for (uint32_t i = 0; i < FS_SECTOR_SIZE / sizeof(struct fs_dir_entry); ++i) {
            if (entries[i].name[0] == '\0') {
                continue;
            }
            if (k_strcmp(entries[i].name, ".") == 0 || k_strcmp(entries[i].name, "..") == 0) {
                continue;
            }

            if (entries[i].type == FS_ENTRY_FILE) {
                if (entries[i].first_cluster != 0) {
                    fs_free_chain(entries[i].first_cluster);
                }
                fs_clear_entry(&entries[i]);
                changed = 1;
                continue;
            }

            if (entries[i].type == FS_ENTRY_DIR) {
                if (fs_remove_dir_contents(entries[i].first_cluster) != 0) {
                    return -1;
                }
                fs_free_chain(entries[i].first_cluster);
                fs_clear_entry(&entries[i]);
                changed = 1;
            }
        }

        if (changed) {
            if (fs_write_cluster(cluster, sector) != 0) {
                return -1;
            }
        }

        uint32_t next = g_fat[cluster];
        if (next == FS_FAT_EOC || next == FS_FAT_FREE) {
            break;
        }
        cluster = next;
    }

    return 0;
}

int fs_remove_dir(const char *path, int recursive) {
    if (!g_ready) {
        fs_error("Storage is offline.");
        return -1;
    }

    if (path != NULL && path[0] == '/' && path[1] == '\0') {
        fs_error("Cannot remove root.");
        return -1;
    }

    struct fs_dir_entry entry;
    uint32_t parent = 0;
    uint32_t entry_cluster = 0;
    uint32_t entry_index = 0;
    if (fs_lookup_entry(path, &entry, &parent, &entry_cluster, &entry_index) != 0) {
        fs_error("Directory not found.");
        return -1;
    }

    if (entry.type != FS_ENTRY_DIR) {
        fs_error("Not a directory.");
        return -1;
    }

    if (!recursive) {
        if (!fs_dir_is_empty(entry.first_cluster)) {
            fs_error("Directory not empty.");
            return -1;
        }
    } else {
        if (fs_remove_dir_contents(entry.first_cluster) != 0) {
            fs_error("Failed to remove directory.");
            return -1;
        }
    }

    fs_free_chain(entry.first_cluster);
    fs_clear_entry(&entry);
    if (fs_write_entry_at(entry_cluster, entry_index, &entry) != 0) {
        fs_error("Failed to remove directory.");
        return -1;
    }

    if (fs_flush_fat() != 0) {
        fs_error("Failed to update FAT.");
        return -1;
    }

    return 0;
}

static int fs_resolve_dir(const char *path, uint32_t *out_cluster, char *out_path, size_t path_size) {
    char component[16];
    const char *cursor = path;
    uint32_t current = 0;

    if (path == NULL || *path == '\0') {
        if (out_cluster) {
            *out_cluster = g_cwd_cluster;
        }
        if (out_path) {
            k_memcpy(out_path, g_cwd_path, k_strlen(g_cwd_path) + 1);
        }
        return 0;
    }

    if (path[0] == '/') {
        current = g_sb.root_dir_cluster;
        if (out_path) {
            k_memset(out_path, 0, path_size);
            out_path[0] = '/';
            out_path[1] = '\0';
        }
    } else {
        current = g_cwd_cluster;
        if (out_path) {
            k_memcpy(out_path, g_cwd_path, k_strlen(g_cwd_path) + 1);
        }
    }

    while ((cursor = fs_next_component(cursor, component, sizeof(component))) != NULL) {
        if (k_strcmp(component, ".") == 0) {
            cursor = fs_has_more(cursor) ? cursor : NULL;
            if (cursor == NULL) {
                break;
            }
            continue;
        }

        if (k_strcmp(component, "..") == 0) {
            struct fs_dir_entry entry;
            if (fs_find_entry(current, "..", &entry, NULL, NULL) != 0) {
                return -1;
            }
            current = entry.first_cluster;
            if (out_path) {
                fs_path_pop(out_path);
            }

            cursor = fs_has_more(cursor) ? cursor : NULL;
            if (cursor == NULL) {
                break;
            }
            continue;
        }

        struct fs_dir_entry entry;
        if (fs_find_entry(current, component, &entry, NULL, NULL) != 0) {
            return -1;
        }
        if (entry.type != FS_ENTRY_DIR) {
            return -1;
        }
        current = entry.first_cluster;
        if (out_path) {
            if (fs_path_append(out_path, path_size, component) != 0) {
                return -1;
            }
        }

        cursor = fs_has_more(cursor) ? cursor : NULL;
        if (cursor == NULL) {
            break;
        }
    }

    if (out_cluster) {
        *out_cluster = current;
    }

    return 0;
}

static int fs_split_path(const char *path, uint32_t *parent_out, char *name_out, size_t name_size, char *parent_path, size_t parent_path_size) {
    char component[16];
    const char *cursor = path;
    uint32_t current;

    if (path == NULL || *path == '\0') {
        return -1;
    }

    if (path[0] == '/') {
        current = g_sb.root_dir_cluster;
        if (parent_path) {
            k_memset(parent_path, 0, parent_path_size);
            parent_path[0] = '/';
            parent_path[1] = '\0';
        }
    } else {
        current = g_cwd_cluster;
        if (parent_path) {
            k_memcpy(parent_path, g_cwd_path, k_strlen(g_cwd_path) + 1);
        }
    }

    while ((cursor = fs_next_component(cursor, component, sizeof(component))) != NULL) {
        int last = !fs_has_more(cursor);

        if (last) {
            if (k_strlen(component) == 0 || k_strlen(component) > FS_NAME_MAX) {
                return -1;
            }
            k_memset(name_out, 0, name_size);
            k_memcpy(name_out, component, k_strlen(component) + 1);
            if (parent_out) {
                *parent_out = current;
            }
            return 0;
        }

        if (k_strcmp(component, ".") == 0) {
            continue;
        }

        if (k_strcmp(component, "..") == 0) {
            struct fs_dir_entry entry;
            if (fs_find_entry(current, "..", &entry, NULL, NULL) != 0) {
                return -1;
            }
            current = entry.first_cluster;
            if (parent_path) {
                fs_path_pop(parent_path);
            }
            continue;
        }

        struct fs_dir_entry entry;
        if (fs_find_entry(current, component, &entry, NULL, NULL) != 0) {
            return -1;
        }
        if (entry.type != FS_ENTRY_DIR) {
            return -1;
        }
        current = entry.first_cluster;
        if (parent_path) {
            if (fs_path_append(parent_path, parent_path_size, component) != 0) {
                return -1;
            }
        }
    }

    return -1;
}

int fs_init(void) {
    uint8_t sector[FS_SECTOR_SIZE];

    if (disk_init() != 0) {
        fs_error("No writable disk found. Attach an IDE disk.");
        g_ready = 0;
        return -1;
    }

    if (fs_read_sector(0, sector) != 0) {
        fs_error("Failed to read disk.");
        g_ready = 0;
        return -1;
    }

    k_memcpy(&g_sb, sector, sizeof(g_sb));
    if (g_sb.magic != FS_MAGIC || g_sb.version != FS_VERSION) {
        console_writeln("Formatting storage...");
        if (fs_format() != 0) {
            g_ready = 0;
            return -1;
        }
    }

    g_fat_entries = g_sb.cluster_count + 2;
    if (g_fat_entries > FS_MAX_CLUSTERS + 2) {
        fs_error("Filesystem too large for memory.");
        g_ready = 0;
        return -1;
    }

    if (fs_load_fat() != 0) {
        fs_error("Failed to load FAT.");
        g_ready = 0;
        return -1;
    }

    g_cwd_cluster = g_sb.root_dir_cluster;
    g_cwd_path[0] = '/';
    g_cwd_path[1] = '\0';
    g_ready = 1;
    return 0;
}

const char *fs_get_cwd(void) {
    if (!g_ready) {
        return "/offline";
    }
    return g_cwd_path;
}

int fs_is_ready(void) {
    return g_ready;
}

unsigned int fs_total_sectors(void) {
    if (!g_ready) {
        return 0;
    }
    return g_sb.total_sectors;
}

unsigned int fs_cluster_count(void) {
    if (!g_ready) {
        return 0;
    }
    return g_sb.cluster_count;
}

int fs_ls(const char *path) {
    if (!g_ready) {
        fs_error("Storage is offline.");
        return -1;
    }

    uint32_t dir_cluster = 0;
    if (fs_resolve_dir(path, &dir_cluster, NULL, 0) != 0) {
        fs_error("Directory not found.");
        return -1;
    }

    uint8_t sector[FS_SECTOR_SIZE];
    uint32_t cluster = dir_cluster;
    int printed = 0;

    while (cluster != FS_FAT_EOC) {
        if (fs_read_cluster(cluster, sector) != 0) {
            fs_error("Failed to read directory.");
            return -1;
        }

        struct fs_dir_entry *entries = (struct fs_dir_entry *) sector;
        for (uint32_t i = 0; i < FS_SECTOR_SIZE / sizeof(struct fs_dir_entry); ++i) {
            if (entries[i].name[0] == '\0') {
                continue;
            }
            if (k_strcmp(entries[i].name, ".") == 0 || k_strcmp(entries[i].name, "..") == 0) {
                continue;
            }
            if (entries[i].type == FS_ENTRY_DIR) {
                console_write("DIR  ");
                console_write(entries[i].name);
                console_write("/");
                console_putc('\n');
                printed = 1;
                continue;
            }
            console_write("FILE ");
            console_write(entries[i].name);
            console_write(" ");
            console_write_dec((int) entries[i].size);
            console_writeln("B");
            printed = 1;
        }

        uint32_t next = g_fat[cluster];
        if (next == FS_FAT_EOC || next == FS_FAT_FREE) {
            break;
        }
        cluster = next;
    }

    if (!printed) {
        console_writeln("(empty)");
    }

    return 0;
}

int fs_cd(const char *path) {
    if (!g_ready) {
        fs_error("Storage is offline.");
        return -1;
    }

    uint32_t target = 0;
    char new_path[128];

    if (fs_resolve_dir(path, &target, new_path, sizeof(new_path)) != 0) {
        fs_error("Directory not found.");
        return -1;
    }

    g_cwd_cluster = target;
    k_memcpy(g_cwd_path, new_path, sizeof(g_cwd_path));
    return 0;
}

int fs_mkdir(const char *path) {
    if (!g_ready) {
        fs_error("Storage is offline.");
        return -1;
    }

    uint32_t parent = 0;
    char name[16];
    char parent_path[128];

    if (fs_split_path(path, &parent, name, sizeof(name), parent_path, sizeof(parent_path)) != 0) {
        fs_error("Invalid path.");
        return -1;
    }

    if (fs_find_entry(parent, name, NULL, NULL, NULL) == 0) {
        fs_error("Name already exists.");
        return -1;
    }

    uint32_t cluster = fs_alloc_cluster();
    if (cluster == 0) {
        fs_error("No free space.");
        return -1;
    }

    if (fs_flush_fat() != 0) {
        fs_error("Failed to update FAT.");
        return -1;
    }

    if (fs_init_dir_cluster(cluster, parent) != 0) {
        fs_error("Failed to create directory.");
        return -1;
    }

    struct fs_dir_entry entry;
    k_memset(&entry, 0, sizeof(entry));
    k_memcpy(entry.name, name, k_strlen(name) + 1);
    entry.type = FS_ENTRY_DIR;
    entry.size = 0;
    entry.first_cluster = cluster;

    if (fs_add_entry(parent, &entry) != 0) {
        fs_error("Failed to add directory entry.");
        return -1;
    }

    return 0;
}

int fs_touch(const char *path) {
    if (!g_ready) {
        fs_error("Storage is offline.");
        return -1;
    }

    uint32_t parent = 0;
    char name[16];

    if (fs_split_path(path, &parent, name, sizeof(name), NULL, 0) != 0) {
        fs_error("Invalid path.");
        return -1;
    }

    if (fs_find_entry(parent, name, NULL, NULL, NULL) == 0) {
        fs_error("Name already exists.");
        return -1;
    }

    struct fs_dir_entry entry;
    k_memset(&entry, 0, sizeof(entry));
    k_memcpy(entry.name, name, k_strlen(name) + 1);
    entry.type = FS_ENTRY_FILE;
    entry.size = 0;
    entry.first_cluster = 0;

    if (fs_add_entry(parent, &entry) != 0) {
        fs_error("Failed to create file.");
        return -1;
    }

    return 0;
}

int fs_cat(const char *path) {
    if (!g_ready) {
        fs_error("Storage is offline.");
        return -1;
    }

    uint32_t parent = 0;
    char name[16];

    if (fs_split_path(path, &parent, name, sizeof(name), NULL, 0) != 0) {
        fs_error("Invalid path.");
        return -1;
    }

    struct fs_dir_entry entry;
    if (fs_find_entry(parent, name, &entry, NULL, NULL) != 0) {
        fs_error("File not found.");
        return -1;
    }

    if (entry.type != FS_ENTRY_FILE) {
        fs_error("Not a file.");
        return -1;
    }

    if (entry.size == 0 || entry.first_cluster == 0) {
        console_writeln("(empty file)");
        return 0;
    }

    uint8_t sector[FS_SECTOR_SIZE];
    uint32_t remaining = entry.size;
    uint32_t cluster = entry.first_cluster;

    while (cluster != FS_FAT_EOC && remaining > 0) {
        if (fs_read_cluster(cluster, sector) != 0) {
            fs_error("Failed to read file.");
            return -1;
        }

        uint32_t chunk = remaining;
        if (chunk > FS_SECTOR_SIZE) {
            chunk = FS_SECTOR_SIZE;
        }

        for (uint32_t i = 0; i < chunk; ++i) {
            console_putc((char) sector[i]);
        }

        remaining -= chunk;
        cluster = g_fat[cluster];
    }

    console_putc('\n');
    return 0;
}
