#ifndef KOSOS_FS_H
#define KOSOS_FS_H

#include <stddef.h>

#define FS_ENTRY_DIR 1
#define FS_ENTRY_FILE 2

int fs_init(void);
const char *fs_get_cwd(void);
int fs_is_ready(void);
unsigned int fs_total_sectors(void);
unsigned int fs_cluster_count(void);
int fs_stat(const char *path, int *type, unsigned int *size);
int fs_read_file(const char *path, unsigned char *buffer, unsigned int max_size, unsigned int *out_size);
int fs_write_file(const char *path, const unsigned char *data, unsigned int size, int overwrite);
int fs_remove_file(const char *path);
int fs_remove_dir(const char *path, int recursive);
int fs_ls(const char *path);
int fs_cd(const char *path);
int fs_mkdir(const char *path);
int fs_touch(const char *path);
int fs_cat(const char *path);

#endif
