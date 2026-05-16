/*
 * ChimaeraOS - FSCK Test Kernel
 * kernel/kernel.c
 *
 * Minimal kernel that:
 *   1. Initialises serial, VGA, ATA, memory, and the panic symbol table.
 *   2. Mounts the FAT32 filesystem (which calls chimerafs_fsck() internally).
 *   3. Under FSCK_TEST mode, runs additional deliberate-corruption tests.
 *   4. Emits [FSCKTEST] PASS/FAIL/DONE markers on the serial console.
 *   5. Halts.
 */
#include "../include/serial.h"
#include "../include/vga.h"
#include "../include/ata.h"
#include "../include/mm.h"
#include "../include/fat32.h"
#include "../include/fsck.h"
#include "../include/types.h"
#include "../include/panic.h"
#include "../include/timer.h"
#include "../include/metrics.h"

/* ── Test helpers ─────────────────────────────────────────────────────────── */

static int tests_passed = 0;
static int tests_failed = 0;

static void test_pass(const char *id, const char *desc)
{
    serial_puts("[FSCKTEST] PASS ");
    serial_puts(id);
    serial_puts(" ");
    serial_puts(desc);
    serial_puts("\n");
    tests_passed++;
}

static void test_fail(const char *id, const char *desc)
{
    serial_puts("[FSCKTEST] FAIL ");
    serial_puts(id);
    serial_puts(" ");
    serial_puts(desc);
    serial_puts("\n");
    tests_failed++;
}

/* Check that a file exists and has the expected size */
static bool check_file(const char *path, int32_t expected_size)
{
    int32_t sz = fat32_filesize(path);
    if (expected_size < 0) return (sz >= 0);  /* just check it exists */
    return (sz == expected_size);
}

/* ── FSCK_TEST mode ───────────────────────────────────────────────────────── */

#ifdef FSCK_TEST

/*
 * The FSCK_TEST kernel receives a pre-corrupted disk image.  The corruption
 * was injected by the test harness (test_fsck.py) before booting.  We simply
 * mount the filesystem (which triggers chimerafs_fsck() automatically) and
 * then verify that the expected repairs were applied.
 *
 * The test harness injects specific corruptions and passes the corruption type
 * via a 1-byte file /CORRUPT_TYPE on the disk.
 */

static char read_corrupt_type(void)
{
    int fd = fat32_open("/CORRUPT_TYPE", false);
    if (fd < 0) return '?';
    char c = '?';
    fat32_read(fd, &c, 1);
    fat32_close(fd);
    return c;
}

void kernel_main(void)
{
    serial_init();
    vga_init();
    mm_init();
    symtab_init();
    timer_init();
    metrics_init();

    serial_puts("[FSCKTEST] ChimeraOS FSCK test kernel starting\n");

    if (!ata_init())
        panic("FSCKTEST: No ATA disk detected");

    /* fat32_mount() calls chimerafs_fsck() internally */
    bool mounted = fat32_mount();
    if (!mounted)
        panic("FSCKTEST: fat32_mount() failed");

    serial_puts("[FSCKTEST] Filesystem mounted\n");

    char corrupt_type = read_corrupt_type();
    serial_puts("[FSCKTEST] Corruption type: ");
    serial_putchar(corrupt_type);
    serial_puts("\n");

    switch (corrupt_type) {

    case '1':
        if (check_file("/testfile.txt", -1))
            test_pass("C1", "FAT mismatch repaired: testfile.txt readable");
        else
            test_fail("C1", "FAT mismatch: testfile.txt not found after repair");
        break;

    case '2':
        {
            fat32_dirent_t de;
            fat32_err_t err = fat32_stat("/lost+found", &de);
            if (err == FAT32_OK)
                test_pass("C2a", "Orphaned cluster: /lost+found exists");
            else
                test_fail("C2a", "Orphaned cluster: /lost+found missing");

            int count = fat32_readdir("/lost+found", NULL, NULL);
            if (count >= 1)
                test_pass("C2b", "Orphaned cluster: rescued file in /lost+found");
            else
                test_fail("C2b", "Orphaned cluster: no rescued file in /lost+found");
        }
        break;

    case '3':
        if (check_file("/file_a.txt", -1))
            test_pass("C3a", "Cross-link: file_a.txt still exists");
        else
            test_fail("C3a", "Cross-link: file_a.txt missing");
        if (check_file("/file_b.txt", -1))
            test_pass("C3b", "Cross-link: file_b.txt still exists");
        else
            test_fail("C3b", "Cross-link: file_b.txt missing");
        break;

    case '4':
        if (check_file("/goodfile.txt", -1))
            test_pass("C4", "Invalid entry: goodfile.txt readable after repair");
        else
            test_fail("C4", "Invalid entry: goodfile.txt not found after repair");
        break;

    case '5':
        if (check_file("/normal.txt", -1))
            test_pass("C5", "Orphaned LFN: normal.txt readable after repair");
        else
            test_fail("C5", "Orphaned LFN: normal.txt not found after repair");
        break;

    default:
        if (mounted)
            test_pass("C0", "Clean disk: filesystem mounted successfully");
        else
            test_fail("C0", "Clean disk: mount failed");
        break;
    }

    /* Summary */
    serial_puts("[FSCKTEST] Tests: passed=");
    {
        char buf[8];
        int n = tests_passed, i = 0;
        if (!n) buf[i++] = '0';
        else { while(n){buf[i++]='0'+n%10;n/=10;} }
        for (int j=i-1;j>=0;j--) serial_putchar(buf[j]);
    }
    serial_puts(" failed=");
    {
        char buf[8];
        int n = tests_failed, i = 0;
        if (!n) buf[i++] = '0';
        else { while(n){buf[i++]='0'+n%10;n/=10;} }
        for (int j=i-1;j>=0;j--) serial_putchar(buf[j]);
    }
    serial_puts("\n");
    serial_puts("[FSCKTEST] DONE\n");

    for (;;) __asm__("hlt");
}

#else  /* Normal kernel (non-test) */

void kernel_main(void)
{
    serial_init();
    vga_init();
    mm_init();
    symtab_init();
    timer_init();
    metrics_init();

    serial_puts("[KERNEL] ChimeraOS starting\n");

    if (!ata_init())
        panic("No ATA disk detected");

    if (!fat32_mount())
        panic("FAT32 mount failed");

    vga_puts("ChimeraOS ready.\n");
    for (;;) __asm__("hlt");
}

#endif /* FSCK_TEST */
