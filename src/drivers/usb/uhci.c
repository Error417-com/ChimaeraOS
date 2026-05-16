/*
 * ChimaeraOS — UHCI Host Controller Driver
 * src/drivers/usb/uhci.c
 *
 * Implements UHCI controller initialisation, port reset, control transfers
 * (SETUP + DATA + STATUS), and periodic TD insertion for interrupt endpoints.
 *
 * Design decisions
 * ────────────────
 * • All TD/QH structures are allocated from the kernel heap (kmalloc).
 *   Since we run without paging, virtual == physical addresses.
 *
 * • Control transfers are synchronous (polled).  We insert a single QH
 *   into frame 0, wait for completion (polling USBSTS), then remove it.
 *   Timeout is 100 ms (10,000 iterations × ~10 µs per loop).
 *
 * • Periodic (interrupt) TDs are inserted into every Nth frame list entry
 *   directly (no QH wrapper) for simplicity.
 *
 * • The frame list is 1024 × uint32_t = 4 KiB, allocated from the heap
 *   and aligned to 4 KiB using a small over-allocation trick.
 *
 * • We do NOT use UHCI interrupts.  The HID driver polls TDs on each
 *   scheduler tick via uhci_td_done().
 */

#include "uhci.h"
#include "pci.h"
#include "../../include/types.h"
#include "../../include/mm.h"
#include "../../include/serial.h"
#include "../../include/timer.h"
#include "../../include/panic.h"

/* ── Port I/O helpers ────────────────────────────────────────────────────── */

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb_p(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb_p(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ── Small delay helper (busy-wait ~1 ms) ────────────────────────────────── */

static void uhci_delay_ms(uint32_t ms)
{
    uint32_t end = timer_ms() + ms;
    while (timer_ms() < end) {
        __asm__ volatile ("pause");
    }
}

/* ── UHCI register accessors ─────────────────────────────────────────────── */

static inline uint16_t uhci_rd16(uint16_t base, uint8_t reg)
{
    return inw((uint16_t)(base + reg));
}

static inline void uhci_wr16(uint16_t base, uint8_t reg, uint16_t val)
{
    outw((uint16_t)(base + reg), val);
}

static inline uint32_t uhci_rd32(uint16_t base, uint8_t reg)
{
    return inl((uint16_t)(base + reg));
}

static inline void uhci_wr32(uint16_t base, uint8_t reg, uint32_t val)
{
    outl((uint16_t)(base + reg), val);
}

/* ── Global controller state ─────────────────────────────────────────────── */

static uhci_ctrl_t g_uhci;
static bool        g_uhci_ready = false;

/* ── Frame list allocation (4 KiB aligned) ───────────────────────────────── */

static uint32_t *alloc_frame_list(void)
{
    /*
     * Allocate 4 KiB + 4 KiB = 8 KiB to guarantee a 4 KiB-aligned region
     * within the allocation.  This wastes up to 4 KiB but avoids needing
     * a custom aligned allocator.
     */
    uint8_t *raw = (uint8_t *)kmalloc(4096 + 4096);
    if (!raw) return NULL;

    /* Round up to next 4 KiB boundary */
    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = (addr + 4095) & ~(uintptr_t)4095;
    return (uint32_t *)aligned;
}

/* ── uhci_alloc_td / uhci_alloc_qh ──────────────────────────────────────── */

uhci_td_t *uhci_alloc_td(void)
{
    /*
     * TDs must be 16-byte aligned.  kmalloc aligns to 4 bytes.
     * Over-allocate by 16 bytes and round up.
     */
    uint8_t *raw = (uint8_t *)kmalloc(sizeof(uhci_td_t) + 16);
    if (!raw) return NULL;
    uintptr_t addr = ((uintptr_t)raw + 15) & ~(uintptr_t)15;
    uhci_td_t *td = (uhci_td_t *)addr;
    /* Zero all fields */
    uint8_t *p = (uint8_t *)td;
    for (uint32_t i = 0; i < sizeof(uhci_td_t); i++) p[i] = 0;
    return td;
}

uhci_qh_t *uhci_alloc_qh(void)
{
    uint8_t *raw = (uint8_t *)kmalloc(sizeof(uhci_qh_t) + 16);
    if (!raw) return NULL;
    uintptr_t addr = ((uintptr_t)raw + 15) & ~(uintptr_t)15;
    uhci_qh_t *qh = (uhci_qh_t *)addr;
    uint8_t *p = (uint8_t *)qh;
    for (uint32_t i = 0; i < sizeof(uhci_qh_t); i++) p[i] = 0;
    return qh;
}

/* ── TD helper functions ─────────────────────────────────────────────────── */

bool uhci_td_done(const uhci_td_t *td)
{
    return !(td->status & TD_STS_ACTIVE);
}

bool uhci_td_error(const uhci_td_t *td)
{
    return !!(td->status & (TD_STS_STALLED | TD_STS_DBUFERR |
                            TD_STS_BABBLE  | TD_STS_CRCTO   |
                            TD_STS_BITSTUFF));
}

uint16_t uhci_td_actual_len(const uhci_td_t *td)
{
    /* Bits 10:0 of status = actual length - 1 (0x7FF = 0 bytes) */
    uint16_t raw = (uint16_t)(td->status & 0x7FF);
    if (raw == 0x7FF) return 0;
    return (uint16_t)(raw + 1);
}

void uhci_reactivate_td(uhci_td_t *td)
{
    /* Flip data toggle (bit 19 of token) for next transfer */
    td->token ^= TD_TOKEN_TOGGLE(1);
    /* Clear status bits, set ACTIVE, preserve token and buffer */
    td->status = (td->status & ~0x00FFFFFF) | TD_STS_ACTIVE |
                 TD_STS_ERRCNT(3) | TD_STS_IOC;
}

/* ── uhci_init ───────────────────────────────────────────────────────────── */

bool uhci_init(void)
{
    /* Find UHCI controller on PCI */
    pci_addr_t pci;
    if (!pci_find_device(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                         PCI_PROGIF_UHCI, &pci)) {
        serial_puts("[UHCI] No UHCI controller found on PCI\r\n");
        return false;
    }

    /* Enable bus mastering and I/O space */
    pci_enable_device(pci);

    /* Get I/O base from BAR4 */
    uint16_t iobase = pci_get_bar_io(pci, 4);
    if (iobase == 0) {
        serial_puts("[UHCI] BAR4 not I/O mapped or not set\r\n");
        return false;
    }

    serial_puts("[UHCI] Controller at PCI ");
    serial_hex8(pci.bus); serial_puts(":"); serial_hex8(pci.dev);
    serial_puts("."); serial_hex8(pci.func);
    serial_puts(" I/O base="); serial_hex16(iobase); serial_puts("\r\n");

    g_uhci.iobase = iobase;
    g_uhci.pci    = pci;

    /* ── Global reset ──────────────────────────────────────────────────── */
    uhci_wr16(iobase, UHCI_USBCMD, UHCI_CMD_GRESET);
    uhci_delay_ms(10);
    uhci_wr16(iobase, UHCI_USBCMD, 0);
    uhci_delay_ms(2);

    /* ── Host controller reset ─────────────────────────────────────────── */
    uhci_wr16(iobase, UHCI_USBCMD, UHCI_CMD_HCRESET);
    uint32_t timeout = timer_ms() + 50;
    while (uhci_rd16(iobase, UHCI_USBCMD) & UHCI_CMD_HCRESET) {
        if (timer_ms() > timeout) {
            serial_puts("[UHCI] HC reset timed out\r\n");
            return false;
        }
    }

    /* ── Disable all interrupts ────────────────────────────────────────── */
    uhci_wr16(iobase, UHCI_USBINTR, 0);

    /* ── Clear status register ─────────────────────────────────────────── */
    uhci_wr16(iobase, UHCI_USBSTS, 0x3F);

    /* ── Set SOF modify to 64 (default) ───────────────────────────────── */
    outb_p((uint16_t)(iobase + UHCI_SOFMOD), 0x40);

    /* ── Allocate and initialise frame list ────────────────────────────── */
    uint32_t *fl = alloc_frame_list();
    if (!fl) {
        serial_puts("[UHCI] Failed to allocate frame list\r\n");
        return false;
    }
    /* All entries terminate (no TDs scheduled yet) */
    for (uint32_t i = 0; i < UHCI_FRAME_COUNT; i++) {
        fl[i] = UHCI_LP_TERMINATE;
    }
    g_uhci.frame_list = fl;

    /* Write frame list base address */
    uhci_wr32(iobase, UHCI_FLBASEADD, (uint32_t)fl);

    /* Reset frame number */
    uhci_wr16(iobase, UHCI_FRNUM, 0);

    /* ── Set max packet size to 64 bytes (MAXP=1) ──────────────────────── */
    uhci_wr16(iobase, UHCI_USBCMD, UHCI_CMD_MAXP);

    /* ── Start the controller ──────────────────────────────────────────── */
    uhci_wr16(iobase, UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_MAXP);

    /* Wait for HC to start (HCH bit should clear) */
    timeout = timer_ms() + 50;
    while (uhci_rd16(iobase, UHCI_USBSTS) & UHCI_STS_HCH) {
        if (timer_ms() > timeout) {
            serial_puts("[UHCI] HC did not start\r\n");
            return false;
        }
    }

    serial_puts("[UHCI] Controller running\r\n");

    /* ── Reset and probe ports ─────────────────────────────────────────── */
    g_uhci.port_count = 0;

    for (uint8_t p = 0; p < UHCI_MAX_PORTS; p++) {
        uint8_t  reg = (p == 0) ? UHCI_PORTSC0 : UHCI_PORTSC1;
        uint16_t sc  = uhci_rd16(iobase, reg);

        /* Check if port is valid (reserved bits must be 0) */
        if (sc == 0xFFFF) continue;

        g_uhci.port_count++;

        /* Clear connect status change */
        uhci_wr16(iobase, reg, UHCI_PORT_CSC | UHCI_PORT_PEDC);
        uhci_delay_ms(2);

        sc = uhci_rd16(iobase, reg);
        if (!(sc & UHCI_PORT_CCS)) {
            serial_puts("[UHCI] Port ");
            serial_dec(p + 1);
            serial_puts(": no device\r\n");
            g_uhci.port_connected[p] = false;
            continue;
        }

        /* Device detected — perform port reset */
        serial_puts("[UHCI] Port ");
        serial_dec(p + 1);
        serial_puts(": device detected, resetting...\r\n");

        uhci_wr16(iobase, reg, UHCI_PORT_RESET);
        uhci_delay_ms(50);   /* USB spec: >= 10 ms reset pulse */
        uhci_wr16(iobase, reg, 0);
        uhci_delay_ms(2);

        /* Enable the port */
        uhci_wr16(iobase, reg, UHCI_PORT_PED);
        uhci_delay_ms(10);

        sc = uhci_rd16(iobase, reg);
        if (!(sc & UHCI_PORT_PED)) {
            /* Try clearing change bits and re-enabling */
            uhci_wr16(iobase, reg, UHCI_PORT_CSC | UHCI_PORT_PEDC | UHCI_PORT_PED);
            uhci_delay_ms(10);
            sc = uhci_rd16(iobase, reg);
        }

        g_uhci.port_connected[p] = !!(sc & UHCI_PORT_CCS);
        g_uhci.port_speed[p]     = (sc & UHCI_PORT_LSDA) ? 1 : 0;

        serial_puts("[UHCI] Port ");
        serial_dec(p + 1);
        serial_puts(g_uhci.port_speed[p] ? ": low-speed device enabled\r\n"
                                         : ": full-speed device enabled\r\n");
    }

    g_uhci_ready = true;
    return true;
}

/* ── uhci_get ────────────────────────────────────────────────────────────── */

uhci_ctrl_t *uhci_get(void)
{
    return g_uhci_ready ? &g_uhci : NULL;
}

/* ── uhci_control_transfer ───────────────────────────────────────────────── */
/*
 * Synchronous control transfer using a single QH + TD chain.
 *
 * Frame list structure during transfer:
 *   frame[0] → QH → SETUP TD → DATA TD(s) → STATUS TD → terminate
 *
 * We insert the QH at frame 0 only, wait for completion, then remove it.
 */

bool uhci_control_transfer(uint8_t dev_addr, bool low_speed,
                           const uint8_t *setup_pkt,
                           uint8_t *data, uint16_t data_len,
                           bool is_in)
{
    if (!g_uhci_ready) return false;

    uint16_t iobase = g_uhci.iobase;
    uint32_t ls_bit = low_speed ? TD_STS_LS : 0;

    /* ── Allocate QH and TDs ────────────────────────────────────────────── */

    uhci_qh_t *qh = uhci_alloc_qh();
    if (!qh) return false;

    /* SETUP TD */
    uhci_td_t *setup_td = uhci_alloc_td();
    if (!setup_td) return false;

    /* Copy setup packet to a heap buffer (needs stable physical address) */
    uint8_t *setup_buf = (uint8_t *)kmalloc(8);
    if (!setup_buf) return false;
    for (int i = 0; i < 8; i++) setup_buf[i] = setup_pkt[i];

    setup_td->status = TD_STS_ACTIVE | TD_STS_ERRCNT(3) | ls_bit;
    setup_td->token  = TD_TOKEN_PID(USB_PID_SETUP)
                     | TD_TOKEN_DEVADDR(dev_addr)
                     | TD_TOKEN_ENDPT(0)
                     | TD_TOKEN_TOGGLE(0)
                     | TD_TOKEN_MAXLEN(8);
    setup_td->buffer = (uint32_t)setup_buf;

    /* ── DATA TDs (if any) ──────────────────────────────────────────────── */

    uhci_td_t *last_td = setup_td;
    uint8_t toggle = 1;  /* DATA1 for first data packet */

    if (data && data_len > 0) {
        uint16_t remaining = data_len;
        uint8_t *ptr = data;
        uint8_t pid = is_in ? USB_PID_IN : USB_PID_OUT;

        while (remaining > 0) {
            uint16_t chunk = (remaining > 64) ? 64 : remaining;

            uhci_td_t *dtd = uhci_alloc_td();
            if (!dtd) return false;

            dtd->status = TD_STS_ACTIVE | TD_STS_ERRCNT(3) | ls_bit;
            if (is_in) dtd->status |= TD_STS_SPD;  /* short packet detect */
            dtd->token  = TD_TOKEN_PID(pid)
                        | TD_TOKEN_DEVADDR(dev_addr)
                        | TD_TOKEN_ENDPT(0)
                        | TD_TOKEN_TOGGLE(toggle)
                        | TD_TOKEN_MAXLEN(chunk);
            dtd->buffer = (uint32_t)ptr;

            last_td->link = (uint32_t)dtd | UHCI_LP_DEPTH;
            last_td = dtd;

            toggle ^= 1;
            ptr += chunk;
            remaining = (uint16_t)(remaining - chunk);
        }
    }

    /* ── STATUS TD ──────────────────────────────────────────────────────── */

    uhci_td_t *status_td = uhci_alloc_td();
    if (!status_td) return false;

    /* Status phase: opposite direction to data phase, DATA1, zero length */
    uint8_t status_pid = is_in ? USB_PID_OUT : USB_PID_IN;
    status_td->status = TD_STS_ACTIVE | TD_STS_ERRCNT(3) | TD_STS_IOC | ls_bit;
    status_td->token  = TD_TOKEN_PID(status_pid)
                      | TD_TOKEN_DEVADDR(dev_addr)
                      | TD_TOKEN_ENDPT(0)
                      | TD_TOKEN_TOGGLE(1)
                      | TD_TOKEN_MAXLEN0;
    status_td->buffer = 0;
    status_td->link   = UHCI_LP_TERMINATE;

    last_td->link = (uint32_t)status_td | UHCI_LP_DEPTH;

    /* ── Wire up QH ─────────────────────────────────────────────────────── */

    qh->link    = UHCI_LP_TERMINATE;
    qh->element = (uint32_t)setup_td;

    /* ── Insert QH into current + next 16 frames (HC keeps running) ──────── */
    /*
     * Read the current frame number and insert the QH into the next 16
     * frames so the HC is guaranteed to pick it up in the next 16 ms.
     * We save the original frame list entries and restore them after.
     */

    uint16_t frnum = uhci_rd16(iobase, UHCI_FRNUM) & 0x3FF;
    uint32_t saved_frames[16];
    for (int fi = 0; fi < 16; fi++) {
        uint16_t idx = (uint16_t)((frnum + 2 + fi) & 0x3FF);
        saved_frames[fi] = g_uhci.frame_list[idx];
        /* Chain: frame → QH → (old frame entry) */
        qh->link = saved_frames[fi];  /* QH horizontal link = old entry */
        g_uhci.frame_list[idx] = (uint32_t)qh | UHCI_LP_QH;
    }

    /* ── Wait for STATUS TD to complete ─────────────────────────────────── */

    bool ok = false;
    uint32_t deadline = timer_ms() + 500;  /* 500 ms timeout */

    while (timer_ms() < deadline) {
        if (uhci_td_done(status_td)) {
            ok = !uhci_td_error(status_td);
            break;
        }
        /* Also check for stall/error on setup TD */
        if (uhci_td_done(setup_td) && uhci_td_error(setup_td)) {
            break;
        }
        __asm__ volatile ("pause");
    }

    /* ── Remove QH from frame list ──────────────────────────────────────── */

    for (int fi = 0; fi < 16; fi++) {
        uint16_t idx = (uint16_t)((frnum + 2 + fi) & 0x3FF);
        g_uhci.frame_list[idx] = saved_frames[fi];
    }

    return ok;
}

/* ── uhci_insert_periodic_td ─────────────────────────────────────────────── */

void uhci_insert_periodic_td(uhci_td_t *td, uint8_t interval)
{
    if (!g_uhci_ready || !td) return;
    if (interval == 0) interval = 1;

    uint32_t *fl = g_uhci.frame_list;

    for (uint32_t i = 0; i < UHCI_FRAME_COUNT; i += interval) {
        /* Insert TD at the head of this frame's list */
        td->link = fl[i];
        fl[i] = (uint32_t)td;   /* no QH bit, no depth bit = breadth-first */
    }
}

/* ── uhci_remove_periodic_td ─────────────────────────────────────────────── */

void uhci_remove_periodic_td(uhci_td_t *td)
{
    if (!g_uhci_ready || !td) return;

    uint32_t *fl = g_uhci.frame_list;
    uint32_t  td_phys = (uint32_t)td;

    for (uint32_t i = 0; i < UHCI_FRAME_COUNT; i++) {
        if ((fl[i] & ~0xFu) == td_phys) {
            /* Remove by replacing with TD's own link */
            fl[i] = td->link;
        }
    }
}
