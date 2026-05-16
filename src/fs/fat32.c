/*
 * ChimaeraOS - FAT32 Persistent Disk Driver
 * fs/fat32.c
 *
 * FAT32 driver backed by the ATA slave drive (chimaera.img).
 * Supports LFN, file/dir CRUD, and is the persistent storage layer.
 *
 * Layout of chimaera.img (64 MB default):
 *   Sector 0        : MBR (partition table)
 *   Sector 2048+    : FAT32 volume (partition 1)
 *     Sectors 0-31  : Reserved (BPB + FSInfo + backup boot)
 *     Sectors 32+   : FAT tables (2 copies)
 *     After FATs    : Data clusters (cluster 2 = root dir)
 */
#include "../include/fat32.h"
#include "../include/fsck.h"
#include "../include/ata.h"
#include "../include/mm.h"
#include "../include/vga.h"
#include "../include/serial.h"
#include "../include/types.h"

/* ── Memory helpers ──────────────────────────────────────────────────────── */
static void f_memset(void *d, uint8_t v, uint32_t n)
{ uint8_t *p=(uint8_t*)d; while(n--)*p++=v; }
static void f_memcpy(void *d, const void *s, uint32_t n)
{ uint8_t *dd=(uint8_t*)d; const uint8_t *ss=(const uint8_t*)s; while(n--)*dd++=*ss++; }
static int f_memcmp(const void *a, const void *b, uint32_t n)
{ const uint8_t *aa=(const uint8_t*)a,*bb=(const uint8_t*)b;
  while(n--){if(*aa!=*bb)return(int)*aa-(int)*bb;aa++;bb++;} return 0; }
static uint32_t f_strlen(const char *s){uint32_t n=0;while(s[n])n++;return n;}
static void f_strcpy(char *d, const char *s){while((*d++=*s++));}
static int f_strcmp(const char *a, const char *b)
{ while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
static int f_strncmp(const char *a, const char *b, uint32_t n)
{ while(n--&&*a&&*a==*b){a++;b++;} if(!n)return 0; return (unsigned char)*a-(unsigned char)*b; }
static char f_toupper(char c){ return (c>='a'&&c<='z')?(c-32):c; }

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

/* ── Volume state ────────────────────────────────────────────────────────── */
#define SECTOR_SIZE     512
#define MAX_CLUSTER_SZ  (64 * 512)  /* 32 KB max cluster */
#define CACHE_SECTORS   128         /* 64 KB sector cache */

static bool     vol_mounted      = false;
static uint32_t vol_lba_start    = 0;    /* LBA of FAT32 volume start */
static uint32_t vol_bytes_per_sec= 512;
static uint32_t vol_sec_per_clus = 0;
static uint32_t vol_reserved_sec = 0;
static uint32_t vol_num_fats     = 0;
static uint32_t vol_fat_size     = 0;
static uint32_t vol_root_cluster = 2;
static uint32_t vol_fat_lba      = 0;    /* LBA of FAT1 */
static uint32_t vol_data_lba     = 0;    /* LBA of first data cluster */
static uint32_t vol_total_clusters = 0;

/* Sector cache: one sector at a time */
static uint8_t  sec_cache[SECTOR_SIZE];
static uint32_t sec_cache_lba  = 0xFFFFFFFF;
static bool     sec_cache_dirty = false;

/* File descriptor table */
static fat32_fd_t fds[FAT32_MAX_FD];

/* Read-only flag set by chimerafs_fsck() on unrecoverable errors */
static bool vol_readonly = false;

/* ── Low-level sector I/O with cache ─────────────────────────────────────── */

static bool sec_flush(void)
{
    if (!sec_cache_dirty) return true;
    if (ata_write(sec_cache_lba, 1, sec_cache) != ATA_OK) return false;
    sec_cache_dirty = false;
    return true;
}

static bool sec_read(uint32_t lba)
{
    if (sec_cache_lba == lba) return true;
    if (!sec_flush()) return false;
    if (ata_read(lba, 1, sec_cache) != ATA_OK) return false;
    sec_cache_lba = lba;
    return true;
}

static bool sec_write(uint32_t lba, const uint8_t *data)
{
    if (sec_cache_lba != lba) {
        if (!sec_flush()) return false;
        sec_cache_lba = lba;
    }
    f_memcpy(sec_cache, data, SECTOR_SIZE);
    sec_cache_dirty = true;
    return true;
}

/* ── FAT32 geometry helpers ──────────────────────────────────────────────── */

static uint32_t cluster_to_lba(uint32_t cluster)
{
    return vol_data_lba + (cluster - 2) * vol_sec_per_clus;
}

static uint32_t fat_lba_for_cluster(uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    return vol_fat_lba + fat_offset / SECTOR_SIZE;
}

static uint32_t fat_offset_in_sector(uint32_t cluster)
{
    return (cluster * 4) % SECTOR_SIZE;
}

/* Read FAT entry for a cluster */
static uint32_t fat_get(uint32_t cluster)
{
    uint32_t lba = fat_lba_for_cluster(cluster);
    if (!sec_read(lba)) return 0x0FFFFFF7;
    uint32_t off = fat_offset_in_sector(cluster);
    uint32_t val;
    f_memcpy(&val, sec_cache + off, 4);
    return val & 0x0FFFFFFF;
}

/* Write FAT entry (both FAT copies) */
static bool fat_set(uint32_t cluster, uint32_t value)
{
    value &= 0x0FFFFFFF;
    for (uint32_t f = 0; f < vol_num_fats; f++) {
        uint32_t fat_lba = vol_fat_lba + f * vol_fat_size;
        uint32_t lba = fat_lba + (cluster * 4) / SECTOR_SIZE;
        uint32_t off = fat_offset_in_sector(cluster);
        if (!sec_read(lba)) return false;
        f_memcpy(sec_cache + off, &value, 4);
        sec_cache_dirty = true;
        if (!sec_flush()) return false;
    }
    return true;
}

#define FAT32_EOC  0x0FFFFFF8
#define FAT32_FREE 0x00000000
#define FAT32_BAD  0x0FFFFFF7

/* Allocate a free cluster and link it after `prev` (0 = start of chain) */
static uint32_t fat_alloc(uint32_t prev)
{
    /* Simple linear scan for a free cluster */
    for (uint32_t c = 2; c < vol_total_clusters + 2; c++) {
        if (fat_get(c) == FAT32_FREE) {
            fat_set(c, 0x0FFFFFFF);  /* Mark as EOC */
            if (prev != 0) fat_set(prev, c);
            /* Zero out the cluster */
            uint8_t zero[SECTOR_SIZE];
            f_memset(zero, 0, SECTOR_SIZE);
            uint32_t lba = cluster_to_lba(c);
            for (uint32_t s = 0; s < vol_sec_per_clus; s++) {
                ata_write(lba + s, 1, zero);
            }
            return c;
        }
    }
    return 0;  /* Disk full */
}

/* Free an entire cluster chain starting at `start` */
static void fat_free_chain(uint32_t start)
{
    uint32_t c = start;
    while (c >= 2 && c < FAT32_EOC) {
        uint32_t next = fat_get(c);
        fat_set(c, FAT32_FREE);
        c = next;
    }
}

/* Follow a cluster chain `n` clusters forward */
static uint32_t fat_follow(uint32_t start, uint32_t n)
{
    uint32_t c = start;
    for (uint32_t i = 0; i < n; i++) {
        if (c >= FAT32_EOC || c < 2) return 0;
        c = fat_get(c);
    }
    return c;
}

/* ── LFN helpers ─────────────────────────────────────────────────────────── */

/* Compute 8.3 checksum for LFN entries */
static uint8_t lfn_checksum(const char name83[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)name83[i];
    }
    return sum;
}

/* Extract LFN characters from an LFN entry into a buffer (UCS-2 → ASCII) */
static void lfn_extract(const fat32_lfn_entry_t *e, char *out13)
{
    int i = 0;
    for (int j = 0; j < 5; j++) out13[i++] = (char)(e->name1[j] & 0xFF);
    for (int j = 0; j < 6; j++) out13[i++] = (char)(e->name2[j] & 0xFF);
    for (int j = 0; j < 2; j++) out13[i++] = (char)(e->name3[j] & 0xFF);
}

/* Build a short 8.3 name from a long name (simplified: uppercase, truncate) */
static void make_83(const char *name, char out[11])
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

/* ── Directory scanning ──────────────────────────────────────────────────── */

#define DIR_ENTRY_SIZE  32
#define ENTRIES_PER_SEC (SECTOR_SIZE / DIR_ENTRY_SIZE)

typedef struct {
    uint32_t cluster;    /* Cluster of the directory */
    uint32_t entry_idx;  /* Entry index within directory */
    uint32_t lba;        /* LBA of the sector containing this entry */
    uint32_t off;        /* Byte offset within that sector */
} dir_pos_t;

/* Read a directory entry at position `pos` */
static bool dir_read_entry(const dir_pos_t *pos, fat32_dir_entry_t *out)
{
    if (!sec_read(pos->lba)) return false;
    f_memcpy(out, sec_cache + pos->off, DIR_ENTRY_SIZE);
    return true;
}

/* Write a directory entry at position `pos` */
static bool dir_write_entry(const dir_pos_t *pos, const fat32_dir_entry_t *e)
{
#ifdef LFN_TEST
    {
        static const char hx[] = "0123456789abcdef";
        char dbuf[96];
        int di = 0;
        /* Print: [DW] lba=LLLLLLLL off=OOO b0=XX */
        dbuf[di++]='['; dbuf[di++]='D'; dbuf[di++]='W'; dbuf[di++]=']';
        dbuf[di++]=' '; dbuf[di++]='o';
        dbuf[di++]='f'; dbuf[di++]='f'; dbuf[di++]='=';
        uint32_t _off = pos->off;
        dbuf[di++] = hx[(_off>>8)&0xf];
        dbuf[di++] = hx[(_off>>4)&0xf];
        dbuf[di++] = hx[(_off>>0)&0xf];
        dbuf[di++]=' '; dbuf[di++]='b'; dbuf[di++]='0'; dbuf[di++]='=';
        uint8_t _b0 = ((const uint8_t*)e)[0];
        dbuf[di++] = hx[(_b0>>4)&0xf];
        dbuf[di++] = hx[(_b0>>0)&0xf];
        dbuf[di++]=' '; dbuf[di++]='b'; dbuf[di++]='1'; dbuf[di++]='1'; dbuf[di++]='=';
        uint8_t _b11 = ((const uint8_t*)e)[11];
        dbuf[di++] = hx[(_b11>>4)&0xf];
        dbuf[di++] = hx[(_b11>>0)&0xf];
        /* If 8.3 entry (attr != 0x0f), print the name */
        if (_b11 != 0x0f) {
            dbuf[di++]=' '; dbuf[di++]='n'; dbuf[di++]='=';
            for (int _ni = 0; _ni < 11; _ni++) {
                uint8_t _nc = ((const uint8_t*)e)[_ni];
                dbuf[di++] = (_nc >= 32 && _nc < 127) ? (char)_nc : '?';
            }
        }
        dbuf[di++]='\n'; dbuf[di]=0;
        serial_puts(dbuf);
    }
#endif
    if (!sec_read(pos->lba)) return false;
    f_memcpy(sec_cache + pos->off, e, DIR_ENTRY_SIZE);
    sec_cache_dirty = true;
    return sec_flush();
}

/* Advance `pos` to the next directory entry. Returns false if end of dir. */
static bool dir_next(dir_pos_t *pos)
{
    pos->entry_idx++;
    pos->off += DIR_ENTRY_SIZE;
    if (pos->off >= SECTOR_SIZE) {
        pos->off = 0;
        pos->lba++;
        /* Check if we've crossed a cluster boundary */
        uint32_t sec_in_clus = (pos->lba - cluster_to_lba(pos->cluster))
                               % vol_sec_per_clus;
        if (sec_in_clus == 0 &&
            pos->lba != cluster_to_lba(pos->cluster)) {
            /* Move to next cluster */
            uint32_t next_clus = fat_get(pos->cluster);
            if (next_clus >= FAT32_EOC || next_clus < 2) return false;
            pos->cluster = next_clus;
            pos->lba = cluster_to_lba(next_clus);
        }
    }
    return true;
}

/* Initialize a dir_pos_t to the start of a directory cluster */
static void dir_pos_init(dir_pos_t *pos, uint32_t cluster)
{
    pos->cluster   = cluster;
    pos->entry_idx = 0;
    pos->lba       = cluster_to_lba(cluster);
    pos->off       = 0;
}

/*
 * Check whether a given 8.3 name already exists in a directory cluster.
 * Returns true if a collision is found.
 */
static bool dir_83_exists(uint32_t dir_cluster, const char name83[11])
{
    dir_pos_t pos;
    dir_pos_init(&pos, dir_cluster);
    for (;;) {
        fat32_dir_entry_t e;
        if (!dir_read_entry(&pos, &e)) break;
        uint8_t first = (uint8_t)e.name[0];
        if (first == 0x00) break;  /* end of directory */
        if (first == 0xE5) goto next83;  /* deleted */
        if (e.attr == FAT32_ATTR_LFN) goto next83;  /* LFN entry */
        /* Compare 8.3 name (11 bytes) */
        if (f_memcmp(e.name, name83, 11) == 0) return true;
next83:
        if (!dir_next(&pos)) break;
    }
    return false;
}

/*
 * Generate a unique 8.3 name for `name` in `dir_cluster`, applying ~N
 * numeric-tail generation (FAT32 LFN spec section 3.4) if the basis name
 * collides with an existing entry.
 *
 * The basis name is the first 8 chars of the stem (uppercase, stripped of
 * illegal chars).  If it collides, the stem is truncated to 6 chars and
 * "~1" is appended; if that also collides, "~2" is tried, up to "~9",
 * then "~10" (5-char stem) up to "~99", etc.
 */
static void make_83_unique(uint32_t dir_cluster, const char *name, char out[11])
{
    make_83(name, out);
#ifdef LFN_TEST
    {
        char _dbg83[32];
        int _di = 0;
        _dbg83[_di++]='['; _dbg83[_di++]='8'; _dbg83[_di++]='3'; _dbg83[_di++]=']'; _dbg83[_di++]=' ';
        for (int _j = 0; _j < 11; _j++) _dbg83[_di++] = (out[_j] == ' ') ? '_' : out[_j];
        _dbg83[_di++]='\n'; _dbg83[_di]=0;
        serial_puts(_dbg83);
    }
#endif
    if (!dir_83_exists(dir_cluster, out)) return;  /* basis name is unique */
#ifdef LFN_TEST
    serial_puts("[83] COLLISION\n");
#endif

    /* Save the extension (bytes 8-10) */
    char ext[3];
    ext[0] = out[8]; ext[1] = out[9]; ext[2] = out[10];

    /* Try ~1 .. ~9999 */
    for (int n = 1; n <= 9999; n++) {
        /* Compute digit string for n */
        char digits[5];
        int dlen = 0;
        int tmp = n;
        /* Build digits in reverse */
        do { digits[dlen++] = '0' + (tmp % 10); tmp /= 10; } while (tmp);
        /* suffix length = 1 (tilde) + dlen */
        int suffix_len = 1 + dlen;
        /* stem is truncated to (8 - suffix_len) chars */
        int stem_max = 8 - suffix_len;
        if (stem_max < 1) break;  /* can't fit */

        /* Rebuild the stem from the original name */
        f_memset(out, ' ', 11);
        uint32_t len = f_strlen(name);
        int dot = -1;
        for (int i = (int)len - 1; i >= 0; i--) {
            if (name[i] == '.') { dot = i; break; }
        }
        int ni = 0;
        for (uint32_t i = 0; i < len && ni < (uint32_t)stem_max; i++) {
            if ((int)i == dot) break;
            char c = f_toupper(name[i]);
            if (c == ' ' || c == '.') continue;
            out[ni++] = c;
        }
        /* Append ~N (digits in forward order) */
        out[ni++] = '~';
        for (int d = dlen - 1; d >= 0; d--) out[ni++] = digits[d];
        /* Restore extension */
        out[8] = ext[0]; out[9] = ext[1]; out[10] = ext[2];

        if (!dir_83_exists(dir_cluster, out)) return;  /* unique */
    }
    /* Fallback: use basis name even if not unique (should not happen) */
    make_83(name, out);
}

/*
 * Scan a directory for a named entry.
 * Handles LFN. Fills `out_pos` with the position of the 8.3 entry.
 * If `out_lfn_start` is non-NULL, fills it with the position of the first
 * LFN entry (or the 8.3 entry itself when there is no LFN).
 * Returns true if found.
 */
static bool dir_find(uint32_t dir_cluster, const char *name,
                     fat32_dir_entry_t *out_entry, dir_pos_t *out_pos,
                     dir_pos_t *out_lfn_start)
{
    char lfn_buf[FAT32_MAX_NAME];
    int  lfn_parts = 0;
    bool building_lfn = false;
    dir_pos_t lfn_start_pos;  /* position of the first LFN entry */

    dir_pos_t pos;
    dir_pos_init(&pos, dir_cluster);
    lfn_start_pos = pos;  /* default: start of dir (updated on first LFN) */

    for (;;) {
        fat32_dir_entry_t e;
        if (!dir_read_entry(&pos, &e)) return false;

        uint8_t first = (uint8_t)e.name[0];
        if (first == 0x00) break;  /* End of directory */
        if (first == 0xE5) {       /* Deleted entry */
            building_lfn = false;
            lfn_parts = 0;
            if (!dir_next(&pos)) break;
            continue;
        }

        if (e.attr == FAT32_ATTR_LFN) {
            /* LFN entry */
            const fat32_lfn_entry_t *lfn = (const fat32_lfn_entry_t *)&e;
            int seq = lfn->order & 0x1F;
            if (lfn->order & 0x40) {
                /* Last LFN entry (first in sequence on disk) */
                f_memset(lfn_buf, 0, sizeof(lfn_buf));
                lfn_parts = seq;
                building_lfn = true;
                lfn_start_pos = pos;  /* remember where the LFN chain starts */
            }
            if (building_lfn && seq >= 1 && seq <= 20) {
                char part[13];
                lfn_extract(lfn, part);
                int base = (seq - 1) * 13;
                for (int i = 0; i < 13 && base + i < FAT32_MAX_NAME - 1; i++) {
                    lfn_buf[base + i] = part[i];
                    if (part[i] == 0) break;
                }
            }
        } else {
            /* 8.3 entry */
            bool match = false;

            if (building_lfn && lfn_parts > 0) {
                /* Compare against LFN */
                /* Case-insensitive compare */
                uint32_t nlen = f_strlen(name);
                uint32_t llen = f_strlen(lfn_buf);
                if (nlen == llen) {
                    match = true;
                    for (uint32_t i = 0; i < nlen; i++) {
                        if (f_toupper(name[i]) != f_toupper(lfn_buf[i])) {
                            match = false; break;
                        }
                    }
                }
            }

            if (!match && !building_lfn) {
                /*
                 * FIX: Only fall back to 8.3 name comparison when there is
                 * NO LFN for this entry.  If we had an LFN and it did not
                 * match, the 8.3 alias must not be used as a fallback —
                 * doing so causes false matches when two long names share
                 * the same truncated 8.3 alias (e.g. "abcdefghijklm.txt"
                 * and "abcdefghijklmn.txt" both map to "ABCDEFGHTXT").
                 */
                char name83[11];
                make_83(name, name83);
                match = (f_memcmp(e.name, name83, 11) == 0);
            }

            if (match) {
                if (out_entry)     f_memcpy(out_entry, &e, DIR_ENTRY_SIZE);
                if (out_pos)       *out_pos = pos;
                if (out_lfn_start) *out_lfn_start = building_lfn
                                                    ? lfn_start_pos : pos;
                return true;
            }

            building_lfn = false;
            lfn_parts = 0;
            lfn_start_pos = pos;  /* reset for next entry */
        }

        if (!dir_next(&pos)) break;
    }
    return false;
}

/* ── Path resolution ─────────────────────────────────────────────────────── */

/*
 * Resolve a path to its directory cluster and filename component.
 * `path` must start with '/'.
 * Returns the cluster of the parent directory, or 0 on error.
 * `name_out` receives the final component name.
 */
static uint32_t path_resolve_parent(const char *path, char *name_out)
{
    if (!path || path[0] != '/') return 0;

    uint32_t cluster = vol_root_cluster;
    const char *p = path + 1;

    /* Walk each component */
    while (*p) {
        /* Extract next component */
        char comp[FAT32_MAX_NAME];
        int ci = 0;
        while (*p && *p != '/') {
            if (ci < FAT32_MAX_NAME - 1) comp[ci++] = *p;
            p++;
        }
        comp[ci] = '\0';
        if (*p == '/') p++;  /* skip slash */

        if (*p == '\0') {
            /* This is the final component */
            if (name_out) f_strcpy(name_out, comp);
            return cluster;
        }

        /* Descend into directory */
        fat32_dir_entry_t e;
        if (!dir_find(cluster, comp, &e, NULL, NULL)) return 0;
        if (!(e.attr & FAT32_ATTR_DIR)) return 0;

        cluster = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;
        if (cluster == 0) cluster = vol_root_cluster;
    }

    /* Path ended with '/' or was just '/' */
    if (name_out) name_out[0] = '\0';
    return cluster;
}

/* Resolve a full path to a directory entry */
static bool path_stat(const char *path, fat32_dir_entry_t *out_e,
                      dir_pos_t *out_pos, uint32_t *out_dir_cluster)
{
    if (f_strcmp(path, "/") == 0) {
        /* Root directory */
        if (out_e) {
            f_memset(out_e, 0, sizeof(*out_e));
            out_e->attr = FAT32_ATTR_DIR;
            out_e->fst_clus_hi = (uint16_t)(vol_root_cluster >> 16);
            out_e->fst_clus_lo = (uint16_t)(vol_root_cluster & 0xFFFF);
        }
        return true;
    }

    char name[FAT32_MAX_NAME];
    uint32_t dir_cluster = path_resolve_parent(path, name);
    if (!dir_cluster || name[0] == '\0') return false;

    if (out_dir_cluster) *out_dir_cluster = dir_cluster;
    return dir_find(dir_cluster, name, out_e, out_pos, NULL);
}

/* ── FAT32 formatting ────────────────────────────────────────────────────── */

static bool fat32_format(uint32_t lba_start, uint32_t total_sectors)
{
    uint8_t buf[SECTOR_SIZE];

    /* Choose geometry */
    uint32_t sec_per_clus = 8;   /* 4 KB clusters */
    uint32_t reserved     = 32;
    uint32_t num_fats     = 2;

    /* Estimate FAT size */
    uint32_t data_sectors = total_sectors - reserved;
    uint32_t total_clusters = data_sectors / sec_per_clus;
    uint32_t fat_size = (total_clusters * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    fat_size = (fat_size + 7) & ~7u;  /* round up to 8 sectors */

    /* Recalculate data clusters */
    data_sectors = total_sectors - reserved - num_fats * fat_size;
    total_clusters = data_sectors / sec_per_clus;

    /* Build BPB */
    f_memset(buf, 0, SECTOR_SIZE);
    fat32_bpb_t *bpb = (fat32_bpb_t *)buf;
    buf[0] = 0xEB; buf[1] = 0x58; buf[2] = 0x90;  /* JMP short + NOP */
    f_memcpy(bpb->oem, "CHIMAERA", 8);
    bpb->bytes_per_sector   = SECTOR_SIZE;
    bpb->sectors_per_cluster= (uint8_t)sec_per_clus;
    bpb->reserved_sectors   = (uint16_t)reserved;
    bpb->num_fats           = (uint8_t)num_fats;
    bpb->root_entry_count   = 0;
    bpb->total_sectors_16   = 0;
    bpb->media_type         = 0xF8;
    bpb->fat_size_16        = 0;
    bpb->sectors_per_track  = 63;
    bpb->num_heads          = 255;
    bpb->hidden_sectors     = lba_start;
    bpb->total_sectors_32   = total_sectors;
    bpb->fat_size_32        = fat_size;
    bpb->ext_flags          = 0;
    bpb->fs_version         = 0;
    bpb->root_cluster       = 2;
    bpb->fs_info            = 1;
    bpb->backup_boot_sector = 6;
    bpb->drive_number       = 0x80;
    bpb->boot_sig           = 0x29;
    bpb->volume_id          = 0xC0FFEE42;
    f_memcpy(bpb->volume_label, "CHIMAERA   ", 11);
    f_memcpy(bpb->fs_type,      "FAT32   ", 8);
    buf[510] = 0x55; buf[511] = 0xAA;

    if (ata_write(lba_start, 1, buf) != ATA_OK) return false;

    /* Clear FAT tables */
    f_memset(buf, 0, SECTOR_SIZE);
    uint32_t fat_lba = lba_start + reserved;
    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t base = fat_lba + f * fat_size;
        for (uint32_t s = 0; s < fat_size; s++) {
            ata_write(base + s, 1, buf);
        }
    }

    /* Set FAT[0] = media byte, FAT[1] = EOC, FAT[2] = EOC (root dir) */
    uint32_t *fat_buf = (uint32_t *)buf;
    f_memset(buf, 0, SECTOR_SIZE);
    fat_buf[0] = 0x0FFFFFF8;  /* Media type */
    fat_buf[1] = 0x0FFFFFFF;  /* EOC */
    fat_buf[2] = 0x0FFFFFFF;  /* Root dir cluster (EOC) */
    for (uint32_t f = 0; f < num_fats; f++) {
        ata_write(fat_lba + f * fat_size, 1, buf);
    }

    /* Clear root directory cluster */
    f_memset(buf, 0, SECTOR_SIZE);
    uint32_t data_lba = fat_lba + num_fats * fat_size;
    uint32_t root_lba = data_lba;  /* cluster 2 = first data cluster */
    for (uint32_t s = 0; s < sec_per_clus; s++) {
        ata_write(root_lba + s, 1, buf);
    }

    return true;
}

/* ── FIX 2b: On-mount orphan reclaim and FSInfo repair ──────────────────── */

/*
 * fat32_fsck_repair() — run after FAT2 mirror repair on every mount.
 *
 * Scans the directory tree to build a bitmap of all reachable clusters.
 * Any allocated cluster not in the bitmap is an orphan (left by a power
 * failure during fat_alloc()).  We free orphaned clusters and update the
 * FSInfo free-cluster count so that fsck.fat -n exits cleanly.
 *
 * Complexity: O(vol_total_clusters) reads, O(orphan_count) writes.
 * Memory:     ~16 KB heap for the bitmap (64 MB disk with 512-byte clusters).
 */
static void mark_chain_reachable(uint8_t *bmp, uint32_t start)
{
    uint32_t c = start;
    while (c >= 2 && c < vol_total_clusters + 2) {
        uint32_t idx = c - 2;
        if (bmp[idx / 8] & (1u << (idx % 8))) break;  /* already visited */
        bmp[idx / 8] |= (1u << (idx % 8));
        uint32_t next = fat_get(c);
        if (next >= 0x0FFFFFF8 || next == 0) break;  /* EOC or free */
        c = next;
    }
}

static void mark_dir_reachable(uint8_t *bmp, uint32_t dir_cluster, int depth)
{
    if (depth > 16 || dir_cluster < 2) return;

    /* Mark the directory cluster chain itself */
    mark_chain_reachable(bmp, dir_cluster);

    /* Scan directory entries */
    dir_pos_t pos;
    dir_pos_init(&pos, dir_cluster);
    for (;;) {
        fat32_dir_entry_t e;
        if (!dir_read_entry(&pos, &e)) break;
        uint8_t first = (uint8_t)e.name[0];
        if (first == 0x00) break;
        if (first == 0xE5) { dir_next(&pos); continue; }
        if (e.attr == FAT32_ATTR_LFN) { dir_next(&pos); continue; }
        if (e.attr & 0x08) { dir_next(&pos); continue; }  /* volume label */

        uint32_t sc = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;
        if (sc >= 2) {
            mark_chain_reachable(bmp, sc);
            /* Recurse into subdirectories (skip . and ..) */
            if ((e.attr & FAT32_ATTR_DIR) &&
                f_memcmp(e.name, ".       ", 8) != 0 &&
                f_memcmp(e.name, "..      ", 8) != 0) {
                mark_dir_reachable(bmp, sc, depth + 1);
            }
        }
        if (!dir_next(&pos)) break;
    }
}

/* Helper: free an entire cluster chain starting at `start` */
static void free_chain(uint32_t start)
{
    uint32_t c = start;
    uint32_t guard = 0;
    while (c >= 2 && c < vol_total_clusters + 2 && guard < vol_total_clusters) {
        uint32_t next = fat_get(c);
        fat_set(c, FAT32_FREE);
        if (next >= FAT32_EOC || next == 0) break;
        c = next;
        guard++;
    }
}

/* Helper: scan one directory cluster chain for size=0 files with non-empty
 * chains (Bug 3: fat32_open wrote first_cluster but fat32_close never ran). */
static void repair_dir_zero_size(uint32_t dir_cluster, int depth)
{
    if (depth > 16 || dir_cluster < 2) return;

    dir_pos_t pos;
    dir_pos_init(&pos, dir_cluster);
    for (;;) {
        fat32_dir_entry_t e;
        if (!dir_read_entry(&pos, &e)) break;
        uint8_t first = (uint8_t)e.name[0];
        if (first == 0x00) break;
        if (first == 0xE5) { dir_next(&pos); continue; }
        if (e.attr == FAT32_ATTR_LFN) { dir_next(&pos); continue; }
        if (e.attr & 0x08) { dir_next(&pos); continue; }  /* volume label */

        uint32_t sc = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;

        if (!(e.attr & FAT32_ATTR_DIR) && e.file_size == 0 && sc >= 2) {
            /* Bug 3: file has size=0 but non-empty cluster chain.
             * Free the chain and zero the cluster pointer in the dir entry. */
            free_chain(sc);
            e.fst_clus_hi = 0;
            e.fst_clus_lo = 0;
            dir_write_entry(&pos, &e);
        }

        /* Recurse into subdirectories */
        if ((e.attr & FAT32_ATTR_DIR) && sc >= 2 &&
            f_memcmp(e.name, ".       ", 8) != 0 &&
            f_memcmp(e.name, "..      ", 8) != 0) {
            repair_dir_zero_size(sc, depth + 1);
        }

        if (!dir_next(&pos)) break;
    }
}

/* Helper: scan one directory cluster chain for files whose first_cluster
 * points to a FREE FAT entry (dangling pointer from a partial write). */
static void repair_dir_dangling_cluster(uint32_t dir_cluster, int depth)
{
    if (depth > 16 || dir_cluster < 2) return;

    dir_pos_t pos;
    dir_pos_init(&pos, dir_cluster);
    for (;;) {
        fat32_dir_entry_t e;
        if (!dir_read_entry(&pos, &e)) break;
        uint8_t first = (uint8_t)e.name[0];
        if (first == 0x00) break;
        if (first == 0xE5) { dir_next(&pos); continue; }
        if (e.attr == FAT32_ATTR_LFN) { dir_next(&pos); continue; }
        if (e.attr & 0x08) { dir_next(&pos); continue; }  /* volume label */

        uint32_t sc = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;

        if (!(e.attr & FAT32_ATTR_DIR) && sc >= 2) {
            /* Check if the first cluster is FREE in the FAT */
            uint32_t fat_val = fat_get(sc);
            if (fat_val == FAT32_FREE) {
                /* Dangling pointer: zero out first_cluster and file_size */
                e.fst_clus_hi = 0;
                e.fst_clus_lo = 0;
                e.file_size   = 0;
                dir_write_entry(&pos, &e);
            }
        }

        /* Recurse into subdirectories */
        if ((e.attr & FAT32_ATTR_DIR) && sc >= 2 &&
            f_memcmp(e.name, ".       ", 8) != 0 &&
            f_memcmp(e.name, "..      ", 8) != 0) {
            repair_dir_dangling_cluster(sc, depth + 1);
        }

        if (!dir_next(&pos)) break;
    }
}

static void fat32_fsck_repair(void)
{
    if (vol_total_clusters == 0) return;

    /*
     * Pass A — Bug 3 fix: truncate files with size=0 but first_cluster != 0.
     * These are created when fat32_open() writes the dir entry with the
     * cluster pointer but fat32_close() is never called (power cut during
     * write).  We free the chain and zero the cluster pointer so that
     * fsck.fat does not complain about "size=0, chain>0".
     *
     * Must run BEFORE the orphan scan (Pass B) so that the freed clusters
     * are correctly counted as free.
     */
    repair_dir_zero_size(vol_root_cluster, 0);

    /*
     * Pass A2 — Truncate files whose first_cluster is FREE in the FAT.
     * This handles the case where fat32_write()/fat32_close() updated the
     * dir entry (size > 0, first_cluster = X) but the FAT entry for cluster X
     * was never written (power cut between dir-entry write and FAT write).
     * The cluster appears FREE in the FAT but the dir entry still references it.
     * fsck.fat reports "Contains a free cluster. Assuming EOF" and then
     * "File size is N bytes, cluster chain length is 0 bytes".
     * Fix: zero out first_cluster and file_size in the dir entry.
     */
    repair_dir_dangling_cluster(vol_root_cluster, 0);

    /* Allocate reachable bitmap: 1 bit per cluster */
    uint32_t bmp_bytes = (vol_total_clusters + 7) / 8;
    uint8_t *bmp = (uint8_t *)kmalloc(bmp_bytes);
    if (!bmp) return;  /* Out of memory — skip repair */
    f_memset(bmp, 0, bmp_bytes);

    /* Pass B — Walk directory tree from root, marking all reachable clusters */
    mark_dir_reachable(bmp, vol_root_cluster, 0);

    /* Pass B — Scan FAT for orphaned clusters and free them */
    uint32_t orphans_freed = 0;
    uint32_t free_count    = 0;
    for (uint32_t c = 2; c < vol_total_clusters + 2; c++) {
        uint32_t val = fat_get(c);
        if (val == FAT32_FREE) {
            free_count++;
            continue;
        }
        if (val == 0x0FFFFFF7) continue;  /* bad cluster */
        /* Cluster is allocated */
        uint32_t idx = c - 2;
        if (!(bmp[idx / 8] & (1u << (idx % 8)))) {
            /* Orphan: free it */
            fat_set(c, FAT32_FREE);
            free_count++;
            orphans_freed++;
        }
    }

    kfree(bmp);

    /* FIX 4 — Update FSInfo free cluster count to match reality.
     * The FSInfo sector (LBA = vol_lba_start + bpb->fs_info, usually 1)
     * caches the free cluster count.  After a power failure the cached
     * value is wrong.  We overwrite it with the value we just computed.
     */
    uint8_t fsinfo[SECTOR_SIZE];
    uint32_t fsinfo_lba = vol_lba_start + 1;  /* standard FSInfo LBA */
    if (ata_read(fsinfo_lba, 1, fsinfo) == ATA_OK) {
        /* Validate FSInfo signature */
        uint32_t sig1, sig2;
        f_memcpy(&sig1, fsinfo + 0,   4);
        f_memcpy(&sig2, fsinfo + 484, 4);
        if (sig1 == 0x41615252 && sig2 == 0x61417272) {
            f_memcpy(fsinfo + 488, &free_count, 4);  /* free cluster count */
            uint32_t next_free = 0xFFFFFFFF;          /* unknown */
            f_memcpy(fsinfo + 492, &next_free, 4);
            ata_write(fsinfo_lba, 1, fsinfo);
        }
    }
    /* Flush any pending sector cache writes BEFORE the raw ATA FSInfo write,
     * then invalidate the cache so subsequent code does not see stale data.
     * Without this flush, the last fat_set() call in the orphan scan may
     * remain in the cache and be silently discarded when we reset the LBA. */
    sec_flush();
    /* Invalidate sector cache after raw ATA I/O */
    sec_cache_lba = 0xFFFFFFFF;
    sec_cache_dirty = false;

    (void)orphans_freed;  /* suppress unused warning */

    /* Signal to the power-fail test harness that the repair pass is complete
     * and all writes have been flushed to disk. The harness kills QEMU on
     * this marker to avoid the PFTEST code running and creating new writes. */
#ifdef POWERFAIL_TEST
    serial_puts("[PFTEST] REPAIR_DONE\n");
#endif
}

/* ── Mount ───────────────────────────────────────────────────────────────── */

bool fat32_mount(void)
{
    if (!ata_disk_present()) return false;

    /* Read MBR */
    uint8_t mbr[SECTOR_SIZE];
    if (ata_read(0, 1, mbr) != ATA_OK) return false;

    /* Check MBR signature */
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        /* No valid MBR — format the disk */
        vga_puts("[FAT32] No MBR found, formatting disk...\n");
        uint32_t total = ata_disk_sectors();
        if (total == 0) total = 131072;  /* 64 MB default */
        if (!fat32_format(0, total)) return false;
        /* Re-read after format */
        if (ata_read(0, 1, mbr) != ATA_OK) return false;
    }

    /* Try to find a FAT32 partition */
    uint32_t lba_start = 0;
    mbr_partition_t *parts = (mbr_partition_t *)(mbr + 0x1BE);
    for (int i = 0; i < 4; i++) {
        if (parts[i].type == 0x0B || parts[i].type == 0x0C) {
            lba_start = parts[i].lba_start;
            break;
        }
    }

    /* If no partition found, treat sector 0 as the volume start */
    /* Read BPB */
    uint8_t boot[SECTOR_SIZE];
    if (ata_read(lba_start, 1, boot) != ATA_OK) return false;

    /* Validate FAT32 signature */
    if (boot[510] != 0x55 || boot[511] != 0xAA) {
        vga_puts("[FAT32] Invalid boot sector, formatting...\n");
        uint32_t total = ata_disk_sectors();
        if (!fat32_format(lba_start, total - lba_start)) return false;
        if (ata_read(lba_start, 1, boot) != ATA_OK) return false;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)boot;

    /* Validate FAT32 type string */
    if (f_memcmp(bpb->fs_type, "FAT32", 5) != 0) {
        vga_puts("[FAT32] Not FAT32, reformatting...\n");
        uint32_t total = ata_disk_sectors();
        if (!fat32_format(lba_start, total - lba_start)) return false;
        if (ata_read(lba_start, 1, boot) != ATA_OK) return false;
        bpb = (fat32_bpb_t *)boot;
    }

    /* Parse volume parameters */
    vol_lba_start     = lba_start;
    vol_bytes_per_sec = bpb->bytes_per_sector;
    vol_sec_per_clus  = bpb->sectors_per_cluster;
    vol_reserved_sec  = bpb->reserved_sectors;
    vol_num_fats      = bpb->num_fats;
    vol_fat_size      = bpb->fat_size_32;
    vol_root_cluster  = bpb->root_cluster;

    vol_fat_lba  = lba_start + vol_reserved_sec;
    vol_data_lba = vol_fat_lba + vol_num_fats * vol_fat_size;

    uint32_t total_sec = bpb->total_sectors_32 ? bpb->total_sectors_32
                                                : bpb->total_sectors_16;
    uint32_t data_sec  = total_sec - (vol_data_lba - lba_start);
    vol_total_clusters = data_sec / vol_sec_per_clus;

    /* Initialize FDs */
    f_memset(fds, 0, sizeof(fds));

    /*
     * FIX 1 — FAT2 mirror repair on mount.
     *
     * A power failure between the FAT1 write and the FAT2 write in fat_set()
     * leaves FAT2 stale. On every mount we scan for divergent sectors and copy
     * FAT1 → FAT2 so that fsck.fat never sees a mismatch.
     *
     * We only repair sectors that differ, so a clean mount is O(fat_size)
     * reads with zero writes.
     */
    if (vol_num_fats >= 2) {
        uint8_t fat1_sec[SECTOR_SIZE];
        uint8_t fat2_sec[SECTOR_SIZE];
        uint32_t fat2_lba_base = vol_fat_lba + vol_fat_size;
        for (uint32_t s = 0; s < vol_fat_size; s++) {
            if (ata_read(vol_fat_lba + s, 1, fat1_sec) != ATA_OK) break;
            if (ata_read(fat2_lba_base + s, 1, fat2_sec) != ATA_OK) break;
            if (f_memcmp(fat1_sec, fat2_sec, SECTOR_SIZE) != 0) {
                /* FAT2 sector is stale — overwrite with FAT1 */
                ata_write(fat2_lba_base + s, 1, fat1_sec);
            }
        }
        /* Invalidate sector cache after raw ATA I/O */
        sec_cache_lba = 0xFFFFFFFF;
        sec_cache_dirty = false;
    }

    /*
     * FIX 2b — Orphan reclaim and FSInfo repair.
     *
     * Must run after FAT2 mirror repair (so fat_get() reads consistent data)
     * and before vol_mounted is set to true (so dir_read_entry() can use
     * the volume parameters we just parsed).
     *
     * We temporarily set vol_mounted=true so that the helper functions
     * (fat_get, dir_read_entry, etc.) work, then reset it if repair fails.
     */
    vol_mounted = true;
    /* chimerafs_fsck() is a strict superset of fat32_fsck_repair():
     * it handles FAT mismatch, orphaned clusters (→ /lost+found),
     * cross-links, invalid entries, and orphaned LFN entries.
     * fat32_fsck_repair() is no longer called to avoid double-processing. */
    chimerafs_fsck();     /* full five-check consistency scan */
    /* vol_mounted stays true — fall through to the success path */
    vol_mounted = false;  /* will be set again below */

    vol_mounted = true;
    vga_puts("[FAT32] Mounted /persist (");
    /* Print cluster count */
    char nbuf[16];
    uint32_t n = vol_total_clusters;
    int ni = 0;
    if (n == 0) { nbuf[ni++] = '0'; }
    else { while(n){nbuf[ni++]='0'+n%10;n/=10;} }
    for (int i=ni-1;i>=0;i--) vga_putchar(nbuf[i]);
    vga_puts(" clusters)\n");
    return true;
}

bool fat32_mounted(void) { return vol_mounted; }

/* ── Directory entry creation ────────────────────────────────────────────── */

/*
 * Add a directory entry (with optional LFN) to a directory cluster.
 * Returns the position of the 8.3 entry, or 0 on failure.
 */
static bool dir_add_entry(uint32_t dir_cluster, const char *name,
                          uint8_t attr, uint32_t first_cluster,
                          uint32_t file_size, dir_pos_t *out_pos)
{
    char name83[11];
    make_83_unique(dir_cluster, name, name83);
    uint8_t checksum = lfn_checksum(name83);

    /* Count LFN entries needed */
    uint32_t nlen = f_strlen(name);
    int lfn_count = (int)((nlen + 12) / 13);

    /* Total entries needed = lfn_count + 1 (8.3) */
    int need = lfn_count + 1;

    /* Scan directory for `need` consecutive free/deleted entries */
    dir_pos_t pos;
    dir_pos_init(&pos, dir_cluster);
    int free_run = 0;
    dir_pos_t run_start = pos;

    for (;;) {
        fat32_dir_entry_t e;
        if (!dir_read_entry(&pos, &e)) {
            /* End of allocated space — try to extend directory */
            break;
        }
        uint8_t first = (uint8_t)e.name[0];
#ifdef LFN_TEST
        {
            static const char hx2[] = "0123456789abcdef";
            char dbuf2[32];
            int di2 = 0;
            dbuf2[di2++]='['; dbuf2[di2++]='S'; dbuf2[di2++]='C'; dbuf2[di2++]=']';
            dbuf2[di2++]=' ';
            uint32_t _o2 = pos.off;
            dbuf2[di2++] = hx2[(_o2>>8)&0xf];
            dbuf2[di2++] = hx2[(_o2>>4)&0xf];
            dbuf2[di2++] = hx2[(_o2>>0)&0xf];
            dbuf2[di2++]='=';
            dbuf2[di2++] = hx2[(first>>4)&0xf];
            dbuf2[di2++] = hx2[(first>>0)&0xf];
            dbuf2[di2++]='\n'; dbuf2[di2]=0;
            serial_puts(dbuf2);
        }
#endif
        if (first == 0x00 || first == 0xE5) {
            if (free_run == 0) run_start = pos;
            free_run++;
            if (free_run >= need) goto found_space;
        } else {
            free_run = 0;
        }
        if (!dir_next(&pos)) {
            /* Need to extend directory with a new cluster */
            uint32_t last_clus = pos.cluster;
            uint32_t new_clus = fat_alloc(last_clus);
            if (!new_clus) return false;
            /* Re-scan: the new cluster is zeroed, so we have free entries */
            dir_pos_init(&pos, new_clus);
            if (free_run == 0) run_start = pos;
            free_run += (int)(vol_sec_per_clus * ENTRIES_PER_SEC);
            goto found_space;
        }
    }
    return false;

found_space:
    /* Write LFN entries in reverse order */
    pos = run_start;
    for (int i = lfn_count; i >= 1; i--) {
        fat32_lfn_entry_t lfn;
        f_memset(&lfn, 0, sizeof(lfn));
        lfn.order    = (uint8_t)i | (i == lfn_count ? 0x40 : 0);
        lfn.attr     = FAT32_ATTR_LFN;
        lfn.checksum = checksum;
        lfn.fst_clus_lo = 0;

        /* Fill name fields with UCS-2 */
        int base = (i - 1) * 13;
        for (int j = 0; j < 5; j++) {
            int ci = base + j;
            lfn.name1[j] = (ci < (int)nlen) ? (uint16_t)(uint8_t)name[ci]
                                              : (ci == (int)nlen ? 0x0000 : 0xFFFF);
        }
        for (int j = 0; j < 6; j++) {
            int ci = base + 5 + j;
            lfn.name2[j] = (ci < (int)nlen) ? (uint16_t)(uint8_t)name[ci]
                                              : (ci == (int)nlen ? 0x0000 : 0xFFFF);
        }
        for (int j = 0; j < 2; j++) {
            int ci = base + 11 + j;
            lfn.name3[j] = (ci < (int)nlen) ? (uint16_t)(uint8_t)name[ci]
                                              : (ci == (int)nlen ? 0x0000 : 0xFFFF);
        }

#ifdef LFN_TEST
        {
            static const char hx3[] = "0123456789abcdef";
            char dbuf3[64];
            int di3 = 0;
            /* Print: [LX] n2_3=XXXX fclus=XXXX n3_0=XXXX */
            dbuf3[di3++]='['; dbuf3[di3++]='L'; dbuf3[di3++]='X'; dbuf3[di3++]=']';
            dbuf3[di3++]=' ';
            uint16_t _n23 = lfn.name2[3];
            dbuf3[di3++] = hx3[(_n23>>12)&0xf];
            dbuf3[di3++] = hx3[(_n23>>8)&0xf];
            dbuf3[di3++] = hx3[(_n23>>4)&0xf];
            dbuf3[di3++] = hx3[(_n23>>0)&0xf];
            dbuf3[di3++]=' ';
            uint16_t _fc = lfn.fst_clus_lo;
            dbuf3[di3++] = hx3[(_fc>>12)&0xf];
            dbuf3[di3++] = hx3[(_fc>>8)&0xf];
            dbuf3[di3++] = hx3[(_fc>>4)&0xf];
            dbuf3[di3++] = hx3[(_fc>>0)&0xf];
            dbuf3[di3++]=' ';
            uint16_t _n30 = lfn.name3[0];
            dbuf3[di3++] = hx3[(_n30>>12)&0xf];
            dbuf3[di3++] = hx3[(_n30>>8)&0xf];
            dbuf3[di3++] = hx3[(_n30>>4)&0xf];
            dbuf3[di3++] = hx3[(_n30>>0)&0xf];
            dbuf3[di3++]='\n'; dbuf3[di3]=0;
            serial_puts(dbuf3);
        }
#endif
        dir_write_entry(&pos, (fat32_dir_entry_t *)&lfn);
        dir_next(&pos);
    }

    /* Write 8.3 entry */
    fat32_dir_entry_t e83;
    f_memset(&e83, 0, sizeof(e83));
    f_memcpy(e83.name, name83, 11);
    e83.attr        = attr;
    e83.fst_clus_hi = (uint16_t)(first_cluster >> 16);
    e83.fst_clus_lo = (uint16_t)(first_cluster & 0xFFFF);
    e83.file_size   = file_size;
    e83.wrt_date    = 0x5B21;  /* 2025-09-01 placeholder */
    e83.wrt_time    = 0x0000;

    dir_write_entry(&pos, &e83);
    if (out_pos) *out_pos = pos;
    return true;
}

/* ── Public file operations ──────────────────────────────────────────────── */

int fat32_open(const char *path, bool write)
{
    if (!vol_mounted) return FAT32_ERR_NODEV;
    if (write && vol_readonly) return FAT32_ERR_IO;

    /* Find a free FD */
    int fd = -1;
    for (int i = 0; i < FAT32_MAX_FD; i++) {
        if (!fds[i].open) { fd = i; break; }
    }
    if (fd < 0) return FAT32_ERR_NOMEM;

    fat32_dir_entry_t e;
    dir_pos_t pos;
    uint32_t dir_cluster;

    if (path_stat(path, &e, &pos, &dir_cluster)) {
        if (e.attr & FAT32_ATTR_DIR) return FAT32_ERR_ISDIR;
        uint32_t cluster = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;
        fds[fd].open        = true;
        fds[fd].write       = write;
        fds[fd].cluster     = cluster;
        fds[fd].dir_cluster = dir_cluster;
        fds[fd].dir_lba     = pos.lba;
        fds[fd].dir_offset  = pos.off;
        fds[fd].size        = e.file_size;
        fds[fd].pos         = 0;
        fds[fd].cur_cluster = cluster;

        if (write) {
            /* Truncate: free chain and reset */
            if (cluster >= 2) fat_free_chain(cluster);
            uint32_t new_clus = fat_alloc(0);
            if (!new_clus) return FAT32_ERR_FULL;
            fds[fd].cluster     = new_clus;
            fds[fd].cur_cluster = new_clus;
            fds[fd].size        = 0;
            fds[fd].pos         = 0;
            /* Update dir entry */
            e.fst_clus_hi = (uint16_t)(new_clus >> 16);
            e.fst_clus_lo = (uint16_t)(new_clus & 0xFFFF);
            e.file_size   = 0;
            dir_write_entry(&pos, &e);
        }
    } else {
        if (!write) return FAT32_ERR_NOTFOUND;

        /* Create new file */
        char name[FAT32_MAX_NAME];
        uint32_t parent = path_resolve_parent(path, name);
        if (!parent) return FAT32_ERR_NOTFOUND;

        /*
         * FIX 2 (revised) — Allocate cluster first, then write dir entry.
         *
         * Original order was: fat_alloc() then dir_add_entry().
         * If killed between the two, the cluster is orphaned.
         *
         * Revised order: fat_alloc() then dir_add_entry() with the real
         * cluster pointer.  The dir entry is written with first_cluster set
         * immediately, so the chain is always reachable from the dir entry.
         *
         * Orphaned clusters that survive a crash are reclaimed by the
         * fat32_fsck_repair() call inside fat32_mount() (see FIX 2b below).
         *
         * FIX 3 — The dir entry is written with first_cluster=new_clus and
         * file_size=0 at open time.  fat32_close() updates file_size (and
         * re-writes first_cluster for safety) in one sector write.
         * If killed during the write, the dir entry has first_cluster set
         * (chain is reachable) but file_size=0.  fsck.fat will truncate the
         * file to 0 bytes and reclaim the chain — no structural corruption.
         */
        uint32_t new_clus = fat_alloc(0);
        if (!new_clus) return FAT32_ERR_FULL;

        dir_pos_t new_pos;
        if (!dir_add_entry(parent, name, FAT32_ATTR_ARCHIVE,
                           new_clus, 0, &new_pos)) {
            fat_free_chain(new_clus);
            return FAT32_ERR_FULL;
        }

        fds[fd].open        = true;
        fds[fd].write       = true;
        fds[fd].cluster     = new_clus;
        fds[fd].dir_cluster = parent;
        fds[fd].dir_lba     = new_pos.lba;
        fds[fd].dir_offset  = new_pos.off;
        fds[fd].size        = 0;
        fds[fd].pos         = 0;
        fds[fd].cur_cluster = new_clus;
    }

    return fd;
}

int fat32_read(int fd, void *buf, uint32_t len)
{
    if (fd < 0 || fd >= FAT32_MAX_FD || !fds[fd].open) return FAT32_ERR_NOTFOUND;
    fat32_fd_t *f = &fds[fd];
    if (f->pos >= f->size) return 0;

    uint32_t remaining = f->size - f->pos;
    if (len > remaining) len = remaining;

    uint8_t *out = (uint8_t *)buf;
    uint32_t read = 0;
    uint32_t cluster_size = vol_sec_per_clus * SECTOR_SIZE;

    while (read < len) {
        if (f->cur_cluster < 2 || f->cur_cluster >= FAT32_EOC) break;

        uint32_t clus_off = f->pos % cluster_size;
        uint32_t sec_off  = clus_off / SECTOR_SIZE;
        uint32_t byte_off = clus_off % SECTOR_SIZE;
        uint32_t lba      = cluster_to_lba(f->cur_cluster) + sec_off;

        if (!sec_read(lba)) break;

        uint32_t can_read = SECTOR_SIZE - byte_off;
        if (can_read > len - read) can_read = len - read;

        f_memcpy(out + read, sec_cache + byte_off, can_read);
        read    += can_read;
        f->pos  += can_read;

        /* Advance cluster if needed */
        if (f->pos % cluster_size == 0) {
            f->cur_cluster = fat_get(f->cur_cluster);
        }
    }

    return (int)read;
}

int fat32_write(int fd, const void *buf, uint32_t len)
{
    if (fd < 0 || fd >= FAT32_MAX_FD || !fds[fd].open || !fds[fd].write)
        return FAT32_ERR_NOTFOUND;
    fat32_fd_t *f = &fds[fd];

    const uint8_t *in = (const uint8_t *)buf;
    uint32_t written = 0;
    uint32_t cluster_size = vol_sec_per_clus * SECTOR_SIZE;

    while (written < len) {
        uint32_t clus_off = f->pos % cluster_size;

        /* Allocate new cluster if at boundary */
        if (clus_off == 0 && f->pos > 0) {
            uint32_t next = fat_get(f->cur_cluster);
            if (next >= FAT32_EOC || next < 2) {
                uint32_t new_clus = fat_alloc(f->cur_cluster);
                if (!new_clus) break;
                f->cur_cluster = new_clus;
            } else {
                f->cur_cluster = next;
            }
        }

        uint32_t sec_off  = clus_off / SECTOR_SIZE;
        uint32_t byte_off = clus_off % SECTOR_SIZE;
        uint32_t lba      = cluster_to_lba(f->cur_cluster) + sec_off;

        /* Read-modify-write the sector */
        if (!sec_read(lba)) break;

        uint32_t can_write = SECTOR_SIZE - byte_off;
        if (can_write > len - written) can_write = len - written;

        f_memcpy(sec_cache + byte_off, in + written, can_write);
        sec_cache_dirty = true;
        if (!sec_flush()) break;

        written  += can_write;
        f->pos   += can_write;
        if (f->pos > f->size) f->size = f->pos;
    }

    return (int)written;
}

fat32_err_t fat32_seek(int fd, uint32_t pos)
{
    if (fd < 0 || fd >= FAT32_MAX_FD || !fds[fd].open) return FAT32_ERR_NOTFOUND;
    fat32_fd_t *f = &fds[fd];
    if (pos > f->size) return FAT32_ERR_NOTFOUND;

    uint32_t cluster_size = vol_sec_per_clus * SECTOR_SIZE;
    uint32_t target_clus  = pos / cluster_size;

    f->cur_cluster = f->cluster;
    for (uint32_t i = 0; i < target_clus; i++) {
        uint32_t next = fat_get(f->cur_cluster);
        if (next >= FAT32_EOC || next < 2) return FAT32_ERR_NOTFOUND;
        f->cur_cluster = next;
    }
    f->pos = pos;
    return FAT32_OK;
}

void fat32_close(int fd)
{
    if (fd < 0 || fd >= FAT32_MAX_FD || !fds[fd].open) return;
    fat32_fd_t *f = &fds[fd];

    /*
     * FIX 3 — Atomic commit on close.
     *
     * Write cluster pointer AND file size in a single sector write.
     * Before this write the dir entry shows cluster=0, size=0 (safe empty
     * file).  After this write the file is fully committed.  There is no
     * intermediate state that leaves a non-empty chain with size=0.
     */
    if (f->write) {
        /* Re-read the dir entry sector.
         * Use the stored dir_lba directly — dir_offset is the byte offset
         * within that sector (0..SECTOR_SIZE-1), NOT within the cluster.
         * Using cluster_to_lba(dir_cluster) + dir_offset/SECTOR_SIZE was
         * wrong because dir_offset/SECTOR_SIZE is always 0.
         */
        uint32_t dir_lba = f->dir_lba;
        uint32_t dir_off = f->dir_offset;
        if (sec_read(dir_lba)) {
            fat32_dir_entry_t *e = (fat32_dir_entry_t *)(sec_cache + dir_off);
            /* Atomic commit: cluster pointer + size in one sector write */
            e->file_size   = f->size;
            e->fst_clus_hi = (uint16_t)(f->cluster >> 16);
            e->fst_clus_lo = (uint16_t)(f->cluster & 0xFFFF);
            sec_cache_dirty = true;
            sec_flush();  /* Single ATA write — atomic at the sector level */
        }
    }

    f_memset(f, 0, sizeof(*f));
}

int32_t fat32_filesize(const char *path)
{
    fat32_dir_entry_t e;
    if (!path_stat(path, &e, NULL, NULL)) return -1;
    if (e.attr & FAT32_ATTR_DIR) return -1;
    return (int32_t)e.file_size;
}

/* ── Directory operations ────────────────────────────────────────────────── */

fat32_err_t fat32_mkdir(const char *path)
{
    if (!vol_mounted) return FAT32_ERR_NODEV;

    /* Check if already exists */
    fat32_dir_entry_t e;
    if (path_stat(path, &e, NULL, NULL)) return FAT32_ERR_EXISTS;

    char name[FAT32_MAX_NAME];
    uint32_t parent = path_resolve_parent(path, name);
    if (!parent || name[0] == '\0') return FAT32_ERR_NOTFOUND;

    /* Allocate cluster for new directory */
    uint32_t new_clus = fat_alloc(0);
    if (!new_clus) return FAT32_ERR_FULL;

    /* Add . and .. entries */
    fat32_dir_entry_t dot;
    f_memset(&dot, 0, sizeof(dot));
    f_memcpy(dot.name, ".          ", 11);
    dot.attr        = FAT32_ATTR_DIR;
    dot.fst_clus_hi = (uint16_t)(new_clus >> 16);
    dot.fst_clus_lo = (uint16_t)(new_clus & 0xFFFF);

    fat32_dir_entry_t dotdot;
    f_memset(&dotdot, 0, sizeof(dotdot));
    f_memcpy(dotdot.name, "..         ", 11);
    dotdot.attr        = FAT32_ATTR_DIR;
    /* FAT32 spec: the .. entry in a directory whose parent is the root
     * directory must store cluster 0 (not vol_root_cluster) so that
     * tools like fsck.fat and Windows correctly resolve the root. */
    uint32_t parent_clus = (parent == vol_root_cluster) ? 0 : parent;
    dotdot.fst_clus_hi = (uint16_t)(parent_clus >> 16);
    dotdot.fst_clus_lo = (uint16_t)(parent_clus & 0xFFFF);

    /* Write . and .. to new cluster */
    uint32_t lba = cluster_to_lba(new_clus);
    if (!sec_read(lba)) { fat_free_chain(new_clus); return FAT32_ERR_IO; }
    f_memcpy(sec_cache, &dot, DIR_ENTRY_SIZE);
    f_memcpy(sec_cache + DIR_ENTRY_SIZE, &dotdot, DIR_ENTRY_SIZE);
    sec_cache_dirty = true;
    sec_flush();

    /* Add entry to parent */
    if (!dir_add_entry(parent, name, FAT32_ATTR_DIR, new_clus, 0, NULL)) {
        fat_free_chain(new_clus);
        return FAT32_ERR_FULL;
    }

    return FAT32_OK;
}

int fat32_readdir(const char *path,
                  void (*cb)(const fat32_dirent_t *e, void *ud),
                  void *userdata)
{
    if (!vol_mounted) return FAT32_ERR_NODEV;

    fat32_dir_entry_t de;
    if (!path_stat(path, &de, NULL, NULL)) return FAT32_ERR_NOTFOUND;
    if (!(de.attr & FAT32_ATTR_DIR)) return FAT32_ERR_NOTDIR;

    uint32_t cluster = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
    if (cluster == 0) cluster = vol_root_cluster;

    dir_pos_t pos;
    dir_pos_init(&pos, cluster);

    char lfn_buf[FAT32_MAX_NAME];
    bool building_lfn = false;
    int count = 0;

    for (;;) {
        fat32_dir_entry_t e;
        if (!dir_read_entry(&pos, &e)) break;

        uint8_t first = (uint8_t)e.name[0];
        if (first == 0x00) break;
        if (first == 0xE5) {
            building_lfn = false;
            if (!dir_next(&pos)) break;
            continue;
        }

        if (e.attr == FAT32_ATTR_LFN) {
            const fat32_lfn_entry_t *lfn = (const fat32_lfn_entry_t *)&e;
            int seq = lfn->order & 0x1F;
            if (lfn->order & 0x40) {
                f_memset(lfn_buf, 0, sizeof(lfn_buf));
                building_lfn = true;
            }
            if (building_lfn && seq >= 1 && seq <= 20) {
                char part[13];
                lfn_extract(lfn, part);
                int base = (seq - 1) * 13;
                for (int i = 0; i < 13 && base + i < FAT32_MAX_NAME - 1; i++) {
                    lfn_buf[base + i] = part[i];
                    if (part[i] == 0) break;
                }
            }
        } else {
            /* Skip . and .. */
            if (e.name[0] == '.' && (e.name[1] == ' ' || e.name[1] == '.')) {
                building_lfn = false;
                if (!dir_next(&pos)) break;
                continue;
            }

            fat32_dirent_t out;
            f_memset(&out, 0, sizeof(out));

            if (building_lfn && lfn_buf[0]) {
                f_strcpy(out.name, lfn_buf);
            } else {
                /* Build name from 8.3 */
                int ni = 0;
                for (int i = 0; i < 8 && e.name[i] != ' '; i++)
                    out.name[ni++] = e.name[i];
                if (e.ext[0] != ' ') {
                    out.name[ni++] = '.';
                    for (int i = 0; i < 3 && e.ext[i] != ' '; i++)
                        out.name[ni++] = e.ext[i];
                }
                out.name[ni] = '\0';
            }

            out.size    = e.file_size;
            out.attr    = e.attr;
            out.is_dir  = (e.attr & FAT32_ATTR_DIR) != 0;
            out.cluster = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;

            if (cb) cb(&out, userdata);
            count++;
            building_lfn = false;
        }

        if (!dir_next(&pos)) break;
    }

    return count;
}

fat32_err_t fat32_stat(const char *path, fat32_dirent_t *out)
{
    if (!vol_mounted) return FAT32_ERR_NODEV;
    fat32_dir_entry_t e;
    if (!path_stat(path, &e, NULL, NULL)) return FAT32_ERR_NOTFOUND;

    if (out) {
        /* Extract name from path */
        const char *last = path;
        for (const char *p = path; *p; p++) if (*p == '/') last = p + 1;
        f_strcpy(out->name, last);
        out->size   = e.file_size;
        out->attr   = e.attr;
        out->is_dir = (e.attr & FAT32_ATTR_DIR) != 0;
        out->cluster= ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;
    }
    return FAT32_OK;
}

fat32_err_t fat32_unlink(const char *path)
{
    if (!vol_mounted) return FAT32_ERR_NODEV;
    fat32_dir_entry_t e;
    dir_pos_t pos;
    dir_pos_t lfn_start;

    /* Resolve path, capturing both the 8.3 position and the LFN start */
    char name[FAT32_MAX_NAME];
    uint32_t dir_cluster = path_resolve_parent(path, name);
    if (!dir_cluster || name[0] == '\0') return FAT32_ERR_NOTFOUND;
    if (!dir_find(dir_cluster, name, &e, &pos, &lfn_start))
        return FAT32_ERR_NOTFOUND;
    if (e.attr & FAT32_ATTR_DIR) return FAT32_ERR_ISDIR;

    /* Free cluster chain */
    uint32_t cluster = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;
    if (cluster >= 2) fat_free_chain(cluster);

    /*
     * FIX (Bug C) — Mark ALL directory entries for this file as deleted.
     *
     * The original code only marked the 8.3 entry (0xE5), leaving the
     * preceding LFN entries as orphans.  Orphaned LFN entries with a
     * non-deleted first byte (0x01..0x14 | 0x40) confuse the directory
     * scanner: it builds an LFN from them, then falls through to the
     * next non-deleted 8.3 entry and may produce a false match.
     *
     * Fix: walk from lfn_start to pos (inclusive) and set the first byte
     * of every entry to 0xE5.  Each write is a separate sector flush so
     * that a power failure between writes leaves at most one entry in an
     * ambiguous state; the remaining orphan is harmless (the 8.3 is
     * already deleted or will be deleted on the next flush).
     */
    dir_pos_t del = lfn_start;
    for (;;) {
        if (!sec_read(del.lba)) break;
        sec_cache[del.off] = 0xE5;
        sec_cache_dirty = true;
        sec_flush();
        if (del.lba == pos.lba && del.off == pos.off) break;  /* done */
        if (!dir_next(&del)) break;
    }

    return FAT32_OK;
}

fat32_err_t fat32_rename(const char *src, const char *dst)
{
    if (!vol_mounted) return FAT32_ERR_NODEV;
    /* Simple: copy + delete */
    fat32_err_t err = fat32_copy(src, dst);
    if (err != FAT32_OK) return err;
    return fat32_unlink(src);
}

fat32_err_t fat32_copy(const char *src, const char *dst)
{
    if (!vol_mounted) return FAT32_ERR_NODEV;

    int src_fd = fat32_open(src, false);
    if (src_fd < 0) return FAT32_ERR_NOTFOUND;

    int dst_fd = fat32_open(dst, true);
    if (dst_fd < 0) { fat32_close(src_fd); return FAT32_ERR_FULL; }

    uint8_t buf[512];
    int n;
    while ((n = fat32_read(src_fd, buf, sizeof(buf))) > 0) {
        fat32_write(dst_fd, buf, (uint32_t)n);
    }

    fat32_close(src_fd);
    fat32_close(dst_fd);
    return FAT32_OK;
}

void *fat32_read_file(const char *path, uint32_t *size_out)
{
    int32_t sz = fat32_filesize(path);
    if (sz < 0) return NULL;

    void *buf = kmalloc((uint32_t)sz + 1);
    if (!buf) return NULL;

    int fd = fat32_open(path, false);
    if (fd < 0) { kfree(buf); return NULL; }

    int n = fat32_read(fd, buf, (uint32_t)sz);
    fat32_close(fd);

    if (n < 0) { kfree(buf); return NULL; }
    ((uint8_t *)buf)[n] = 0;
    if (size_out) *size_out = (uint32_t)n;
    return buf;
}

fat32_err_t fat32_write_file(const char *path, const void *data, uint32_t len)
{
    int fd = fat32_open(path, true);
    if (fd < 0) return (fat32_err_t)fd;
    int n = fat32_write(fd, data, len);
    fat32_close(fd);
    return (n < 0) ? FAT32_ERR_IO : FAT32_OK;
}

fat32_err_t fat32_append_file(const char *path, const void *data, uint32_t len)
{
    /* Open for read to get size, then reopen for write at end */
    int32_t sz = fat32_filesize(path);
    if (sz < 0) {
        /* File doesn't exist, create it */
        return fat32_write_file(path, data, len);
    }

    /* Read existing content */
    uint32_t old_sz = (uint32_t)sz;
    uint8_t *buf = (uint8_t *)kmalloc(old_sz + len + 1);
    if (!buf) return FAT32_ERR_NOMEM;

    int fd = fat32_open(path, false);
    if (fd >= 0) {
        fat32_read(fd, buf, old_sz);
        fat32_close(fd);
    }
    f_memcpy(buf + old_sz, data, len);

    fat32_err_t err = fat32_write_file(path, buf, old_sz + len);
    kfree(buf);
    return err;
}

const char *fat32_strerror(fat32_err_t err)
{
    switch (err) {
    case FAT32_OK:           return "OK";
    case FAT32_ERR_NOTFOUND: return "File not found";
    case FAT32_ERR_EXISTS:   return "Already exists";
    case FAT32_ERR_FULL:     return "Disk full";
    case FAT32_ERR_IO:       return "I/O error";
    case FAT32_ERR_NOTDIR:   return "Not a directory";
    case FAT32_ERR_ISDIR:    return "Is a directory";
    case FAT32_ERR_NOMEM:    return "Out of memory";
    case FAT32_ERR_NODEV:    return "No device";
    case FAT32_ERR_NOTEMPTY: return "Directory not empty";
    default:                 return "Unknown error";
    }
}

/* ── FSCK bridge API (called from fsck.c) ───────────────────────────────── */

void fsck_get_vol_params(uint32_t *out_fat_lba, uint32_t *out_fat_size,
                         uint32_t *out_num_fats, uint32_t *out_total_clusters,
                         uint32_t *out_root_cluster)
{
    *out_fat_lba        = vol_fat_lba;
    *out_fat_size       = vol_fat_size;
    *out_num_fats       = vol_num_fats;
    *out_total_clusters = vol_total_clusters;
    *out_root_cluster   = vol_root_cluster;
}

uint32_t fsck_fat_get(uint32_t cluster)
{
    return fat_get(cluster);
}

void fsck_fat_free_chain(uint32_t start)
{
    fat_free_chain(start);
}

void fsck_dir_iter_init(fsck_dir_iter_t *it, uint32_t cluster)
{
    it->cluster   = cluster;
    it->entry_idx = 0;
    it->lba       = cluster_to_lba(cluster);
    it->off       = 0;
}

bool fsck_dir_iter_next(fsck_dir_iter_t *it, fsck_raw_entry_t *re)
{
    if (!sec_read(it->lba)) return false;
    f_memcpy(re->raw, sec_cache + it->off, 32);
    /* Advance position */
    it->off += 32;
    it->entry_idx++;
    if (it->off >= SECTOR_SIZE) {
        it->off = 0;
        it->lba++;
        uint32_t sec_in_clus = (it->lba - cluster_to_lba(it->cluster))
                               % vol_sec_per_clus;
        if (sec_in_clus == 0 &&
            it->lba != cluster_to_lba(it->cluster)) {
            uint32_t next_clus = fat_get(it->cluster);
            if (next_clus >= FAT32_EOC || next_clus < 2) return true;
            it->cluster = next_clus;
            it->lba = cluster_to_lba(next_clus);
        }
    }
    return true;
}

bool fsck_delete_entry(fsck_dir_iter_t *it)
{
    /* The iterator has already advanced; the entry is at (lba, off - 32) */
    uint32_t lba = it->lba;
    uint32_t off = it->off;
    if (off == 0) {
        /* Entry was the last in the previous sector */
        lba--;
        off = SECTOR_SIZE - 32;
    } else {
        off -= 32;
    }
    if (!sec_read(lba)) return false;
    sec_cache[off] = 0xE5;
    sec_cache_dirty = true;
    return sec_flush();
}

void fsck_delete_entries_range(fsck_dir_iter_t *it,
                               uint32_t first_entry_idx,
                               uint32_t last_entry_idx,
                               uint32_t dir_cluster)
{
    /* Re-scan from first_entry_idx to last_entry_idx and mark each 0xE5 */
    fsck_dir_iter_t scan;
    fsck_dir_iter_init(&scan, dir_cluster);
    for (uint32_t i = 0; i <= last_entry_idx; i++) {
        fsck_raw_entry_t re;
        if (!fsck_dir_iter_next(&scan, &re)) break;
        if (i < first_entry_idx) continue;
        /* Mark this entry deleted */
        uint32_t lba = scan.lba;
        uint32_t off = scan.off;
        if (off == 0) { lba--; off = SECTOR_SIZE - 32; } else { off -= 32; }
        if (!sec_read(lba)) continue;
        sec_cache[off] = 0xE5;
        sec_cache_dirty = true;
        sec_flush();
    }
    (void)it;
}

void fsck_entry_fields(const fsck_raw_entry_t *re,
                       uint32_t *out_fst_clus_hi,
                       uint32_t *out_fst_clus_lo,
                       uint32_t *out_file_size)
{
    /* fat32_dir_entry_t layout:
     *  [0..7]   name
     *  [8..10]  ext
     *  [11]     attr
     *  [20..21] fst_clus_hi (little-endian uint16)
     *  [26..27] fst_clus_lo (little-endian uint16)
     *  [28..31] file_size   (little-endian uint32)
     */
    uint16_t hi, lo;
    uint32_t sz;
    f_memcpy(&hi, re->raw + 20, 2);
    f_memcpy(&lo, re->raw + 26, 2);
    f_memcpy(&sz, re->raw + 28, 4);
    *out_fst_clus_hi = hi;
    *out_fst_clus_lo = lo;
    *out_file_size   = sz;
}

void fsck_mark_chain(uint8_t *reach_bmp,
                     void *ref_table,
                     uint32_t start,
                     uint32_t dir_cluster,
                     uint32_t entry_idx,
                     uint32_t vol_total_clusters_arg,
                     fsck_stats_t *stats,
                     bool *need_readonly)
{
    /* clus_ref_t is defined in fsck.c; we cast through void* */
    typedef struct {
        uint32_t ref_cluster;
        uint32_t ref_entry;
        bool     claimed;
    } local_ref_t;
    local_ref_t *refs = (local_ref_t *)ref_table;

    uint32_t c = start;
    uint32_t guard = 0;
    while (c >= 2 && c < vol_total_clusters_arg + 2 &&
           guard < vol_total_clusters_arg) {
        uint32_t idx = c - 2;
        if (reach_bmp[idx / 8] & (uint8_t)(1u << (idx % 8))) {
            /* Already visited — check for cross-link.
             * Exception: when start == dir_cluster, this is walk_dir() marking
             * the directory's own cluster chain.  The parent directory already
             * claimed these clusters when it processed the dir entry, so this
             * is NOT a cross-link — just update the owner to this directory. */
            if (start == dir_cluster) {
                /* Self-mark: update ownership to this directory, no error */
                refs[idx].ref_cluster = dir_cluster;
                refs[idx].ref_entry   = entry_idx;
                /* Continue following the chain to update all clusters */
            } else if (refs[idx].ref_cluster != dir_cluster ||
                       refs[idx].ref_entry   != entry_idx) {
                stats->crosslink_clusters++;
                serial_puts("[FSCK] C3: cross-link at cluster ");
                /* simple hex print */
                static const char hx2[] = "0123456789abcdef";
                char hbuf[11];
                hbuf[0]='0'; hbuf[1]='x';
                uint32_t v = c;
                for (int i=9;i>=2;i--){hbuf[i]=hx2[v&0xf];v>>=4;}
                hbuf[10]='\0';
                serial_puts(hbuf);
                serial_puts(" (second ref from dir=");
                v = dir_cluster;
                for (int i=9;i>=2;i--){hbuf[i]=hx2[v&0xf];v>>=4;}
                serial_puts(hbuf);
                serial_puts(")\n");
                /* Repair: zero out the cluster pointer in the second dir entry
                 * by returning here — the caller (walk_dir) will see that
                 * first_cluster is already claimed and will zero the entry. */
                *need_readonly = false;  /* repair is possible */
                stats->crosslink_repaired++;
                break;  /* stop following this chain */
            } else {
                break;  /* same owner, already fully marked */
            }
            /* For self-mark: don't break, continue to next cluster */
            uint32_t next_self = fat_get(c);
            if (next_self >= FAT32_EOC || next_self == 0) break;
            c = next_self;
            guard++;
            continue;
        }
        reach_bmp[idx / 8] |= (uint8_t)(1u << (idx % 8));
        refs[idx].claimed     = true;
        refs[idx].ref_cluster = dir_cluster;
        refs[idx].ref_entry   = entry_idx;
        uint32_t next = fat_get(c);
        if (next >= FAT32_EOC || next == 0) break;
        c = next;
        guard++;
    }
}

uint8_t fsck_lfn_checksum(const uint8_t name83[11])
{
    return lfn_checksum((const char *)name83);
}

bool fsck_zero_entry_cluster(uint32_t dir_cluster, uint32_t entry_idx)
{
    /* Walk to the entry_idx-th entry in dir_cluster and zero its
     * fst_clus_hi, fst_clus_lo, and file_size fields. */
    fsck_dir_iter_t it;
    fsck_dir_iter_init(&it, dir_cluster);
    for (uint32_t i = 0; i <= entry_idx; i++) {
        fsck_raw_entry_t re;
        if (!fsck_dir_iter_next(&it, &re)) return false;
        if (i < entry_idx) continue;
        /* Found the entry — compute its LBA and offset */
        uint32_t lba = it.lba;
        uint32_t off = it.off;
        if (off == 0) { lba--; off = SECTOR_SIZE - 32; } else { off -= 32; }
        if (!sec_read(lba)) return false;
        /* Zero fst_clus_hi (bytes 20-21), fst_clus_lo (bytes 26-27),
         * and file_size (bytes 28-31) */
        sec_cache[off + 20] = 0; sec_cache[off + 21] = 0;
        sec_cache[off + 26] = 0; sec_cache[off + 27] = 0;
        sec_cache[off + 28] = 0; sec_cache[off + 29] = 0;
        sec_cache[off + 30] = 0; sec_cache[off + 31] = 0;
        sec_cache_dirty = true;
        return sec_flush();
    }
    return false;
}

bool fsck_adopt_chain(const char *path, uint32_t first_cluster)
{
    /* Resolve parent directory */
    char name[FAT32_MAX_NAME];
    uint32_t parent = path_resolve_parent(path, name);
    if (!parent || name[0] == '\0') return false;

    /* Compute chain length in bytes so fsck.fat does not complain about
     * "file_size=0 but cluster chain length > 0". */
    uint32_t cluster_size = vol_sec_per_clus * SECTOR_SIZE;
    uint32_t chain_len    = 0;
    uint32_t c = first_cluster;
    uint32_t guard = 0;
    while (c >= 2 && c < FAT32_EOC && guard < vol_total_clusters) {
        chain_len += cluster_size;
        c = fat_get(c);
        guard++;
    }

    /* Build a minimal 8.3 dir entry pointing to the existing chain */
    char name83[11];
    make_83_unique(parent, name, name83);

    dir_pos_t new_pos;
    bool ok = dir_add_entry(parent, name,
                            FAT32_ATTR_ARCHIVE,
                            first_cluster,
                            chain_len,
                            &new_pos);
    return ok;
}

void fsck_set_readonly(void)
{
    vol_readonly = true;
}

void fat32_print_stats(void)
{
    if (!vol_mounted) { vga_puts("/persist: not mounted\n"); return; }
    vga_puts("/persist: FAT32, ");
    /* Count free clusters */
    uint32_t free_clus = 0;
    for (uint32_t c = 2; c < vol_total_clusters + 2; c++) {
        if (fat_get(c) == FAT32_FREE) free_clus++;
    }
    uint32_t free_mb = (free_clus * vol_sec_per_clus * SECTOR_SIZE) / (1024*1024);
    uint32_t total_mb= (vol_total_clusters * vol_sec_per_clus * SECTOR_SIZE) / (1024*1024);
    char buf[32];
    /* Print total MB */
    int ni = 0;
    uint32_t n = total_mb;
    if (!n) buf[ni++]='0';
    else { while(n){buf[ni++]='0'+n%10;n/=10;} }
    for(int i=ni-1;i>=0;i--) vga_putchar(buf[i]);
    vga_puts(" MB total, ");
    ni = 0; n = free_mb;
    if (!n) buf[ni++]='0';
    else { while(n){buf[ni++]='0'+n%10;n/=10;} }
    for(int i=ni-1;i>=0;i--) vga_putchar(buf[i]);
    vga_puts(" MB free\n");
}
