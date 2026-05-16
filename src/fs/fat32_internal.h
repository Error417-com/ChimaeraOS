/*
 * ChimaeraOS - FAT32 Driver — Internal Header
 * fs/fat32_internal.h
 *
 * Shared types, structures, constants, global declarations, and pure
 * helper functions used across the three FAT32 translation units:
 *
 *   fat32_core.c  — volume state, sector I/O, FAT/dir helpers, mount
 *   fat32_ops.c   — public file and directory operations
 *   fat32_fsck.c  — FSCK bridge API
 *
 * This header is #included by each of those files.  It is NOT part of
 * the public driver API; use fat32.h for that.
 */
#ifndef FAT32_INTERNAL_H
#define FAT32_INTERNAL_H

#include "../include/fat32.h"
#include "../include/fsck.h"
#include "../include/ata.h"
#include "../include/mm.h"
#include "../include/vga.h"
#include "../include/serial.h"
#include "../include/types.h"

/* ── Constants ───────────────────────────────────────────────────────────── */

#define SECTOR_SIZE     512
#define MAX_CLUSTER_SZ  (64 * 512)  /* 32 KB max cluster */
#define CACHE_SECTORS   128         /* 64 KB sector cache */

#define DIR_ENTRY_SIZE  32
#define ENTRIES_PER_SEC (SECTOR_SIZE / DIR_ENTRY_SIZE)

#define FAT32_EOC  0x0FFFFFF8
#define FAT32_FREE 0x00000000
#define FAT32_BAD  0x0FFFFFF7

/* ── FAT32 on-disk structures ────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  jump[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;    /* 0 for FAT32 */
    uint16_t total_sectors_16;    /* 0 for FAT32 */
    uint8_t  media_type;
    uint16_t fat_size_16;         /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended BPB */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;        /* Usually 2 */
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];          /* "FAT32   " */
} fat32_bpb_t;

/* Standard 8.3 directory entry */
typedef struct __attribute__((packed)) {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;   /* High 16 bits of first cluster */
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;   /* Low 16 bits of first cluster */
    uint32_t file_size;
} fat32_dir_entry_t;

/* LFN directory entry */
typedef struct __attribute__((packed)) {
    uint8_t  order;          /* Sequence number (bit 6 = last entry) */
    uint16_t name1[5];       /* Characters 1-5 */
    uint8_t  attr;           /* Always 0x0F */
    uint8_t  type;           /* Always 0 */
    uint8_t  checksum;
    uint16_t name2[6];       /* Characters 6-11 */
    uint16_t fst_clus_lo;    /* Always 0 */
    uint16_t name3[2];       /* Characters 12-13 */
} fat32_lfn_entry_t;

/* MBR partition entry */
typedef struct __attribute__((packed)) {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t lba_size;
} mbr_partition_t;

/* Directory iterator position */
typedef struct {
    uint32_t cluster;    /* Cluster of the directory */
    uint32_t entry_idx;  /* Entry index within directory */
    uint32_t lba;        /* LBA of the sector containing this entry */
    uint32_t off;        /* Byte offset within that sector */
} dir_pos_t;

/* ── Global variable declarations (defined in fat32_core.c) ──────────────── */

extern bool     vol_mounted;
extern uint32_t vol_lba_start;
extern uint32_t vol_bytes_per_sec;
extern uint32_t vol_sec_per_clus;
extern uint32_t vol_reserved_sec;
extern uint32_t vol_num_fats;
extern uint32_t vol_fat_size;
extern uint32_t vol_root_cluster;
extern uint32_t vol_fat_lba;
extern uint32_t vol_data_lba;
extern uint32_t vol_total_clusters;

extern uint8_t  sec_cache[SECTOR_SIZE];
extern uint32_t sec_cache_lba;
extern bool     sec_cache_dirty;

extern fat32_fd_t fds[FAT32_MAX_FD];

extern bool vol_readonly;

/* ── Pure helper functions (static inline — no global state) ─────────────── */

static inline void f_memset(void *d, uint8_t v, uint32_t n)
{ uint8_t *p=(uint8_t*)d; while(n--)*p++=v; }

static inline void f_memcpy(void *d, const void *s, uint32_t n)
{ uint8_t *dd=(uint8_t*)d; const uint8_t *ss=(const uint8_t*)s; while(n--)*dd++=*ss++; }

static inline int f_memcmp(const void *a, const void *b, uint32_t n)
{ const uint8_t *aa=(const uint8_t*)a,*bb=(const uint8_t*)b;
  while(n--){if(*aa!=*bb)return(int)*aa-(int)*bb;aa++;bb++;} return 0; }

static inline uint32_t f_strlen(const char *s)
{ uint32_t n=0; while(s[n])n++; return n; }

static inline void f_strcpy(char *d, const char *s)
{ while((*d++=*s++)); }

static inline int f_strcmp(const char *a, const char *b)
{ while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }

static inline int f_strncmp(const char *a, const char *b, uint32_t n)
{ while(n--&&*a&&*a==*b){a++;b++;} if(!n)return 0; return (unsigned char)*a-(unsigned char)*b; }

static inline char f_toupper(char c)
{ return (c>='a'&&c<='z')?(c-32):c; }

/* Compute 8.3 checksum for LFN entries */
static inline uint8_t lfn_checksum(const char name83[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)name83[i];
    }
    return sum;
}

/* Extract LFN characters from an LFN entry into a buffer (UCS-2 → ASCII) */
static inline void lfn_extract(const fat32_lfn_entry_t *e, char *out13)
{
    int i = 0;
    for (int j = 0; j < 5; j++) out13[i++] = (char)(e->name1[j] & 0xFF);
    for (int j = 0; j < 6; j++) out13[i++] = (char)(e->name2[j] & 0xFF);
    for (int j = 0; j < 2; j++) out13[i++] = (char)(e->name3[j] & 0xFF);
}

/* Build a short 8.3 name from a long name (simplified: uppercase, truncate) */
static inline void make_83(const char *name, char out[11])
{
    f_memset(out, ' ', 11);
    int dot = -1;
    uint32_t len = f_strlen(name);
    for (int i = (int)len - 1; i >= 0; i--) {
        if (name[i] == '.') { dot = i; break; }
    }
    int ni = 0;
    for (uint32_t i = 0; i < len && ni < 8; i++) {
        if ((int)i == dot) break;
        char c = f_toupper(name[i]);
        if (c == ' ' || c == '.') continue;
        out[ni++] = c;
    }
    if (dot >= 0) {
        int ei = 8;
        for (int i = dot + 1; i < (int)len && ei < 11; i++) {
            out[ei++] = f_toupper(name[i]);
        }
    }
}

/* ── Non-pure helper declarations (defined in fat32_core.c) ──────────────── */

bool     sec_flush(void);
bool     sec_read(uint32_t lba);
bool     sec_write(uint32_t lba, const uint8_t *data);

uint32_t cluster_to_lba(uint32_t cluster);
uint32_t fat_lba_for_cluster(uint32_t cluster);
uint32_t fat_offset_in_sector(uint32_t cluster);
uint32_t fat_get(uint32_t cluster);
bool     fat_set(uint32_t cluster, uint32_t value);
uint32_t fat_alloc(uint32_t prev);
void     fat_free_chain(uint32_t start);
uint32_t fat_follow(uint32_t start, uint32_t n);

bool     dir_read_entry(const dir_pos_t *pos, fat32_dir_entry_t *out);
bool     dir_write_entry(const dir_pos_t *pos, const fat32_dir_entry_t *e);
bool     dir_next(dir_pos_t *pos);
void     dir_pos_init(dir_pos_t *pos, uint32_t cluster);
bool     dir_83_exists(uint32_t dir_cluster, const char name83[11]);
void     make_83_unique(uint32_t dir_cluster, const char *name, char out[11]);
bool     dir_find(uint32_t dir_cluster, const char *name,
                  fat32_dir_entry_t *out_entry, dir_pos_t *out_pos,
                  dir_pos_t *out_lfn_start);

uint32_t path_resolve_parent(const char *path, char *name_out);
bool     path_stat(const char *path, fat32_dir_entry_t *out_e,
                   dir_pos_t *out_pos, uint32_t *out_dir_cluster);

/* dir_add_entry is defined in fat32_ops.c but used by fat32_fsck.c */
bool     dir_add_entry(uint32_t dir_cluster, const char *name,
                       uint8_t attr, uint32_t first_cluster,
                       uint32_t file_size, dir_pos_t *out_pos);

#endif /* FAT32_INTERNAL_H */
