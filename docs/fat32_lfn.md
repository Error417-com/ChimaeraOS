# FAT32 Long Filename (LFN) Support Audit & Fixes

**Author:** Manus AI
**Date:** May 15, 2026

## Executive Summary

An audit of the ChimeraOS FAT32 Long Filename (LFN) implementation was conducted to verify compliance with the Microsoft FAT32 specification. The audit identified four critical bugs related to LFN entry creation, short-name fallback, deletion handling, and short-name collision generation. All issues have been successfully resolved, and a comprehensive Python test harness (`tests/fs/test_lfn.py`) has been added to the CI suite to prevent regressions.

## Supported vs. Unsupported Character Ranges

The FAT32 LFN specification uses UTF-16LE encoding for filenames. The current ChimeraOS implementation provides the following support:

| Character Range | Support Status | Notes |
| :--- | :--- | :--- |
| **ASCII (U+0020 to U+007E)** | Fully Supported | Standard English characters, numbers, and symbols. |
| **Latin-1 Supplement (U+00A0 to U+00FF)** | Partially Supported | Characters like `é` and `ü` are preserved on disk in UTF-16LE format. However, the kernel's internal string handling and `f_toupper` functions are currently ASCII-only, meaning case-insensitive lookups for non-ASCII characters may fail. |
| **BMP (U+0100 to U+FFFF)** | Partially Supported | Characters like `中` are preserved on disk, but case-insensitive lookups are not supported. |
| **Supplementary Planes (Emoji, etc.)** | Unsupported | Surrogate pairs (requiring 4 bytes per character) are not correctly handled by the current string length and comparison functions. |

## Identified Bugs and Root Causes

### Bug A: `fst_clus_lo` Corruption in LFN Entries
**Symptom:** `fsck.fat` reported non-zero `fst_clus_lo` fields in LFN entries, which the FAT32 specification requires to be exactly `0x0000`.
**Root Cause:** The `fat32_close` function performs an atomic commit by writing the file size and starting cluster to the 8.3 directory entry. It computed the sector LBA using `cluster_to_lba(dir_cluster) + dir_offset / SECTOR_SIZE`. However, `dir_offset` was the byte offset *within the sector* (0–511), not within the cluster. This caused the division to always evaluate to 0, meaning `fat32_close` always wrote to the *first sector* of the directory cluster. If the 8.3 entry was in the second sector, `fat32_close` would overwrite LFN entries in the first sector with cluster/size data, corrupting the `fst_clus_lo` and `name2` fields.
**Fix:** The `fat32_fd_t` struct was updated to store the exact `dir_lba` (sector LBA) during `fat32_open`, which is then used directly by `fat32_close`.

### Bug B: Incorrect 8.3 Fallback Match
**Symptom:** `fat32_unlink("/abcdefghijklmn.txt")` would accidentally delete `"/abcdefghijklm.txt"`.
**Root Cause:** When `dir_find` searched for a file, if an LFN entry was present but did not match the target name, the function would incorrectly fall through and compare the target name against the 8.3 short name. Since both files generated the same 8.3 name (`ABCDEFGHTXT`), the lookup would match the wrong file.
**Fix:** `dir_find` was updated to skip the 8.3 comparison if a valid LFN was present for the entry.

### Bug C: Orphaned LFN Entries on Deletion
**Symptom:** Deleting a file with an LFN left the LFN entries on disk, causing them to be associated with the next file created in that directory slot.
**Root Cause:** `fat32_unlink` only marked the 8.3 entry as deleted (`0xE5`), leaving the preceding LFN entries intact.
**Fix:** `dir_find` was modified to return the starting position of the LFN entries (`out_lfn_start`). `fat32_unlink` now iterates from `lfn_start` to the 8.3 entry, marking all associated entries as deleted.

### Bug D: Missing Short-Name Collision Generation
**Symptom:** Creating multiple files with similar long names (e.g., `abcdefghijklm.txt` and `abcdefghijklmn.txt`) resulted in duplicate 8.3 entries (`ABCDEFGHTXT`), violating the FAT32 specification.
**Root Cause:** The `make_83` function did not implement `~1`, `~2` collision generation.
**Fix:** A new `make_83_unique` function was implemented. It checks if the generated 8.3 name already exists in the directory. If a collision is detected, it truncates the stem and appends `~1` through `~9999` until a unique name is found.

## Verification

A comprehensive Python test harness (`tests/fs/test_lfn.py`) was developed to automate verification. The harness boots ChimeraOS in QEMU, runs 12 specific LFN test cases, and then verifies the resulting disk image using `fsck.fat`.

All 12 tests now pass successfully:
1. 13-char boundary name create+stat
2. 14-char name (2 LFN entries)
3. 26-char name (2 LFN entries boundary)
4. 100-char name
5. 250-char name (near max)
6. Latin-1 non-ASCII byte roundtrip
7. Deletion+recreation (orphan LFN entries)
8. Orphan LFN not matched to subsequent 8.3 entry
9. Delete+recreate same LFN name
10. Case-insensitive LFN lookup
11. Short-name collision: both files findable
12. LFN entry readable by mtools (host verification)

The `fsck.fat` utility confirms that the filesystem is completely clean, with no duplicate entries or `fst_clus_lo` violations.
