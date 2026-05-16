/*
 * ChimaeraOS — ACPI Subsystem
 * src/kernel/acpi.c
 *
 * Implements ACPI table discovery and power management for ChimaeraOS.
 *
 * Discovery sequence
 * ------------------
 *   1. Scan the BIOS Extended BIOS Data Area (EBDA) pointer at 0x040E, then
 *      scan the BIOS ROM area 0xE0000–0xFFFFF for the 8-byte signature
 *      "RSD PTR " (note trailing space).  Each candidate is verified by
 *      summing all bytes of the v1 RSDP structure; the sum must be 0 mod 256.
 *
 *   2. From the RSDP, read the RSDT physical address (always 32-bit).
 *      If RSDP revision >= 2 and the XSDT address is non-zero, prefer XSDT
 *      (64-bit pointers to child tables).  In a 32-bit kernel we only use
 *      the low 32 bits of each XSDT entry.
 *
 *   3. Walk the RSDT/XSDT entry array looking for a table whose 4-byte
 *      signature is "FACP" (the FADT).  Verify the FADT checksum.
 *
 *   4. Extract from the FADT:
 *        SMI_CMD       (offset 48, 32-bit I/O port)
 *        ACPI_ENABLE   (offset 52, 8-bit value to write to SMI_CMD)
 *        PM1a_CNT_BLK  (offset 64, 32-bit I/O port)
 *        PM1b_CNT_BLK  (offset 68, 32-bit I/O port, may be 0)
 *        PM1_CNT_LEN   (offset 89, 8-bit, usually 2)
 *        SLP_TYPa      extracted from the \_S5_ DSDT object (see below)
 *
 * SLP_TYPa extraction
 * -------------------
 *   The SLP_TYPa value for S5 (soft-off) is encoded in the DSDT as an AML
 *   package named \_S5_.  Full AML parsing is out of scope; instead we scan
 *   the raw DSDT bytes for the 4-byte sequence {0x08, '_', 'S', '5', '_'}
 *   (DefName opcode + name) followed by the Package opcode (0x12) and the
 *   two ByteData values for SLP_TYPa and SLP_TYPb.
 *
 *   QEMU's DSDT always contains this pattern.  If the scan fails we default
 *   to SLP_TYPa = 0, which is correct for QEMU's S5 state.
 *
 * Shutdown
 * --------
 *   outw(PM1a_CNT_BLK, (SLP_TYPa << 10) | SLP_EN)
 *   If PM1b_CNT_BLK != 0: outw(PM1b_CNT_BLK, (SLP_TYPb << 10) | SLP_EN)
 *   Fallback: QEMU ISA debug exit (port 0x501, value 0x31 → exit code 0).
 *   Last resort: triple fault.
 *
 * Reboot
 * ------
 *   If FADT revision >= 2 and RESET_REG is an I/O port: outb(reset_port, reset_val).
 *   Fallback: keyboard controller pulse (outb(0x64, 0xFE)).
 *   Last resort: triple fault.
 */

#include "../include/acpi.h"
#include "../include/serial.h"
#include "../include/types.h"

/* ── Port I/O helpers ────────────────────────────────────────────────────── */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void io_wait(void)
{
    outb(0x80, 0x00);
}

/* ── ACPI table structure definitions ────────────────────────────────────── */

/*
 * All ACPI structures are packed and use little-endian byte order (x86 native).
 *
 * Offsets are per ACPI Specification 6.5, section 5.
 */

/* RSDP (Root System Description Pointer) — v1 is 20 bytes, v2 is 36 bytes */
typedef struct __attribute__((packed)) {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;       /* sum of first 20 bytes must be 0 mod 256 */
    char     oem_id[6];
    uint8_t  revision;       /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_address;   /* physical address of RSDT */
    /* v2 extension (revision >= 2): */
    uint32_t length;
    uint64_t xsdt_address;   /* physical address of XSDT (64-bit) */
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} rsdp_t;

/* Standard ACPI System Description Table header (SDT header) */
typedef struct __attribute__((packed)) {
    char     signature[4];   /* e.g. "RSDT", "FACP", "DSDT" */
    uint32_t length;         /* total table length in bytes */
    uint8_t  revision;
    uint8_t  checksum;       /* sum of all bytes must be 0 mod 256 */
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} sdt_header_t;

/*
 * FADT (Fixed ACPI Description Table) — "FACP"
 *
 * We only need a handful of fields; we access them by offset rather than
 * defining the full 276-byte structure to keep the code simple.
 *
 * Key field offsets (ACPI spec 5.2.9):
 *   36   FIRMWARE_CTRL   uint32  physical address of FACS
 *   40   DSDT            uint32  physical address of DSDT
 *   48   SMI_CMD         uint32  I/O port for SMI command
 *   52   ACPI_ENABLE     uint8   value to write to SMI_CMD to enable ACPI
 *   53   ACPI_DISABLE    uint8   value to write to SMI_CMD to disable ACPI
 *   64   PM1a_CNT_BLK    uint32  I/O port for PM1a control register
 *   68   PM1b_CNT_BLK    uint32  I/O port for PM1b control register (may be 0)
 *   89   PM1_CNT_LEN     uint8   byte length of PM1 control register (usually 2)
 *  116   FLAGS           uint32  feature flags
 *  116+  RESET_REG       GAS (12 bytes) — generic address structure for reset
 *  128   RESET_VALUE     uint8   value to write to RESET_REG
 *
 * GAS (Generic Address Structure, 12 bytes):
 *   0  address_space_id  uint8  0=system memory, 1=I/O space
 *   1  register_bit_width uint8
 *   2  register_bit_offset uint8
 *   3  access_size        uint8
 *   4  address            uint64
 */

#define FADT_OFF_DSDT           40
#define FADT_OFF_SMI_CMD        48
#define FADT_OFF_ACPI_ENABLE    52
#define FADT_OFF_PM1a_CNT_BLK  64
#define FADT_OFF_PM1b_CNT_BLK  68
#define FADT_OFF_PM1_CNT_LEN   89
#define FADT_OFF_FLAGS         112
#define FADT_OFF_RESET_REG     116   /* GAS, 12 bytes */
#define FADT_OFF_RESET_VALUE   128

/* GAS address_space_id values */
#define GAS_SYSTEM_MEMORY  0
#define GAS_SYSTEM_IO      1

/* FADT FLAGS bit: HW_REDUCED_ACPI (bit 20) — no PM1 registers if set */
#define FADT_FLAG_HW_REDUCED  (1u << 20)
/* FADT FLAGS bit: RESET_REG_SUP (bit 10) — reset register is supported */
#define FADT_FLAG_RESET_REG_SUP (1u << 10)

/* SLP_EN bit in PM1_CNT register */
#define SLP_EN  0x2000u

/* ── Module state ────────────────────────────────────────────────────────── */

static bool     g_acpi_ok        = false;
static uint32_t g_pm1a_cnt       = 0;    /* I/O port */
static uint32_t g_pm1b_cnt       = 0;    /* I/O port, 0 if absent */
static uint16_t g_slp_typa       = 0;    /* SLP_TYPa for S5 (bits 13:10) */
static uint16_t g_slp_typb       = 0;    /* SLP_TYPb for S5 */
static uint8_t  g_pm1_cnt_len    = 2;    /* byte width of PM1 CNT register */
static bool     g_reset_reg_ok   = false;
static uint16_t g_reset_port     = 0;
static uint8_t  g_reset_value    = 0;

/* ── Checksum helper ─────────────────────────────────────────────────────── */

static uint8_t acpi_checksum(const uint8_t *buf, uint32_t len)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += buf[i];
    return sum;
}

/* ── Memory comparison helper ────────────────────────────────────────────── */

static bool mem4eq(const char *a, const char *b)
{
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

static bool mem8eq(const char *a, const char *b)
{
    return mem4eq(a,b) && mem4eq(a+4,b+4);
}

/* ── Read a uint32_t from a byte array at a given offset ─────────────────── */

static uint32_t read_u32(const uint8_t *base, uint32_t off)
{
    return (uint32_t)base[off]
         | ((uint32_t)base[off+1] << 8)
         | ((uint32_t)base[off+2] << 16)
         | ((uint32_t)base[off+3] << 24);
}

static uint64_t read_u64(const uint8_t *base, uint32_t off)
{
    uint64_t lo = read_u32(base, off);
    uint64_t hi = read_u32(base, off + 4);
    return lo | (hi << 32);
}

/* ── RSDP scan ───────────────────────────────────────────────────────────── */

/*
 * Scan a memory range (16-byte aligned) for the RSDP signature.
 * Returns a pointer to the RSDP if found and checksum-valid, else NULL.
 */
static const rsdp_t *scan_rsdp(uint32_t start, uint32_t end)
{
    for (uint32_t addr = start; addr < end; addr += 16) {
        const rsdp_t *r = (const rsdp_t *)addr;
        if (!mem8eq(r->signature, "RSD PTR ")) continue;
        /* Verify v1 checksum (first 20 bytes) */
        if (acpi_checksum((const uint8_t *)r, 20) != 0) continue;
        /* If revision >= 2, verify extended checksum too */
        if (r->revision >= 2 && r->length >= 36) {
            if (acpi_checksum((const uint8_t *)r, r->length) != 0) continue;
        }
        return r;
    }
    return NULL;
}

/* ── SDT checksum verification ───────────────────────────────────────────── */

static bool sdt_valid(const sdt_header_t *hdr)
{
    if (hdr->length < sizeof(sdt_header_t)) return false;
    return acpi_checksum((const uint8_t *)hdr, hdr->length) == 0;
}

/* ── DSDT \_S5_ scan ─────────────────────────────────────────────────────── */

/*
 * Scan the DSDT byte stream for the AML \_S5_ package and extract
 * SLP_TYPa and SLP_TYPb.
 *
 * The AML encoding of:
 *   Name (\_S5_, Package (0x02) { 0x05, 0x05 })
 * is approximately:
 *   08  5F 53 35 5F  12  0A 02  0A 05  0A 05
 *   ^   \_S5_        ^   ^  ^   ^  ^   ^  ^
 *   DefName          Pkg  len  ByteData  ByteData
 *
 * We search for the 5-byte sequence: 08 5F 53 35 5F
 * then skip the Package opcode (0x12), PkgLength, NumElements,
 * and read the first two ByteData (0x0A <val>) entries.
 *
 * QEMU's DSDT uses exactly this encoding.  We default to 0 if not found.
 */
static void extract_s5(const uint8_t *dsdt_bytes, uint32_t dsdt_len,
                        uint16_t *slp_typa, uint16_t *slp_typb)
{
    *slp_typa = 0;
    *slp_typb = 0;

    if (dsdt_len < 40) return;

    /* Search for DefName \_S5_ */
    for (uint32_t i = 0; i + 12 < dsdt_len; i++) {
        if (dsdt_bytes[i]   != 0x08) continue;
        if (dsdt_bytes[i+1] != '_')  continue;
        if (dsdt_bytes[i+2] != 'S')  continue;
        if (dsdt_bytes[i+3] != '5')  continue;
        if (dsdt_bytes[i+4] != '_')  continue;

        /* Found \_S5_ name.  Next byte should be Package opcode 0x12. */
        uint32_t j = i + 5;
        if (j >= dsdt_len) break;

        /* Skip optional ScopeOp (0x10) that some compilers emit */
        if (dsdt_bytes[j] == 0x10) j++;
        if (j >= dsdt_len) break;

        if (dsdt_bytes[j] != 0x12) continue;  /* not a Package */
        j++;

        /*
         * PkgLength: variable-length encoding.
         * If bits 7:6 of the first byte are 00, it's a 1-byte length (bits 5:0).
         * Otherwise bits 7:6 give the number of additional bytes (1-3).
         */
        if (j >= dsdt_len) break;
        uint8_t pkg_lead = dsdt_bytes[j++];
        uint32_t extra = (pkg_lead >> 6) & 0x03;
        j += extra;  /* skip additional PkgLength bytes */

        /* NumElements byte */
        if (j >= dsdt_len) break;
        j++;  /* skip NumElements */

        /* Read SLP_TYPa: expect 0x0A (ByteData) followed by value */
        if (j + 1 >= dsdt_len) break;
        if (dsdt_bytes[j] == 0x0A) {
            *slp_typa = dsdt_bytes[j + 1];
            j += 2;
        } else if (dsdt_bytes[j] == 0x00) {
            /* ZeroOp — value is 0 */
            *slp_typa = 0;
            j++;
        } else {
            break;
        }

        /* Read SLP_TYPb */
        if (j + 1 >= dsdt_len) break;
        if (dsdt_bytes[j] == 0x0A) {
            *slp_typb = dsdt_bytes[j + 1];
        } else {
            *slp_typb = 0;
        }

        serial_puts("[ACPI] S5 found: SLP_TYPa=0x");
        serial_hex8((uint8_t)*slp_typa);
        serial_puts(" SLP_TYPb=0x");
        serial_hex8((uint8_t)*slp_typb);
        serial_puts("\n");
        return;
    }

    serial_puts("[ACPI] S5 not found in DSDT, defaulting SLP_TYPa=0\n");
}

/* ── FADT parser ─────────────────────────────────────────────────────────── */

static bool parse_fadt(const sdt_header_t *fadt_hdr)
{
    if (!sdt_valid(fadt_hdr)) {
        serial_puts("[ACPI] FADT checksum invalid\n");
        return false;
    }

    const uint8_t *f = (const uint8_t *)fadt_hdr;
    uint32_t flen = fadt_hdr->length;

    /* Require at least 90 bytes to read PM1_CNT_LEN */
    if (flen < 90) {
        serial_puts("[ACPI] FADT too short\n");
        return false;
    }

    uint32_t flags = read_u32(f, FADT_OFF_FLAGS);
    if (flags & FADT_FLAG_HW_REDUCED) {
        serial_puts("[ACPI] HW_REDUCED_ACPI set — no PM1 registers\n");
        return false;
    }

    g_pm1a_cnt    = read_u32(f, FADT_OFF_PM1a_CNT_BLK);
    g_pm1b_cnt    = read_u32(f, FADT_OFF_PM1b_CNT_BLK);
    g_pm1_cnt_len = f[FADT_OFF_PM1_CNT_LEN];
    if (g_pm1_cnt_len == 0) g_pm1_cnt_len = 2;

    serial_puts("[ACPI] FADT: PM1a_CNT=0x");
    serial_hex32(g_pm1a_cnt);
    if (g_pm1b_cnt) {
        serial_puts(" PM1b_CNT=0x");
        serial_hex32(g_pm1b_cnt);
    }
    serial_puts(" SMI_CMD=0x");
    serial_hex32(read_u32(f, FADT_OFF_SMI_CMD));
    serial_puts("\n");

    /* Check for ACPI reset register (FADT revision >= 2, length >= 129) */
    if (fadt_hdr->revision >= 2 && flen >= 129) {
        if (flags & FADT_FLAG_RESET_REG_SUP) {
            uint8_t gas_asid = f[FADT_OFF_RESET_REG];      /* address space */
            uint64_t gas_addr = read_u64(f, FADT_OFF_RESET_REG + 4);
            uint8_t  reset_val = f[FADT_OFF_RESET_VALUE];

            if (gas_asid == GAS_SYSTEM_IO && gas_addr <= 0xFFFF) {
                g_reset_port  = (uint16_t)gas_addr;
                g_reset_value = reset_val;
                g_reset_reg_ok = true;
                serial_puts("[ACPI] Reset register: port=0x");
                serial_hex16(g_reset_port);
                serial_puts(" val=0x");
                serial_hex8(g_reset_value);
                serial_puts("\n");
            }
        }
    }

    /* Extract DSDT address and scan for \_S5_ */
    uint32_t dsdt_addr = read_u32(f, FADT_OFF_DSDT);
    if (dsdt_addr >= 0x1000) {
        const sdt_header_t *dsdt_hdr = (const sdt_header_t *)dsdt_addr;
        if (mem4eq(dsdt_hdr->signature, "DSDT") && sdt_valid(dsdt_hdr)) {
            const uint8_t *dsdt_bytes = (const uint8_t *)dsdt_hdr;
            extract_s5(dsdt_bytes + sizeof(sdt_header_t),
                       dsdt_hdr->length - sizeof(sdt_header_t),
                       &g_slp_typa, &g_slp_typb);
        } else {
            serial_puts("[ACPI] DSDT not found or invalid, using SLP_TYPa=0\n");
        }
    }

    return true;
}

/* ── acpi_init ───────────────────────────────────────────────────────────── */

bool acpi_init(void)
{
    serial_puts("[ACPI] Scanning for RSDP...\n");

    const rsdp_t *rsdp = NULL;

    /*
     * Step 1: Check the EBDA.
     * The EBDA segment is stored as a 16-bit value at physical address 0x040E.
     * The RSDP may be in the first 1 KiB of the EBDA.
     */
    uint16_t ebda_seg = *(const uint16_t *)0x040E;
    if (ebda_seg >= 0x7000 && ebda_seg <= 0x9FFF) {
        uint32_t ebda_base = (uint32_t)ebda_seg << 4;
        rsdp = scan_rsdp(ebda_base, ebda_base + 1024);
    }

    /* Step 2: Scan the BIOS ROM area 0xE0000–0xFFFFF */
    if (!rsdp) {
        rsdp = scan_rsdp(0xE0000, 0x100000);
    }

    if (!rsdp) {
        serial_puts("[ACPI] RSDP not found — ACPI unavailable\n");
        return false;
    }

    serial_puts("[ACPI] RSDP found at 0x");
    serial_hex32((uint32_t)rsdp);
    serial_puts(" (rev=");
    serial_putchar('0' + rsdp->revision);
    serial_puts(")\n");

    /*
     * Step 3: Locate the FADT in RSDT or XSDT.
     *
     * RSDT: 32-bit pointers, each 4 bytes.
     * XSDT: 64-bit pointers, each 8 bytes.
     *
     * In a 32-bit kernel we only use the low 32 bits of XSDT entries.
     */
    const sdt_header_t *fadt_hdr = NULL;
    bool use_xsdt = (rsdp->revision >= 2 && rsdp->xsdt_address != 0);

    if (use_xsdt) {
        uint32_t xsdt_addr = (uint32_t)(rsdp->xsdt_address & 0xFFFFFFFF);
        const sdt_header_t *xsdt = (const sdt_header_t *)xsdt_addr;
        if (!sdt_valid(xsdt)) {
            serial_puts("[ACPI] XSDT checksum invalid, falling back to RSDT\n");
            use_xsdt = false;
        } else {
            serial_puts("[ACPI] Using XSDT at 0x");
            serial_hex32(xsdt_addr);
            serial_puts("\n");
            uint32_t n_entries = (xsdt->length - sizeof(sdt_header_t)) / 8;
            const uint8_t *entries = (const uint8_t *)xsdt + sizeof(sdt_header_t);
            for (uint32_t i = 0; i < n_entries; i++) {
                uint32_t entry_addr = read_u32(entries, i * 8);
                if (entry_addr < 0x1000) continue;
                const sdt_header_t *tbl = (const sdt_header_t *)entry_addr;
                if (mem4eq(tbl->signature, "FACP")) {
                    fadt_hdr = tbl;
                    break;
                }
            }
        }
    }

    if (!use_xsdt || !fadt_hdr) {
        uint32_t rsdt_addr = rsdp->rsdt_address;
        const sdt_header_t *rsdt = (const sdt_header_t *)rsdt_addr;
        if (!sdt_valid(rsdt)) {
            serial_puts("[ACPI] RSDT checksum invalid\n");
            return false;
        }
        serial_puts("[ACPI] Using RSDT at 0x");
        serial_hex32(rsdt_addr);
        serial_puts("\n");
        uint32_t n_entries = (rsdt->length - sizeof(sdt_header_t)) / 4;
        const uint8_t *entries = (const uint8_t *)rsdt + sizeof(sdt_header_t);
        for (uint32_t i = 0; i < n_entries; i++) {
            uint32_t entry_addr = read_u32(entries, i * 4);
            if (entry_addr < 0x1000) continue;
            const sdt_header_t *tbl = (const sdt_header_t *)entry_addr;
            if (mem4eq(tbl->signature, "FACP")) {
                fadt_hdr = tbl;
                break;
            }
        }
    }

    if (!fadt_hdr) {
        serial_puts("[ACPI] FADT (FACP) not found in RSDT/XSDT\n");
        return false;
    }

    serial_puts("[ACPI] FADT at 0x");
    serial_hex32((uint32_t)fadt_hdr);
    serial_puts("\n");

    if (!parse_fadt(fadt_hdr)) return false;

    if (g_pm1a_cnt == 0) {
        serial_puts("[ACPI] PM1a_CNT_BLK is 0 — cannot shutdown via ACPI\n");
        return false;
    }

    g_acpi_ok = true;
    serial_puts("[ACPI] Initialisation complete\n");
    return true;
}

/* ── acpi_available ──────────────────────────────────────────────────────── */

bool acpi_available(void)
{
    return g_acpi_ok;
}

/* ── acpi_shutdown ───────────────────────────────────────────────────────── */

void acpi_shutdown(void)
{
    serial_puts("[ACPI] Shutdown requested\n");

    if (g_acpi_ok && g_pm1a_cnt != 0) {
        /*
         * Write SLP_TYPa (bits 13:10) | SLP_EN (bit 13) to PM1a_CNT_BLK.
         * SLP_TYPa is stored as a raw 3-bit value; shift it into position.
         */
        uint16_t pm1a_val = (uint16_t)((g_slp_typa << 10) | SLP_EN);
        serial_puts("[ACPI] Writing 0x");
        serial_hex16(pm1a_val);
        serial_puts(" to PM1a_CNT 0x");
        serial_hex32(g_pm1a_cnt);
        serial_puts("\n");

        outw((uint16_t)g_pm1a_cnt, pm1a_val);

        /* Small delay to let the hardware process the write */
        for (volatile int i = 0; i < 1000; i++) io_wait();

        /* If PM1b_CNT_BLK is present, write to it too */
        if (g_pm1b_cnt != 0) {
            uint16_t pm1b_val = (uint16_t)((g_slp_typb << 10) | SLP_EN);
            outw((uint16_t)g_pm1b_cnt, pm1b_val);
            for (volatile int i = 0; i < 1000; i++) io_wait();
        }

        serial_puts("[ACPI] PM1_CNT write done — machine should power off\n");
    } else {
        serial_puts("[ACPI] ACPI not available, using fallback shutdown\n");
    }

    /*
     * Fallback 1: QEMU ISA debug exit device.
     * Writing 0x31 to port 0x501 causes QEMU to exit with code
     * ((0x31 << 1) | 1) = 99.  Writing 0x00 exits with code 1.
     * We write 0x00 for a clean "power off" exit.
     */
    serial_puts("[ACPI] Fallback: QEMU ISA debug exit\n");
    outb(0x501, 0x00);

    /*
     * Fallback 2: Triple fault — load a null IDT and trigger a fault.
     * This is the "nuclear option" and will cause QEMU to reset rather
     * than power off, but it is better than spinning forever.
     */
    serial_puts("[ACPI] Fallback: triple fault\n");
    __asm__ volatile (
        "cli\n"
        "lidt %0\n"
        "int $0\n"
        :
        : "m"(*(uint64_t *)0)   /* load IDT limit=0, base=0 */
    );

    /* Should never reach here */
    for (;;) __asm__ volatile ("hlt");
}

/* ── acpi_reboot ─────────────────────────────────────────────────────────── */

void acpi_reboot(void)
{
    serial_puts("[ACPI] Reboot requested\n");

    /* Method 1: ACPI reset register (FADT rev >= 2) */
    if (g_reset_reg_ok) {
        serial_puts("[ACPI] Reboot via ACPI reset register port=0x");
        serial_hex16(g_reset_port);
        serial_puts(" val=0x");
        serial_hex8(g_reset_value);
        serial_puts("\n");
        outb(g_reset_port, g_reset_value);
        for (volatile int i = 0; i < 10000; i++) io_wait();
    }

    /*
     * Method 2: Keyboard controller CPU reset pulse.
     *
     * The 8042 keyboard controller has a line connected to the CPU RESET pin.
     * Writing 0xFE to port 0x64 (KBC command port) pulses this line low,
     * causing a hardware reset.  This works on virtually all x86 machines.
     *
     * Sequence:
     *   1. Wait for KBC input buffer to be empty (bit 1 of port 0x64 = 0).
     *   2. Write 0xFE to port 0x64 (pulse output port).
     */
    serial_puts("[ACPI] Reboot via keyboard controller\n");

    /* Drain the KBC input buffer */
    uint32_t timeout = 100000;
    while ((inb(0x64) & 0x02) && timeout--) io_wait();

    /* Pulse the CPU reset line */
    outb(0x64, 0xFE);

    /* Wait for reset to take effect */
    for (volatile int i = 0; i < 100000; i++) io_wait();

    /*
     * Method 3: Triple fault.
     */
    serial_puts("[ACPI] Fallback: triple fault reboot\n");
    __asm__ volatile (
        "cli\n"
        "lidt %0\n"
        "int $0\n"
        :
        : "m"(*(uint64_t *)0)
    );

    for (;;) __asm__ volatile ("hlt");
}
