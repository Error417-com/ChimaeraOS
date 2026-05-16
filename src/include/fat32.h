/*
 * ChimaeraOS - FAT32 Persistent Disk Driver Header
 * include/fat32.h
 *
 * Provides a FAT32 filesystem driver backed by the ATA slave drive.
 * Mounted at /persist — all user files, settings, and models live here.
 *
 * Supports:
 *   - File creation, read, write, append, delete
 *   - Directory creation, listing, deletion
 *   - Long File Names (LFN) — up to 255 chars
 *   - Files up to 4 GiB (FAT32 limit)
 *   - Shell commands: ls, cp, mv, rm, mkdir on /persist
 */
#ifndef FAT32_H
#define FAT32_H

#include "types.h"

/* Maximum path length */
#define FAT32_MAX_PATH   256
/* Maximum filename length (LFN) */
#define FAT32_MAX_NAME   256
/* Maximum open files */
#define FAT32_MAX_FD     16

/* File attributes */
#define FAT32_ATTR_RDONLY  0x01
#define FAT32_ATTR_HIDDEN  0x02
#define FAT32_ATTR_SYSTEM  0x04
#define FAT32_ATTR_VOLID   0x08
#define FAT32_ATTR_DIR     0x10
#define FAT32_ATTR_ARCHIVE 0x20
#define FAT32_ATTR_LFN     0x0F

/* Error codes */
typedef enum {
    FAT32_OK           =  0,
    FAT32_ERR_NOTFOUND = -1,
    FAT32_ERR_EXISTS   = -2,
    FAT32_ERR_FULL     = -3,
    FAT32_ERR_IO       = -4,
    FAT32_ERR_NOTDIR   = -5,
    FAT32_ERR_ISDIR    = -6,
    FAT32_ERR_NOMEM    = -7,
    FAT32_ERR_NODEV    = -8,
    FAT32_ERR_NOTEMPTY = -9,
} fat32_err_t;

/* Directory entry info (returned by fat32_stat / fat32_readdir) */
typedef struct {
    char     name[FAT32_MAX_NAME];
    uint32_t size;          /* File size in bytes (0 for dirs) */
    uint8_t  attr;          /* FAT32_ATTR_* flags */
    bool     is_dir;
    uint32_t cluster;       /* Starting cluster */
} fat32_dirent_t;

/* File descriptor */
typedef struct {
    bool     open;
    bool     write;         /* Opened for writing */
    uint32_t cluster;       /* Starting cluster of file */
    uint32_t dir_cluster;   /* Cluster of parent directory */
    uint32_t dir_lba;       /* LBA of sector containing the 8.3 dir entry */
    uint32_t dir_offset;    /* Byte offset of 8.3 dir entry within that sector */
    uint32_t size;          /* File size */
    uint32_t pos;           /* Current read/write position */
    uint32_t cur_cluster;   /* Current cluster during sequential I/O */
} fat32_fd_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Mount the FAT32 filesystem from the ATA slave drive.
 * If the disk is blank/unformatted, formats it with a fresh FAT32 volume.
 * Returns true on success. */
bool fat32_mount(void);

/* Return true if the filesystem is mounted. */
bool fat32_mounted(void);

/* ── File operations ─────────────────────────────────────────────────────── */

/* Open a file. If `write` is true and file doesn't exist, creates it.
 * Returns a file descriptor index >= 0, or < 0 on error. */
int fat32_open(const char *path, bool write);

/* Read up to `len` bytes from fd into buf. Returns bytes read, 0=EOF, <0=err. */
int fat32_read(int fd, void *buf, uint32_t len);

/* Write `len` bytes from buf to fd. Returns bytes written, <0=err. */
int fat32_write(int fd, const void *buf, uint32_t len);

/* Seek to absolute position. Returns FAT32_OK or error. */
fat32_err_t fat32_seek(int fd, uint32_t pos);

/* Close a file descriptor. */
void fat32_close(int fd);

/* Get file size. Returns -1 if not found. */
int32_t fat32_filesize(const char *path);

/* ── Directory operations ────────────────────────────────────────────────── */

/* Create a directory (and all parents). Returns FAT32_OK or error. */
fat32_err_t fat32_mkdir(const char *path);

/* List directory contents. Calls `cb(entry, userdata)` for each entry.
 * Returns number of entries, or < 0 on error. */
int fat32_readdir(const char *path,
                  void (*cb)(const fat32_dirent_t *e, void *ud),
                  void *userdata);

/* Stat a path. Returns FAT32_OK and fills `out`, or error. */
fat32_err_t fat32_stat(const char *path, fat32_dirent_t *out);

/* ── File management ─────────────────────────────────────────────────────── */

/* Delete a file. Returns FAT32_OK or error. */
fat32_err_t fat32_unlink(const char *path);

/* Rename/move a file or directory. Returns FAT32_OK or error. */
fat32_err_t fat32_rename(const char *src, const char *dst);

/* Copy a file. Returns FAT32_OK or error. */
fat32_err_t fat32_copy(const char *src, const char *dst);

/* ── Convenience helpers ─────────────────────────────────────────────────── */

/* Read entire file into a malloc'd buffer. Caller must kfree().
 * Returns pointer and sets *size, or NULL on error. */
void *fat32_read_file(const char *path, uint32_t *size);

/* Write an entire buffer to a file (creates or truncates).
 * Returns FAT32_OK or error. */
fat32_err_t fat32_write_file(const char *path, const void *data, uint32_t len);

/* Append data to a file (creates if not exists). */
fat32_err_t fat32_append_file(const char *path, const void *data, uint32_t len);

/* Human-readable error string */
const char *fat32_strerror(fat32_err_t err);

/* Print filesystem statistics to VGA console */
void fat32_print_stats(void);

#endif /* FAT32_H */
