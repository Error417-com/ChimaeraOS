/*
 * ChimaeraOS - FAT32 Boot-Time Consistency Checker
 * fs/fsck.c
 *
 * chimerafs_fsck() is called from fat32_mount() after the volume parameters
 * are parsed but before vol_mounted is set to true.  It performs five checks
 * in a single directory-tree pass, logs a summary to the serial console, and
 * attempts in-place repair where possible.  If repair is impossible the volume
 * is left mounted read-only (vol_readonly = true).
 *
 * Checks performed:
 *   C1 — FAT table mismatch (FAT1 vs FAT2)
 *   C2 — Orphaned clusters (allocated in FAT but not reachable from any dir)
 *   C3 — Cross-linked cluster chains (two dir entries share a cluster)
 *   C4 — Invalid directory entries (bad attribute, bad cluster pointer, etc.)
 *   C5 — Orphaned LFN entries (LFN without a matching 8.3 short name)
 *
 * Repair actions:
 *   C1 — Copy FAT1 → FAT2 for divergent sectors.
 *   C2 — Move orphaned chains to /lost+found/FILE_XXXX.
 *   C3 — Zero out the cluster pointer in the second dir entry that references
 *         the shared cluster (the first reference wins).
 *   C4 — Zero out the invalid dir entry (mark as deleted, 0xE5).
 *   C5 — Mark orphaned LFN entries as deleted (0xE5).
 *
 * Read-only fallback:
 *   If any repair write fails (I/O error), vol_readonly is set to true and
 *   subsequent fat32_open(write=true) calls return FAT32_ERR_IO.
 */

#include "../include/fat32.h"
#include "../include/fsck.h"
#include "../include/ata.h"
#include "../include/mm.h"
#include "../include/vga.h"
#include "../include/serial.h"
#include "../include/types.h"

/* ── Forward declarations of fat32.c internals exposed via fsck.h ─────────── */
/* (See fsck.h for the extern declarations.) */

/* ── Serial output helpers ───────────────────────────────────────────────── */

static void fsck_puts(const char *s) { serial_puts(s); }

static void fsck_put_u32(uint32_t v)
{
    static const char hx[] = "0123456789abcdef";
    char buf[11];
    buf[0]='0'; buf[1]='x';
    for (int i = 9; i >= 2; i--) { buf[i] = hx[v & 0xf]; v >>= 4; }
    buf[10] = '\0';
    fsck_puts(buf);
}

static void fsck_put_dec(uint32_t v)
{
    char buf[12];
    int i = 0;
    if (v == 0) { fsck_puts("0"); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    for (int j = i - 1; j >= 0; j--) {
        char c = buf[j]; serial_puts((char[]){c, '\0'});
    }
}

/* ── Cluster bitmap helpers ──────────────────────────────────────────────── */

static inline void bmp_set(uint8_t *bmp, uint32_t idx)
{
    bmp[idx / 8] |= (uint8_t)(1u << (idx % 8));
}

static inline bool bmp_get(const uint8_t *bmp, uint32_t idx)
{
    return (bmp[idx / 8] >> (idx % 8)) & 1u;
}

/* ── /lost+found management ──────────────────────────────────────────────── */

/*
 * Ensure /lost+found exists.  Returns its cluster, or 0 on failure.
 * We call fat32_mkdir which is idempotent if the dir already exists.
 */
static uint32_t ensure_lost_found(void)
{
    /* fat32_mkdir returns FAT32_OK or FAT32_ERR_EXISTS — both are fine */
    fat32_mkdir("/lost+found");

    fat32_dirent_t de;
    if (fat32_stat("/lost+found", &de) != FAT32_OK) return 0;
    return de.cluster;
}

/*
 * Move an orphaned cluster chain to /lost+found/FILE_XXXX where XXXX is the
 * starting cluster number in hex.  We create a directory entry pointing to the
 * chain with a file_size of 0 (unknown — we don't walk the chain to count
 * bytes).
 */
static bool rescue_chain(uint32_t start_cluster, uint32_t lf_cluster,
                          uint32_t *lf_seq,
                          /* fat32 internals passed in: */
                          uint32_t vol_data_lba, uint32_t vol_sec_per_clus,
                          uint32_t vol_fat_lba, uint32_t vol_fat_size,
                          uint32_t vol_total_clusters)
{
    (void)vol_data_lba; (void)vol_sec_per_clus;
    (void)vol_fat_lba; (void)vol_fat_size; (void)vol_total_clusters;

    /* Build path: /lost+found/FILE_XXXX */
    char path[64];
    static const char hx[] = "0123456789ABCDEF";
    uint32_t seq = (*lf_seq)++;
    int pi = 0;
    const char *prefix = "/lost+found/FILE_";
    for (int i = 0; prefix[i]; i++) path[pi++] = prefix[i];
    path[pi++] = hx[(seq >> 12) & 0xf];
    path[pi++] = hx[(seq >>  8) & 0xf];
    path[pi++] = hx[(seq >>  4) & 0xf];
    path[pi++] = hx[(seq >>  0) & 0xf];
    path[pi]   = '\0';

    (void)start_cluster; (void)lf_cluster;

    /*
     * We cannot call fat32_open(path, true) here because that would allocate
     * a NEW cluster for the file and leave the orphaned chain unreferenced.
     * Instead we use the internal fsck_adopt_chain() helper (declared in
     * fsck.h) which creates a directory entry pointing directly to the
     * existing chain without allocating a new cluster.
     */
    return fsck_adopt_chain(path, start_cluster);
}

/* ── Check C1: FAT table mismatch ────────────────────────────────────────── */

static uint32_t check_fat_mismatch(uint32_t vol_fat_lba, uint32_t vol_fat_size,
                                   uint32_t vol_num_fats,
                                   fsck_stats_t *stats)
{
    if (vol_num_fats < 2) return 0;

    uint8_t fat1[512], fat2[512];
    uint32_t fat2_base = vol_fat_lba + vol_fat_size;
    uint32_t repaired = 0;

    for (uint32_t s = 0; s < vol_fat_size; s++) {
        if (ata_read(vol_fat_lba + s, 1, fat1) != ATA_OK) continue;
        if (ata_read(fat2_base   + s, 1, fat2) != ATA_OK) continue;

        /* Compare 128 uint32_t entries */
        bool differ = false;
        for (int i = 0; i < 512; i++) {
            if (fat1[i] != fat2[i]) { differ = true; break; }
        }
        if (differ) {
            stats->fat_mismatch_sectors++;
            /* Repair: copy FAT1 → FAT2 */
            if (ata_write(fat2_base + s, 1, fat1) == ATA_OK) {
                repaired++;
            } else {
                stats->repair_failures++;
            }
        }
    }
    stats->fat_mismatch_repaired = repaired;
    return repaired;
}

/* ── Tree walk: build reachability bitmap and collect dir-entry info ──────── */

/*
 * Per-cluster reference record.  We keep a compact array indexed by
 * (cluster - 2) to detect cross-links.
 */
typedef struct {
    uint32_t ref_cluster;  /* cluster of the dir entry that first claimed this */
    uint32_t ref_entry;    /* entry_idx of the claiming dir entry */
    bool     claimed;
} clus_ref_t;

/*
 * Recursive directory walker.
 *
 * For each non-deleted, non-LFN directory entry:
 *   1. Mark all clusters in the file/dir chain as reachable.
 *   2. Check for cross-links (a cluster already claimed by another entry).
 *   3. Validate the directory entry fields.
 *   4. Check LFN consistency (orphaned LFN entries, checksum mismatch).
 *
 * `depth` prevents infinite recursion on circular directory chains.
 */
static void walk_dir(uint32_t dir_cluster, int depth,
                     uint8_t *reach_bmp, clus_ref_t *ref_table,
                     uint32_t vol_total_clusters,
                     fsck_stats_t *stats,
                     bool *need_readonly)
{
    if (depth > 32 || dir_cluster < 2) return;

    /* Mark the directory cluster chain itself as reachable */
    fsck_mark_chain(reach_bmp, (void *)ref_table, dir_cluster, dir_cluster, 0,
                    vol_total_clusters, stats, need_readonly);

    /* Scan directory entries */
    uint32_t entry_idx = 0;
    bool building_lfn  = false;
    int  lfn_seq_expected = 0;
    uint8_t lfn_checksum_expected = 0;
    uint32_t lfn_start_entry = 0;

    fsck_dir_iter_t it;
    fsck_dir_iter_init(&it, dir_cluster);

    for (;;) {
        fsck_raw_entry_t re;
        if (!fsck_dir_iter_next(&it, &re)) break;

        uint8_t first = re.raw[0];

        /* End of directory */
        if (first == 0x00) break;

        /* Deleted entry */
        if (first == 0xE5) {
            building_lfn = false;
            lfn_seq_expected = 0;
            entry_idx++;
            continue;
        }

        uint8_t attr = re.raw[11];

        /* LFN entry */
        if (attr == 0x0F) {
            uint8_t order    = re.raw[0];
            uint8_t checksum = re.raw[13];
            int seq = order & 0x1F;

            if (order & 0x40) {
                /* LAST LFN entry (first on disk) */
                building_lfn = true;
                lfn_seq_expected = seq;
                lfn_checksum_expected = checksum;
                lfn_start_entry = entry_idx;
            } else if (!building_lfn) {
                /* Orphaned LFN entry — no LAST marker seen */
                stats->orphan_lfn_entries++;
                fsck_puts("[FSCK] C5: orphaned LFN entry at entry ");
                fsck_put_dec(entry_idx);
                fsck_puts(" in cluster ");
                fsck_put_dec(dir_cluster);
                fsck_puts("\n");
                /* Repair: mark as deleted */
                if (!fsck_delete_entry(&it)) {
                    stats->repair_failures++;
                    *need_readonly = true;
                }
                entry_idx++;
                continue;
            } else {
                /* Validate sequence and checksum */
                if (seq != lfn_seq_expected - 1) {
                    stats->orphan_lfn_entries++;
                    fsck_puts("[FSCK] C5: LFN sequence gap at entry ");
                    fsck_put_dec(entry_idx);
                    fsck_puts("\n");
                    if (!fsck_delete_entry(&it)) {
                        stats->repair_failures++;
                        *need_readonly = true;
                    }
                    building_lfn = false;
                    lfn_seq_expected = 0;
                    entry_idx++;
                    continue;
                }
                if (checksum != lfn_checksum_expected) {
                    stats->orphan_lfn_entries++;
                    fsck_puts("[FSCK] C5: LFN checksum mismatch at entry ");
                    fsck_put_dec(entry_idx);
                    fsck_puts("\n");
                    if (!fsck_delete_entry(&it)) {
                        stats->repair_failures++;
                        *need_readonly = true;
                    }
                    building_lfn = false;
                    lfn_seq_expected = 0;
                    entry_idx++;
                    continue;
                }
                lfn_seq_expected = seq;
            }
            entry_idx++;
            continue;
        }

        /* 8.3 directory entry */

        /* C5: If we were building an LFN, verify the checksum against this
         *     8.3 entry's name.  If the LFN sequence is incomplete (we never
         *     saw seq=1), the LFN entries are orphaned. */
        if (building_lfn) {
            if (lfn_seq_expected != 1) {
                /* Incomplete LFN sequence — mark all LFN entries as deleted */
                stats->orphan_lfn_entries++;
                fsck_puts("[FSCK] C5: incomplete LFN sequence ending at entry ");
                fsck_put_dec(entry_idx);
                fsck_puts("\n");
                fsck_delete_entries_range(&it, lfn_start_entry, entry_idx - 1,
                                          dir_cluster);
                /* The 8.3 entry itself is valid — continue processing it */
            } else {
                /* Verify checksum */
                uint8_t computed = fsck_lfn_checksum(&re.raw[0]);
                if (computed != lfn_checksum_expected) {
                    stats->orphan_lfn_entries++;
                    fsck_puts("[FSCK] C5: LFN checksum mismatch vs 8.3 at entry ");
                    fsck_put_dec(entry_idx);
                    fsck_puts("\n");
                    fsck_delete_entries_range(&it, lfn_start_entry, entry_idx - 1,
                                              dir_cluster);
                }
            }
            building_lfn = false;
            lfn_seq_expected = 0;
        }

        /* C4: Validate attribute byte — only known bits should be set.
         * Check this BEFORE the volume-label skip so that attr=0xFF
         * (which has the VOL bit set) is caught as invalid rather than
         * silently skipped. */
        uint8_t valid_attr_mask = 0x3F;  /* RO|HID|SYS|VOL|DIR|ARC */
        if (attr & ~valid_attr_mask) {
            stats->invalid_entries++;
            fsck_puts("[FSCK] C4: invalid attr ");
            fsck_put_u32(attr);
            fsck_puts(" at entry ");
            fsck_put_dec(entry_idx);
            fsck_puts(" in cluster ");
            fsck_put_dec(dir_cluster);
            fsck_puts("\n");
            if (!fsck_delete_entry(&it)) {
                stats->repair_failures++;
                *need_readonly = true;
            }
            entry_idx++;
            continue;
        }

        /* Skip volume label (valid attr, but no cluster chain to check) */
        if (attr & 0x08) { entry_idx++; continue; }

        /* Extract first cluster */
        uint32_t fst_clus_hi, fst_clus_lo, file_size;
        fsck_entry_fields(&re, &fst_clus_hi, &fst_clus_lo, &file_size);
        uint32_t first_cluster = ((uint32_t)fst_clus_hi << 16) | fst_clus_lo;

        /* C4: Cluster 1 is reserved and must never appear in a dir entry */
        if (first_cluster == 1) {
            stats->invalid_entries++;
            fsck_puts("[FSCK] C4: cluster=1 (reserved) at entry ");
            fsck_put_dec(entry_idx);
            fsck_puts("\n");
            if (!fsck_delete_entry(&it)) {
                stats->repair_failures++;
                *need_readonly = true;
            }
            entry_idx++;
            continue;
        }

        /* C4: Cluster out of range */
        if (first_cluster >= 2 &&
            first_cluster >= vol_total_clusters + 2) {
            stats->invalid_entries++;
            fsck_puts("[FSCK] C4: cluster ");
            fsck_put_u32(first_cluster);
            fsck_puts(" out of range at entry ");
            fsck_put_dec(entry_idx);
            fsck_puts("\n");
            if (!fsck_delete_entry(&it)) {
                stats->repair_failures++;
                *need_readonly = true;
            }
            entry_idx++;
            continue;
        }

        /* Skip . and .. entries — their cluster pointers refer to the current
         * and parent directories which are already (or will be) marked
         * reachable.  Treating them as file entries would cause false
         * cross-link reports. */
        bool is_dot_entry = (re.raw[0] == '.' &&
                             (re.raw[1] == ' ' || re.raw[1] == '.'));
        if (is_dot_entry) {
            entry_idx++;
            continue;
        }

        /* Mark cluster chain as reachable; detect cross-links */
        if (first_cluster >= 2) {
            uint32_t xlink_before = stats->crosslink_repaired;
            fsck_mark_chain(reach_bmp, (void *)ref_table, first_cluster,
                            dir_cluster, entry_idx,
                            vol_total_clusters, stats, need_readonly);
            if (stats->crosslink_repaired > xlink_before) {
                /* A cross-link was detected for this entry.  Detach it from
                 * the shared chain by zeroing its cluster pointer and size. */
                if (!fsck_zero_entry_cluster(dir_cluster, entry_idx)) {
                    stats->repair_failures++;
                    *need_readonly = true;
                }
            }
        }

        /* Recurse into subdirectories (. and .. already skipped above) */
        if ((attr & 0x10) && first_cluster >= 2) {
            walk_dir(first_cluster, depth + 1,
                     reach_bmp, ref_table, vol_total_clusters,
                     stats, need_readonly);
        }

        entry_idx++;
    }
}

/* ── Public entry point ──────────────────────────────────────────────────── */

void chimerafs_fsck(void)
{
    fsck_stats_t stats;
    fsck_stats_init(&stats);

    bool need_readonly = false;

    /* Retrieve volume parameters from fat32.c */
    uint32_t vol_fat_lba, vol_fat_size, vol_num_fats;
    uint32_t vol_total_clusters, vol_root_cluster;
    fsck_get_vol_params(&vol_fat_lba, &vol_fat_size, &vol_num_fats,
                        &vol_total_clusters, &vol_root_cluster);

    fsck_puts("[FSCK] Starting ChimeraFS consistency check...\n");

    /* ── C1: FAT table mismatch ─────────────────────────────────────────── */
    check_fat_mismatch(vol_fat_lba, vol_fat_size, vol_num_fats, &stats);
    if (stats.fat_mismatch_sectors > 0) {
        fsck_puts("[FSCK] C1: ");
        fsck_put_dec(stats.fat_mismatch_sectors);
        fsck_puts(" FAT mismatch sector(s) repaired\n");
    }

    /* Allocate reachability bitmap (1 bit per cluster) */
    uint32_t bmp_bytes = (vol_total_clusters + 7) / 8;
    uint8_t *reach_bmp = (uint8_t *)kmalloc(bmp_bytes);
    if (!reach_bmp) {
        fsck_puts("[FSCK] Out of memory — skipping C2/C3/C4/C5\n");
        goto summary;
    }

    /* Allocate cross-link reference table (1 entry per cluster) */
    clus_ref_t *ref_table = (clus_ref_t *)kmalloc(
        vol_total_clusters * sizeof(clus_ref_t));
    if (!ref_table) {
        kfree(reach_bmp);
        fsck_puts("[FSCK] Out of memory — skipping C2/C3/C4/C5\n");
        goto summary;
    }

    /* Zero both arrays */
    for (uint32_t i = 0; i < bmp_bytes; i++) reach_bmp[i] = 0;
    for (uint32_t i = 0; i < vol_total_clusters; i++) {
        ref_table[i].claimed = false;
        ref_table[i].ref_cluster = 0;
        ref_table[i].ref_entry   = 0;
    }

    /* ── C2/C3/C4/C5: Walk the directory tree ───────────────────────────── */
    walk_dir(vol_root_cluster, 0, reach_bmp, ref_table,
             vol_total_clusters, &stats, &need_readonly);

    /* ── C2: Orphaned clusters ──────────────────────────────────────────── */
    {
        uint32_t lf_cluster = 0;
        uint32_t lf_seq     = 0;
        bool lf_created     = false;

        for (uint32_t c = 2; c < vol_total_clusters + 2; c++) {
            uint32_t fat_val = fsck_fat_get(c);
            if (fat_val == 0x00000000) continue;  /* FREE */
            if (fat_val == 0x0FFFFFF7) continue;  /* BAD */

            uint32_t idx = c - 2;
            if (bmp_get(reach_bmp, idx)) continue;  /* reachable */

            /* Orphaned cluster — only report the head of each chain */
            uint32_t prev_val = (c >= 3) ? fsck_fat_get(c - 1) : 0;
            bool is_chain_head = (prev_val == 0 || prev_val >= 0x0FFFFFF7 ||
                                  prev_val == 0x0FFFFFF8 ||
                                  !bmp_get(reach_bmp, (c - 1) - 2));
            if (!is_chain_head) continue;

            stats.orphan_clusters++;
            fsck_puts("[FSCK] C2: orphaned chain starting at cluster ");
            fsck_put_u32(c);
            fsck_puts("\n");

            /* Repair: move to /lost+found */
            if (!lf_created) {
                lf_cluster = ensure_lost_found();
                lf_created = true;
                /* Mark /lost+found's cluster chain as reachable so the
                 * orphan scan does not report it as an orphan itself. */
                if (lf_cluster) {
                    uint32_t lfc = lf_cluster;
                    uint32_t guard2 = 0;
                    while (lfc >= 2 && lfc < vol_total_clusters + 2 &&
                           guard2 < vol_total_clusters) {
                        bmp_set(reach_bmp, lfc - 2);
                        uint32_t nxt2 = fsck_fat_get(lfc);
                        if (nxt2 >= 0x0FFFFFF8 || nxt2 == 0) break;
                        lfc = nxt2;
                        guard2++;
                    }
                }
            }
            if (lf_cluster) {
                if (rescue_chain(c, lf_cluster, &lf_seq,
                                 0, 0, 0, 0, 0)) {
                    stats.orphan_clusters_rescued++;
                    /* Mark chain as reachable so we don't report sub-clusters */
                    uint32_t nc = c;
                    while (nc >= 2 && nc < vol_total_clusters + 2) {
                        bmp_set(reach_bmp, nc - 2);
                        uint32_t nxt = fsck_fat_get(nc);
                        if (nxt >= 0x0FFFFFF8 || nxt == 0) break;
                        nc = nxt;
                    }
                } else {
                    /* Could not rescue — free the chain */
                    fsck_fat_free_chain(c);
                    stats.repair_failures++;
                }
            } else {
                /* No /lost+found — free the chain */
                fsck_fat_free_chain(c);
            }
        }
    }

    kfree(ref_table);
    kfree(reach_bmp);

summary:
    /* ── Summary ────────────────────────────────────────────────────────── */
    {
        uint32_t total_errors =
            stats.fat_mismatch_sectors +
            stats.orphan_clusters +
            stats.crosslink_clusters +
            stats.invalid_entries +
            stats.orphan_lfn_entries;

        fsck_puts("[FSCK] ");
        fsck_put_dec(total_errors);
        fsck_puts(" error(s): FAT_mismatch=");
        fsck_put_dec(stats.fat_mismatch_sectors);
        fsck_puts(" orphan_clus=");
        fsck_put_dec(stats.orphan_clusters);
        fsck_puts(" crosslink=");
        fsck_put_dec(stats.crosslink_clusters);
        fsck_puts(" bad_entry=");
        fsck_put_dec(stats.invalid_entries);
        fsck_puts(" orphan_lfn=");
        fsck_put_dec(stats.orphan_lfn_entries);
        fsck_puts("\n");

        if (total_errors == 0) {
            fsck_puts("[FSCK] 0 errors — filesystem is clean\n");
        } else {
            fsck_puts("[FSCK] Repair summary: rescued=");
            fsck_put_dec(stats.orphan_clusters_rescued);
            fsck_puts(" failures=");
            fsck_put_dec(stats.repair_failures);
            fsck_puts("\n");
        }

        if (need_readonly || stats.repair_failures > 0) {
            fsck_puts("[FSCK] WARNING: filesystem marked read-only\n");
            fsck_set_readonly();
        }
    }
}
