/*
 * ChimaeraOS - FAT32 Driver — Core
 * fs/fat32_core.c
 *
 * Volume state, sector I/O with cache, FAT geometry helpers, LFN helpers,
 * directory scanning, path resolution, FAT32 formatting, on-mount orphan
 * reclaim and FSInfo repair, and the mount/unmount entry points.
 *
 * Split from the original monolithic fat32.c.
 * See fat32_internal.h for shared types and declarations.
 */
#include "fat32_internal.h"

/* ── Volume state ────────────────────────────────────────────────────────── */

bool     vol_mounted       = false;
uint32_t vol_lba_start     = 0;
uint32_t vol_bytes_per_sec = 512;
uint32_t vol_sec_per_clus  = 0;
uint32_t vol_reserved_sec  = 0;
uint32_t vol_num_fats      = 0;
uint32_t vol_fat_size      = 0;
uint32_t vol_root_cluster  = 2;
uint32_t vol_fat_lba       = 0;
uint32_t vol_data_lba      = 0;
uint32_t vol_total_clusters= 0;

/* Sector cache: one sector at a time */
uint8_t  sec_cache[SECTOR_SIZE];
uint32_t sec_cache_lba   = 0xFFFFFFFF;
bool     sec_cache_dirty  = false;

/* File descriptor table */
fat32_fd_t fds[FAT32_MAX_FD];

/* Read-only flag set by chimerafs_fsck() on unrecoverable errors */
bool vol_readonly = false;

/* ── Low-level sector I/O with cache ─────────────────────────────────────── */

bool sec_flush(void)
{
    if (!sec_cache_dirty) return true;
    if (ata_write(sec_cache_lba, 1, sec_cache) != ATA_OK) return false;
    sec_cache_dirty = false;
    return true;
}

bool sec_read(uint32_t lba)
{
    if (sec_cache_lba == lba) return true;
    if (!sec_flush()) return false;
    if (ata_read(lba, 1, sec_cache) != ATA_OK) return false;
    sec_cache_lba = lba;
    return true;
}

bool sec_write(uint32_t lba, const uint8_t *data)
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

uint32_t cluster_to_lba(uint32_t cluster)
{
    return vol_data_lba + (cluster - 2) * vol_sec_per_clus;
}

uint32_t fat_lba_for_cluster(uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    return vol_fat_lba + fat_offset / SECTOR_SIZE;
}

uint32_t fat_offset_in_sector(uint32_t cluster)
{
    return (cluster * 4) % SECTOR_SIZE;
}

/* Read FAT entry for a cluster */
uint32_t fat_get(uint32_t cluster)
{
    uint32_t lba = fat_lba_for_cluster(cluster);
    if (!sec_read(lba)) return 0x0FFFFFF7;
    uint32_t off = fat_offset_in_sector(cluster);
    uint32_t val;
    f_memcpy(&val, sec_cache + off, 4);
    return val & 0x0FFFFFFF;
}

/* Write FAT entry (both FAT copies) */
bool fat_set(uint32_t cluster, uint32_t value)
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

/* Allocate a free cluster and link it after `prev` (0 = start of chain) */
uint32_t fat_alloc(uint32_t prev)
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
void fat_free_chain(uint32_t start)
{
    uint32_t c = start;
    while (c >= 2 && c < FAT32_EOC) {
        uint32_t next = fat_get(c);
        fat_set(c, FAT32_FREE);
        c = next;
    }
}

/* Follow a cluster chain `n` clusters forward */
uint32_t fat_follow(uint32_t start, uint32_t n)
{
    uint32_t c = start;
    for (uint32_t i = 0; i < n; i++) {
        if (c >= FAT32_EOC || c < 2) return 0;
        c = fat_get(c);
    }
    return c;
}

/* ── Directory scanning ──────────────────────────────────────────────────── */

/* Read a directory entry at position `pos` */
bool dir_read_entry(const dir_pos_t *pos, fat32_dir_entry_t *out)
{
    if (!sec_read(pos->lba)) return false;
    f_memcpy(out, sec_cache + pos->off, DIR_ENTRY_SIZE);
    return true;
}

/* Write a directory entry at position `pos` */
bool dir_write_entry(const dir_pos_t *pos, const fat32_dir_entry_t *e)
{
#ifdef LFN_TEST
    {
        static const char hx[] = "0123456789abcdef";
        char dbuf[96];
        int di = 0;
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
bool dir_next(dir_pos_t *pos)
{
    pos->entry_idx++;
    pos->off += DIR_ENTRY_SIZE;
    if (pos->off >= SECTOR_SIZE) {
        pos->off = 0;
        pos->lba++;
        uint32_t sec_in_clus = (pos->lba - cluster_to_lba(pos->cluster))
                               % vol_sec_per_clus;
        if (sec_in_clus == 0 &&
            pos->lba != cluster_to_lba(pos->cluster)) {
            uint32_t next_clus = fat_get(pos->cluster);
            if (next_clus >= FAT32_EOC || next_clus < 2) return false;
            pos->cluster = next_clus;
            pos->lba = cluster_to_lba(next_clus);
        }
    }
    return true;
}

/* Initialize a dir_pos_t to the start of a directory cluster */
void dir_pos_init(dir_pos_t *pos, uint32_t cluster)
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
bool dir_83_exists(uint32_t dir_cluster, const char name83[11])
{
    dir_pos_t pos;
    dir_pos_init(&pos, dir_cluster);
    for (;;) {
        fat32_dir_entry_t e;
        if (!dir_read_entry(&pos, &e)) break;
        uint8_t first = (uint8_t)e.name[0];
        if (first == 0x00) break;
        if (first == 0xE5) goto next83;
        if (e.attr == FAT32_ATTR_LFN) goto next83;
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
 */
void make_83_unique(uint32_t dir_cluster, const char *name, char out[11])
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
    if (!dir_83_exists(dir_cluster, out)) return;
#ifdef LFN_TEST
    serial_puts("[83] COLLISION\n");
#endif

    char ext[3];
    ext[0] = out[8]; ext[1] = out[9]; ext[2] = out[10];

    for (int n = 1; n <= 9999; n++) {
        char digits[5];
        int dlen = 0;
        int tmp = n;
        do { digits[dlen++] = '0' + (tmp % 10); tmp /= 10; } while (tmp);
        int suffix_len = 1 + dlen;
        int stem_max = 8 - suffix_len;
        if (stem_max < 1) break;

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
        out[ni++] = '~';
        for (int d = dlen - 1; d >= 0; d--) out[ni++] = digits[d];
        out[8] = ext[0]; out[9] = ext[1]; out[10] = ext[2];

        if (!dir_83_exists(dir_cluster, out)) return;
    }
    make_83(name, out);
}

/*
 * Scan a directory for a named entry.
 * Handles LFN. Fills `out_pos` with the position of the 8.3 entry.
 * If `out_lfn_start` is non-NULL, fills it with the position of the first
 * LFN entry (or the 8.3 entry itself when there is no LFN).
 * Returns true if found.
 */
bool dir_find(uint32_t dir_cluster, const char *name,
              fat32_dir_entry_t *out_entry, dir_pos_t *out_pos,
              dir_pos_t *out_lfn_start)
{
    char lfn_buf[FAT32_MAX_NAME];
    int  lfn_parts = 0;
    bool building_lfn = false;
    dir_pos_t lfn_start_pos;

    dir_pos_t pos;
    dir_pos_init(&pos, dir_cluster);
    lfn_start_pos = pos;

    for (;;) {
        fat32_dir_entry_t e;
        if (!dir_read_entry(&pos, &e)) return false;

        uint8_t first = (uint8_t)e.name[0];
        if (first == 0x00) break;
        if (first == 0xE5) {
            building_lfn = false;
            lfn_parts = 0;
            if (!dir_next(&pos)) break;
            continue;
        }

        if (e.attr == FAT32_ATTR_LFN) {
            const fat32_lfn_entry_t *lfn = (const fat32_lfn_entry_t *)&e;
            int seq = lfn->order & 0x1F;
            if (lfn->order & 0x40) {
                f_memset(lfn_buf, 0, sizeof(lfn_buf));
                lfn_parts = seq;
                building_lfn = true;
                lfn_start_pos = pos;
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
            bool match = false;

            if (building_lfn && lfn_parts > 0) {
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
            lfn_start_pos = pos;
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
uint32_t path_resolve_parent(const char *path, char *name_out)
{
    if (!path || path[0] != '/') return 0;

    uint32_t cluster = vol_root_cluster;
    const char *p = path + 1;

    while (*p) {
        char comp[FAT32_MAX_NAME];
        int ci = 0;
        while (*p && *p != '/') {
            if (ci < FAT32_MAX_NAME - 1) comp[ci++] = *p;
            p++;
        }
        comp[ci] = '\0';
        if (*p == '/') p++;

        if (*p == '\0') {
            if (name_out) f_strcpy(name_out, comp);
            return cluster;
        }

        fat32_dir_entry_t e;
        if (!dir_find(cluster, comp, &e, NULL, NULL)) return 0;
        if (!(e.attr & FAT32_ATTR_DIR)) return 0;

        cluster = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;
        if (cluster == 0) cluster = vol_root_cluster;
    }

    if (name_out) name_out[0] = '\0';
    return cluster;
}

/* Resolve a full path to a directory entry */
bool path_stat(const char *path, fat32_dir_entry_t *out_e,
               dir_pos_t *out_pos, uint32_t *out_dir_cluster)
{
    if (f_strcmp(path, "/") == 0) {
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

    uint32_t sec_per_clus = 8;
    uint32_t reserved     = 32;
    uint32_t num_fats     = 2;

    uint32_t data_sectors = total_sectors - reserved;
    uint32_t total_clusters = data_sectors / sec_per_clus;
    uint32_t fat_size = (total_clusters * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    fat_size = (fat_size + 7) & ~7u;

    data_sectors = total_sectors - reserved - num_fats * fat_size;
    total_clusters = data_sectors / sec_per_clus;

    f_memset(buf, 0, SECTOR_SIZE);
    fat32_bpb_t *bpb = (fat32_bpb_t *)buf;
    buf[0] = 0xEB; buf[1] = 0x58; buf[2] = 0x90;
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

    f_memset(buf, 0, SECTOR_SIZE);
    uint32_t fat_lba = lba_start + reserved;
    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t base = fat_lba + f * fat_size;
        for (uint32_t s = 0; s < fat_size; s++) {
            ata_write(base + s, 1, buf);
        }
    }

    uint32_t *fat_buf = (uint32_t *)buf;
    f_memset(buf, 0, SECTOR_SIZE);
    fat_buf[0] = 0x0FFFFFF8;
    fat_buf[1] = 0x0FFFFFFF;
    fat_buf[2] = 0x0FFFFFFF;
    for (uint32_t f = 0; f < num_fats; f++) {
        ata_write(fat_lba + f * fat_size, 1, buf);
    }

    f_memset(buf, 0, SECTOR_SIZE);
    uint32_t data_lba = fat_lba + num_fats * fat_size;
    uint32_t root_lba = data_lba;
    for (uint32_t s = 0; s < sec_per_clus; s++) {
        ata_write(root_lba + s, 1, buf);
    }

    return true;
}

/* ── FIX 2b: On-mount orphan reclaim and FSInfo repair ──────────────────── */

static void mark_chain_reachable(uint8_t *bmp, uint32_t start)
{
    uint32_t c = start;
    while (c >= 2 && c < vol_total_clusters + 2) {
        uint32_t idx = c - 2;
        if (bmp[idx / 8] & (1u << (idx % 8))) break;
        bmp[idx / 8] |= (1u << (idx % 8));
        uint32_t next = fat_get(c);
        if (next >= 0x0FFFFFF8 || next == 0) break;
        c = next;
    }
}

static void mark_dir_reachable(uint8_t *bmp, uint32_t dir_cluster, int depth)
{
    if (depth > 16 || dir_cluster < 2) return;

    mark_chain_reachable(bmp, dir_cluster);

    dir_pos_t pos;
    dir_pos_init(&pos, dir_cluster);
    for (;;) {
        fat32_dir_entry_t e;
        if (!dir_read_entry(&pos, &e)) break;
        uint8_t first = (uint8_t)e.name[0];
        if (first == 0x00) break;
        if (first == 0xE5) { dir_next(&pos); continue; }
        if (e.attr == FAT32_ATTR_LFN) { dir_next(&pos); continue; }
        if (e.attr & 0x08) { dir_next(&pos); continue; }

        uint32_t sc = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;
        if (sc >= 2) {
            mark_chain_reachable(bmp, sc);
            if ((e.attr & FAT32_ATTR_DIR) &&
                f_memcmp(e.name, ".       ", 8) != 0 &&
                f_memcmp(e.name, "..      ", 8) != 0) {
                mark_dir_reachable(bmp, sc, depth + 1);
            }
        }
        if (!dir_next(&pos)) break;
    }
}

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
        if (e.attr & 0x08) { dir_next(&pos); continue; }

        uint32_t sc = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;

        if (!(e.attr & FAT32_ATTR_DIR) && e.file_size == 0 && sc >= 2) {
            free_chain(sc);
            e.fst_clus_hi = 0;
            e.fst_clus_lo = 0;
            dir_write_entry(&pos, &e);
        }

        if ((e.attr & FAT32_ATTR_DIR) && sc >= 2 &&
            f_memcmp(e.name, ".       ", 8) != 0 &&
            f_memcmp(e.name, "..      ", 8) != 0) {
            repair_dir_zero_size(sc, depth + 1);
        }

        if (!dir_next(&pos)) break;
    }
}

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
        if (e.attr & 0x08) { dir_next(&pos); continue; }

        uint32_t sc = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;

        if (!(e.attr & FAT32_ATTR_DIR) && sc >= 2) {
            uint32_t fat_val = fat_get(sc);
            if (fat_val == FAT32_FREE) {
                e.fst_clus_hi = 0;
                e.fst_clus_lo = 0;
                e.file_size   = 0;
                dir_write_entry(&pos, &e);
            }
        }

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

    repair_dir_zero_size(vol_root_cluster, 0);
    repair_dir_dangling_cluster(vol_root_cluster, 0);

    uint32_t bmp_bytes = (vol_total_clusters + 7) / 8;
    uint8_t *bmp = (uint8_t *)kmalloc(bmp_bytes);
    if (!bmp) return;
    f_memset(bmp, 0, bmp_bytes);

    mark_dir_reachable(bmp, vol_root_cluster, 0);

    uint32_t orphans_freed = 0;
    uint32_t free_count    = 0;
    for (uint32_t c = 2; c < vol_total_clusters + 2; c++) {
        uint32_t val = fat_get(c);
        if (val == FAT32_FREE) {
            free_count++;
            continue;
        }
        if (val == 0x0FFFFFF7) continue;
        uint32_t idx = c - 2;
        if (!(bmp[idx / 8] & (1u << (idx % 8)))) {
            fat_set(c, FAT32_FREE);
            free_count++;
            orphans_freed++;
        }
    }

    kfree(bmp);

    uint8_t fsinfo[SECTOR_SIZE];
    uint32_t fsinfo_lba = vol_lba_start + 1;
    if (ata_read(fsinfo_lba, 1, fsinfo) == ATA_OK) {
        uint32_t sig1, sig2;
        f_memcpy(&sig1, fsinfo + 0,   4);
        f_memcpy(&sig2, fsinfo + 484, 4);
        if (sig1 == 0x41615252 && sig2 == 0x61417272) {
            f_memcpy(fsinfo + 488, &free_count, 4);
            uint32_t next_free = 0xFFFFFFFF;
            f_memcpy(fsinfo + 492, &next_free, 4);
            ata_write(fsinfo_lba, 1, fsinfo);
        }
    }
    sec_flush();
    sec_cache_lba = 0xFFFFFFFF;
    sec_cache_dirty = false;

    (void)orphans_freed;

#ifdef POWERFAIL_TEST
    serial_puts("[PFTEST] REPAIR_DONE\n");
#endif
}

/* ── Mount ───────────────────────────────────────────────────────────────── */

bool fat32_mount(void)
{
    if (!ata_disk_present()) return false;

    uint8_t mbr[SECTOR_SIZE];
    if (ata_read(0, 1, mbr) != ATA_OK) return false;

    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        vga_puts("[FAT32] No MBR found, formatting disk...\n");
        uint32_t total = ata_disk_sectors();
        if (total == 0) total = 131072;
        if (!fat32_format(0, total)) return false;
        if (ata_read(0, 1, mbr) != ATA_OK) return false;
    }

    uint32_t lba_start = 0;
    mbr_partition_t *parts = (mbr_partition_t *)(mbr + 0x1BE);
    for (int i = 0; i < 4; i++) {
        if (parts[i].type == 0x0B || parts[i].type == 0x0C) {
            lba_start = parts[i].lba_start;
            break;
        }
    }

    uint8_t boot[SECTOR_SIZE];
    if (ata_read(lba_start, 1, boot) != ATA_OK) return false;

    if (boot[510] != 0x55 || boot[511] != 0xAA) {
        vga_puts("[FAT32] Invalid boot sector, formatting...\n");
        uint32_t total = ata_disk_sectors();
        if (!fat32_format(lba_start, total - lba_start)) return false;
        if (ata_read(lba_start, 1, boot) != ATA_OK) return false;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)boot;

    if (f_memcmp(bpb->fs_type, "FAT32", 5) != 0) {
        vga_puts("[FAT32] Not FAT32, reformatting...\n");
        uint32_t total = ata_disk_sectors();
        if (!fat32_format(lba_start, total - lba_start)) return false;
        if (ata_read(lba_start, 1, boot) != ATA_OK) return false;
        bpb = (fat32_bpb_t *)boot;
    }

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

    f_memset(fds, 0, sizeof(fds));

    /*
     * FIX 1 — FAT2 mirror repair on mount.
     */
    if (vol_num_fats >= 2) {
        uint8_t fat1_sec[SECTOR_SIZE];
        uint8_t fat2_sec[SECTOR_SIZE];
        uint32_t fat2_lba_base = vol_fat_lba + vol_fat_size;
        for (uint32_t s = 0; s < vol_fat_size; s++) {
            if (ata_read(vol_fat_lba + s, 1, fat1_sec) != ATA_OK) break;
            if (ata_read(fat2_lba_base + s, 1, fat2_sec) != ATA_OK) break;
            if (f_memcmp(fat1_sec, fat2_sec, SECTOR_SIZE) != 0) {
                ata_write(fat2_lba_base + s, 1, fat1_sec);
            }
        }
        sec_cache_lba = 0xFFFFFFFF;
        sec_cache_dirty = false;
    }

    /*
     * FIX 2b — Orphan reclaim and FSInfo repair.
     */
    vol_mounted = true;
    chimerafs_fsck();
    vol_mounted = false;

    vol_mounted = true;
    vga_puts("[FAT32] Mounted /persist (");
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
