# ChimeraFS Write-Ahead Log (WAL) Design Specification

**Author:** Manus AI
**Date:** May 16, 2026

## 1. Introduction

ChimeraFS currently relies on an ad-hoc, boot-time `fat32_fsck_repair()` routine to recover from power failures. This routine scans the entire directory tree to reclaim orphaned clusters and repairs the FAT2 mirror. While functional, this approach is $O(N)$ with respect to the number of allocated clusters and directory entries, leading to unbounded boot times as the filesystem grows.

This document specifies the design for a Write-Ahead Log (WAL) for ChimeraFS. The WAL will provide atomic metadata transactions, eliminating the need for full-volume scans on boot and ensuring strict crash consistency.

## 2. Journal Format and Location

### 2.1. Reserved File Approach
To maintain backward compatibility with standard FAT32 drivers (e.g., `fsck.fat`, Windows, Linux), the journal will be stored as a standard, pre-allocated hidden file in the root directory named `CHIMERA.JNL`.

- **Size:** Fixed at 4 MB (8192 sectors).
- **Attributes:** Hidden, System, Read-Only (`0x02 | 0x04 | 0x01`).
- **Allocation:** Contiguous cluster chain allocated at format time or during migration.

### 2.2. Ring Buffer Layout
The 4 MB file operates as a circular ring buffer of 512-byte sectors.

| Offset | Size | Description |
| :--- | :--- | :--- |
| `0x0000` | 512 bytes | **Superblock:** Contains head/tail pointers and sequence numbers. |
| `0x0200` | 4,193,792 bytes | **Log Area:** Array of 8191 log blocks. |

### 2.3. Superblock Structure
The superblock is always located at sector 0 of `CHIMERA.JNL`.

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 0x4A4D4943 ("CHMJ") */
    uint32_t version;       /* 1 */
    uint32_t head_blk;      /* Next block to write (1 to 8191) */
    uint32_t tail_blk;      /* Oldest uncommitted block (1 to 8191) */
    uint64_t sequence;      /* Monotonically increasing transaction ID */
    uint32_t checksum;      /* CRC32 of the superblock */
    uint8_t  reserved[488];
} wal_super_t;
```

### 2.4. Log Block Structure
Each transaction consists of a Descriptor Block, one or more Data Blocks, and a Commit Block.

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 0x424C4B44 ("DKLB") for desc, 0x434D4954 ("TIMC") for commit */
    uint64_t sequence;      /* Must match superblock sequence */
    uint32_t num_sectors;   /* Number of data sectors following this descriptor */
    uint32_t target_lbas[125]; /* Target LBAs for the following data sectors */
    uint32_t checksum;      /* CRC32 of this block */
} wal_desc_t;
```

## 3. Journaled Operations

### 3.1. Metadata-Only Journaling
To minimize write amplification and preserve flash memory lifespan, ChimeraFS will implement **metadata-only journaling**.

The following sectors will be routed through the WAL:
1. **FAT Sectors:** Any modifications to FAT1 (FAT2 mirroring will be handled asynchronously or dropped entirely in favor of the WAL).
2. **Directory Sectors:** Any modifications to directory entries (creation, deletion, renaming, size updates).
3. **FSInfo Sector:** Updates to the free cluster count.

### 3.2. Data Sectors
File data sectors will **not** be journaled. Instead, ChimeraFS will enforce an ordering constraint:
1. Write data sectors to their final LBA locations.
2. Wait for ATA flush.
3. Write the metadata transaction (FAT updates + directory entry updates) to the WAL.
4. Wait for ATA flush.
5. Write the commit block to the WAL.
6. Asynchronously flush the metadata to its final LBA locations.

This ensures that if a crash occurs, the metadata either points to fully written data, or the metadata update is rolled back, leaving the data sectors as unreferenced garbage (which is safe).

## 4. Replay Logic on Boot

Upon mounting the FAT32 volume, ChimeraFS will execute the following replay sequence before allowing any standard filesystem operations:

1. **Locate Journal:** Search the root directory for `CHIMERA.JNL`. If not found, fall back to the legacy `fat32_fsck_repair()` routine.
2. **Read Superblock:** Read sector 0 of the journal. Verify the `magic` and `checksum`. If invalid, assume the journal is corrupt and fall back to legacy repair.
3. **Check State:** If `head_blk == tail_blk`, the journal is clean. No replay is necessary.
4. **Replay Loop:**
   - Start reading at `tail_blk`.
   - Read the `wal_desc_t`. Verify `magic` and `sequence`.
   - If valid, read the next `num_sectors` data blocks from the journal.
   - Read the `wal_commit_t` block. Verify `magic` and `sequence`.
   - If the commit block is valid, write the data blocks to their respective `target_lbas`.
   - Advance `tail_blk` past the commit block.
   - Repeat until an invalid block is encountered or `tail_blk == head_blk`.
5. **Clean Up:** Update the superblock to set `tail_blk = head_blk` and flush it to disk.

## 5. Performance Impact Estimate

### 5.1. Write Amplification
Metadata-only journaling introduces a write amplification factor for metadata operations. A typical file creation (allocating 1 cluster, writing 1 directory entry) currently requires:
- 1 read + 1 write for FAT1
- 1 read + 1 write for FAT2
- 1 read + 1 write for the directory sector
- Total: 3 writes.

With the WAL, the same operation requires:
- 1 write for Descriptor Block
- 2 writes for Data Blocks (FAT1 + Dir)
- 1 write for Commit Block
- 2 writes for final metadata flush (FAT1 + Dir)
- Total: 6 writes.

While the number of writes doubles, the WAL writes are strictly sequential, which is highly optimized by modern flash controllers and the ATA driver.

### 5.2. Boot Time
Current boot time scales linearly with the number of allocated clusters due to the `fat32_fsck_repair()` scan. On a 64 MB disk, this takes approximately 50-100ms. On a 32 GB disk, this could take several seconds.

With the WAL, boot time is bounded by the size of the uncommitted log. Replaying a full 4 MB log takes less than 10ms on standard ATA hardware. Boot time becomes $O(1)$ with respect to filesystem size.

## 6. Migration Path for Existing Filesystems

To ensure a seamless transition for existing ChimeraOS installations, the migration will occur automatically during the first boot with the new kernel.

1. **Mount & Legacy Repair:** The kernel mounts the FAT32 volume and runs the legacy `fat32_fsck_repair()` to ensure the filesystem is in a perfectly consistent state.
2. **Allocate Journal:** The kernel calls `fat32_write_file("/CHIMERA.JNL", zero_buf, 4 * 1024 * 1024)` to allocate the 4 MB contiguous file.
3. **Set Attributes:** The kernel modifies the directory entry for `CHIMERA.JNL` to set the Hidden, System, and Read-Only attributes.
4. **Initialize Superblock:** The kernel writes the initial `wal_super_t` to sector 0 of the file.
5. **Enable WAL:** The kernel sets an internal `wal_enabled = true` flag. All subsequent metadata operations are routed through the WAL.

If the disk is completely full and the 4 MB allocation fails, the kernel will log a warning and continue operating in legacy mode (synchronous metadata writes + boot-time repair).
