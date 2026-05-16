/*
 * ChimaeraOS - FAT32 Driver — Public Operations
 * fs/fat32_ops.c
 *
 * Directory entry creation (dir_add_entry) and all public file and
 * directory operations: open, read, write, seek, close, filesize,
 * mkdir, readdir, stat, unlink, rename, copy, read_file, write_file,
 * append_file, strerror, and print_stats.
 *
 * Split from the original monolithic fat32.c.
 * See fat32_internal.h for shared types and declarations.
 */
#include "fat32_internal.h"

/* ── Directory entry creation ────────────────────────────────────────────── */

/*
 * Add a directory entry (with optional LFN) to a directory cluster.
 * Returns true on success and fills `out_pos` with the 8.3 entry position.
 */
bool dir_add_entry(uint32_t dir_cluster, const char *name,
                   uint8_t attr, uint32_t first_cluster,
                   uint32_t file_size, dir_pos_t *out_pos)
{
    char name83[11];
    make_83_unique(dir_cluster, name, name83);
    uint8_t checksum = lfn_checksum(name83);

    uint32_t nlen = f_strlen(name);
    int lfn_count = (int)((nlen + 12) / 13);
    int need = lfn_count + 1;

    dir_pos_t pos;
    dir_pos_init(&pos, dir_cluster);
    int free_run = 0;
    dir_pos_t run_start = pos;

    for (;;) {
        fat32_dir_entry_t e;
        if (!dir_read_entry(&pos, &e)) {
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
            uint32_t last_clus = pos.cluster;
            uint32_t new_clus = fat_alloc(last_clus);
            if (!new_clus) return false;
            dir_pos_init(&pos, new_clus);
            if (free_run == 0) run_start = pos;
            free_run += (int)(vol_sec_per_clus * ENTRIES_PER_SEC);
            goto found_space;
        }
    }
    return false;

found_space:
    pos = run_start;
    for (int i = lfn_count; i >= 1; i--) {
        fat32_lfn_entry_t lfn;
        f_memset(&lfn, 0, sizeof(lfn));
        lfn.order    = (uint8_t)i | (i == lfn_count ? 0x40 : 0);
        lfn.attr     = FAT32_ATTR_LFN;
        lfn.checksum = checksum;
        lfn.fst_clus_lo = 0;

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

    fat32_dir_entry_t e83;
    f_memset(&e83, 0, sizeof(e83));
    f_memcpy(e83.name, name83, 11);
    e83.attr        = attr;
    e83.fst_clus_hi = (uint16_t)(first_cluster >> 16);
    e83.fst_clus_lo = (uint16_t)(first_cluster & 0xFFFF);
    e83.file_size   = file_size;
    e83.wrt_date    = 0x5B21;
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
            if (cluster >= 2) fat_free_chain(cluster);
            uint32_t new_clus = fat_alloc(0);
            if (!new_clus) return FAT32_ERR_FULL;
            fds[fd].cluster     = new_clus;
            fds[fd].cur_cluster = new_clus;
            fds[fd].size        = 0;
            fds[fd].pos         = 0;
            e.fst_clus_hi = (uint16_t)(new_clus >> 16);
            e.fst_clus_lo = (uint16_t)(new_clus & 0xFFFF);
            e.file_size   = 0;
            dir_write_entry(&pos, &e);
        }
    } else {
        if (!write) return FAT32_ERR_NOTFOUND;

        char name[FAT32_MAX_NAME];
        uint32_t parent = path_resolve_parent(path, name);
        if (!parent) return FAT32_ERR_NOTFOUND;

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

    if (f->write) {
        uint32_t dir_lba = f->dir_lba;
        uint32_t dir_off = f->dir_offset;
        if (sec_read(dir_lba)) {
            fat32_dir_entry_t *e = (fat32_dir_entry_t *)(sec_cache + dir_off);
            e->file_size   = f->size;
            e->fst_clus_hi = (uint16_t)(f->cluster >> 16);
            e->fst_clus_lo = (uint16_t)(f->cluster & 0xFFFF);
            sec_cache_dirty = true;
            sec_flush();
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

    fat32_dir_entry_t e;
    if (path_stat(path, &e, NULL, NULL)) return FAT32_ERR_EXISTS;

    char name[FAT32_MAX_NAME];
    uint32_t parent = path_resolve_parent(path, name);
    if (!parent || name[0] == '\0') return FAT32_ERR_NOTFOUND;

    uint32_t new_clus = fat_alloc(0);
    if (!new_clus) return FAT32_ERR_FULL;

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
    uint32_t parent_clus = (parent == vol_root_cluster) ? 0 : parent;
    dotdot.fst_clus_hi = (uint16_t)(parent_clus >> 16);
    dotdot.fst_clus_lo = (uint16_t)(parent_clus & 0xFFFF);

    uint32_t lba = cluster_to_lba(new_clus);
    if (!sec_read(lba)) { fat_free_chain(new_clus); return FAT32_ERR_IO; }
    f_memcpy(sec_cache, &dot, DIR_ENTRY_SIZE);
    f_memcpy(sec_cache + DIR_ENTRY_SIZE, &dotdot, DIR_ENTRY_SIZE);
    sec_cache_dirty = true;
    sec_flush();

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

    char name[FAT32_MAX_NAME];
    uint32_t dir_cluster = path_resolve_parent(path, name);
    if (!dir_cluster || name[0] == '\0') return FAT32_ERR_NOTFOUND;
    if (!dir_find(dir_cluster, name, &e, &pos, &lfn_start))
        return FAT32_ERR_NOTFOUND;
    if (e.attr & FAT32_ATTR_DIR) return FAT32_ERR_ISDIR;

    uint32_t cluster = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;
    if (cluster >= 2) fat_free_chain(cluster);

    /*
     * FIX (Bug C) — Mark ALL directory entries for this file as deleted.
     */
    dir_pos_t del = lfn_start;
    for (;;) {
        if (!sec_read(del.lba)) break;
        sec_cache[del.off] = 0xE5;
        sec_cache_dirty = true;
        sec_flush();
        if (del.lba == pos.lba && del.off == pos.off) break;
        if (!dir_next(&del)) break;
    }

    return FAT32_OK;
}

fat32_err_t fat32_rename(const char *src, const char *dst)
{
    if (!vol_mounted) return FAT32_ERR_NODEV;
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
    int32_t sz = fat32_filesize(path);
    if (sz < 0) {
        return fat32_write_file(path, data, len);
    }

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

void fat32_print_stats(void)
{
    if (!vol_mounted) { vga_puts("/persist: not mounted\n"); return; }
    vga_puts("/persist: FAT32, ");
    uint32_t free_clus = 0;
    for (uint32_t c = 2; c < vol_total_clusters + 2; c++) {
        if (fat_get(c) == FAT32_FREE) free_clus++;
    }
    uint32_t free_mb = (free_clus * vol_sec_per_clus * SECTOR_SIZE) / (1024*1024);
    uint32_t total_mb= (vol_total_clusters * vol_sec_per_clus * SECTOR_SIZE) / (1024*1024);
    char buf[32];
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
