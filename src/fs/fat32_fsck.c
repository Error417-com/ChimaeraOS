/*
 * ChimaeraOS - FAT32 Driver — FSCK Bridge API
 * fs/fat32_fsck.c
 *
 * All fsck_* functions called from fsck.c.  These bridge the FAT32
 * driver internals (sector cache, FAT table, directory iterators) to
 * the filesystem checker.
 *
 * Split from the original monolithic fat32.c.
 * See fat32_internal.h for shared types and declarations.
 */
#include "fat32_internal.h"

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
    uint32_t lba = it->lba;
    uint32_t off = it->off;
    if (off == 0) {
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
    fsck_dir_iter_t scan;
    fsck_dir_iter_init(&scan, dir_cluster);
    for (uint32_t i = 0; i <= last_entry_idx; i++) {
        fsck_raw_entry_t re;
        if (!fsck_dir_iter_next(&scan, &re)) break;
        if (i < first_entry_idx) continue;
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
                refs[idx].ref_cluster = dir_cluster;
                refs[idx].ref_entry   = entry_idx;
            } else if (refs[idx].ref_cluster != dir_cluster ||
                       refs[idx].ref_entry   != entry_idx) {
                stats->crosslink_clusters++;
                serial_puts("[FSCK] C3: cross-link at cluster ");
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
                *need_readonly = false;
                stats->crosslink_repaired++;
                break;
            } else {
                break;
            }
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
    fsck_dir_iter_t it;
    fsck_dir_iter_init(&it, dir_cluster);
    for (uint32_t i = 0; i <= entry_idx; i++) {
        fsck_raw_entry_t re;
        if (!fsck_dir_iter_next(&it, &re)) return false;
        if (i < entry_idx) continue;
        uint32_t lba = it.lba;
        uint32_t off = it.off;
        if (off == 0) { lba--; off = SECTOR_SIZE - 32; } else { off -= 32; }
        if (!sec_read(lba)) return false;
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
    char name[FAT32_MAX_NAME];
    uint32_t parent = path_resolve_parent(path, name);
    if (!parent || name[0] == '\0') return false;

    uint32_t cluster_size = vol_sec_per_clus * SECTOR_SIZE;
    uint32_t chain_len    = 0;
    uint32_t c = first_cluster;
    uint32_t guard = 0;
    while (c >= 2 && c < FAT32_EOC && guard < vol_total_clusters) {
        chain_len += cluster_size;
        c = fat_get(c);
        guard++;
    }

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
