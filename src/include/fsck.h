/*
 * ChimaeraOS - FAT32 Consistency Checker Interface
 * include/fsck.h
 *
 * This header declares:
 *   1. The public entry point chimerafs_fsck() called from fat32_mount().
 *   2. The fsck_stats_t result structure.
 *   3. Internal bridge functions implemented in fat32.c that fsck.c calls
 *      to access volume state and perform low-level I/O without duplicating
 *      the sector cache or FAT accessor logic.
 *
 * Design principle: fsck.c contains only the checker/repair logic.  All
 * knowledge of on-disk layout (cluster-to-LBA mapping, FAT entry format,
 * directory entry format) lives in fat32.c and is exposed here via a thin
 * bridge API.  This keeps the two files independently testable.
 */
#ifndef FSCK_H
#define FSCK_H

#include "types.h"

/* ── Result statistics ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t fat_mismatch_sectors;    /* C1: FAT1 vs FAT2 divergent sectors */
    uint32_t fat_mismatch_repaired;   /* C1: sectors successfully resynced */
    uint32_t orphan_clusters;         /* C2: orphaned chain heads found */
    uint32_t orphan_clusters_rescued; /* C2: chains moved to /lost+found */
    uint32_t crosslink_clusters;      /* C3: cross-linked cluster references */
    uint32_t crosslink_repaired;      /* C3: second references zeroed */
    uint32_t invalid_entries;         /* C4: bad dir entries deleted */
    uint32_t orphan_lfn_entries;      /* C5: orphaned LFN entries deleted */
    uint32_t repair_failures;         /* Any repair that could not be applied */
} fsck_stats_t;

static inline void fsck_stats_init(fsck_stats_t *s)
{
    s->fat_mismatch_sectors    = 0;
    s->fat_mismatch_repaired   = 0;
    s->orphan_clusters         = 0;
    s->orphan_clusters_rescued = 0;
    s->crosslink_clusters      = 0;
    s->crosslink_repaired      = 0;
    s->invalid_entries         = 0;
    s->orphan_lfn_entries      = 0;
    s->repair_failures         = 0;
}

/* ── Public entry point ──────────────────────────────────────────────────── */

/*
 * chimerafs_fsck() — run the full five-check consistency scan.
 *
 * Called from fat32_mount() after volume parameters are parsed and
 * vol_mounted is temporarily set to true (so FAT/dir helpers work).
 * Logs results to serial console.  Sets vol_readonly on unrecoverable errors.
 */
void chimerafs_fsck(void);

/* ── Bridge API: volume parameters ──────────────────────────────────────── */

/*
 * Retrieve the current volume parameters from fat32.c's static state.
 * All output pointers are required (non-NULL).
 */
void fsck_get_vol_params(uint32_t *out_fat_lba,
                         uint32_t *out_fat_size,
                         uint32_t *out_num_fats,
                         uint32_t *out_total_clusters,
                         uint32_t *out_root_cluster);

/* ── Bridge API: FAT access ──────────────────────────────────────────────── */

/* Read a FAT entry (masked to 28 bits).  Returns 0x0FFFFFF7 on I/O error. */
uint32_t fsck_fat_get(uint32_t cluster);

/* Free an entire cluster chain starting at `start`. */
void fsck_fat_free_chain(uint32_t start);

/* ── Bridge API: directory iteration ────────────────────────────────────── */

/*
 * Opaque directory iterator.  Wraps dir_pos_t and the sector cache so that
 * fsck.c does not need to know the on-disk layout of directory entries.
 */
typedef struct {
    uint32_t cluster;
    uint32_t entry_idx;
    uint32_t lba;
    uint32_t off;
} fsck_dir_iter_t;

/* Raw 32-byte directory entry buffer */
typedef struct {
    uint8_t raw[32];
} fsck_raw_entry_t;

/* Initialise an iterator to the start of a directory cluster chain. */
void fsck_dir_iter_init(fsck_dir_iter_t *it, uint32_t cluster);

/*
 * Read the current entry into `re` and advance to the next position.
 * Returns false when the directory is exhausted or an I/O error occurs.
 */
bool fsck_dir_iter_next(fsck_dir_iter_t *it, fsck_raw_entry_t *re);

/*
 * Mark the current entry (the one most recently returned by
 * fsck_dir_iter_next) as deleted by writing 0xE5 to its first byte.
 * Returns false on I/O error.
 */
bool fsck_delete_entry(fsck_dir_iter_t *it);

/*
 * Mark a range of entries [first_entry_idx .. last_entry_idx] in the
 * directory rooted at `dir_cluster` as deleted.  Used to wipe orphaned
 * LFN entry sequences.
 */
void fsck_delete_entries_range(fsck_dir_iter_t *it,
                               uint32_t first_entry_idx,
                               uint32_t last_entry_idx,
                               uint32_t dir_cluster);

/* ── Bridge API: directory entry field extraction ────────────────────────── */

/*
 * Extract fst_clus_hi, fst_clus_lo, and file_size from a raw 8.3 entry.
 * The caller must ensure `re` is an 8.3 entry (attr != 0x0F).
 */
void fsck_entry_fields(const fsck_raw_entry_t *re,
                       uint32_t *out_fst_clus_hi,
                       uint32_t *out_fst_clus_lo,
                       uint32_t *out_file_size);

/* ── Bridge API: cluster chain reachability ──────────────────────────────── */

/*
 * Walk the cluster chain starting at `start`, marking each cluster as
 * reachable in `reach_bmp`.  Detects cross-links using `ref_table`.
 *
 * `dir_cluster` and `entry_idx` identify the directory entry that owns
 * this chain (used for cross-link reporting).
 */

void fsck_mark_chain(uint8_t *reach_bmp,
                     void *ref_table,
                     uint32_t start,
                     uint32_t dir_cluster,
                     uint32_t entry_idx,
                     uint32_t vol_total_clusters,
                     fsck_stats_t *stats,
                     bool *need_readonly);

/* ── Bridge API: LFN checksum ────────────────────────────────────────────── */

/*
 * Compute the 8.3 checksum from the first 11 bytes of a raw 8.3 dir entry.
 * Matches the algorithm in fat32.c's lfn_checksum().
 */
uint8_t fsck_lfn_checksum(const uint8_t name83[11]);

/* ── Bridge API: /lost+found helpers ─────────────────────────────────────── */

/*
 * Create a directory entry at `path` whose cluster chain is the existing
 * chain starting at `first_cluster`.  Does NOT allocate a new cluster.
 * Returns true on success.
 */
bool fsck_adopt_chain(const char *path, uint32_t first_cluster);

/*
 * Zero the cluster pointer (fst_clus_hi, fst_clus_lo) and file_size in the
 * entry_idx-th directory entry of dir_cluster.  Used to repair cross-linked
 * files by detaching the second reference from the shared cluster chain.
 * Returns true on success.
 */
bool fsck_zero_entry_cluster(uint32_t dir_cluster, uint32_t entry_idx);

/* ── Bridge API: read-only flag ──────────────────────────────────────────── */

/* Set the volume read-only flag.  Subsequent fat32_open(write=true) → error. */
void fsck_set_readonly(void);

#endif /* FSCK_H */
