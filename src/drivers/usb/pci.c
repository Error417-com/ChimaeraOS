/*
 * ChimaeraOS — PCI Configuration Space Access
 * src/drivers/usb/pci.c
 *
 * Implements PCI config space access via the legacy I/O port mechanism
 * (ports 0xCF8 / 0xCFC).  Works on all x86 systems and QEMU.
 */

#include "pci.h"
#include "../../include/types.h"

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

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ── PCI address construction ────────────────────────────────────────────── */

#define PCI_ADDR_PORT  0xCF8
#define PCI_DATA_PORT  0xCFC

static uint32_t pci_make_addr(pci_addr_t a, uint8_t offset)
{
    return (1u << 31)                       /* enable bit  */
         | ((uint32_t)a.bus  << 16)
         | ((uint32_t)a.dev  << 11)
         | ((uint32_t)a.func <<  8)
         | (offset & 0xFC);                 /* dword-align */
}

/* ── Read functions ──────────────────────────────────────────────────────── */

uint32_t pci_read32(pci_addr_t addr, uint8_t offset)
{
    outl(PCI_ADDR_PORT, pci_make_addr(addr, offset));
    return inl(PCI_DATA_PORT);
}

uint16_t pci_read16(pci_addr_t addr, uint8_t offset)
{
    outl(PCI_ADDR_PORT, pci_make_addr(addr, offset));
    return inw((uint16_t)(PCI_DATA_PORT + (offset & 2)));
}

uint8_t pci_read8(pci_addr_t addr, uint8_t offset)
{
    outl(PCI_ADDR_PORT, pci_make_addr(addr, offset));
    return inb((uint16_t)(PCI_DATA_PORT + (offset & 3)));
}

/* ── Write functions ─────────────────────────────────────────────────────── */

void pci_write32(pci_addr_t addr, uint8_t offset, uint32_t val)
{
    outl(PCI_ADDR_PORT, pci_make_addr(addr, offset));
    outl(PCI_DATA_PORT, val);
}

void pci_write16(pci_addr_t addr, uint8_t offset, uint16_t val)
{
    outl(PCI_ADDR_PORT, pci_make_addr(addr, offset));
    outw((uint16_t)(PCI_DATA_PORT + (offset & 2)), val);
}

void pci_write8(pci_addr_t addr, uint8_t offset, uint8_t val)
{
    outl(PCI_ADDR_PORT, pci_make_addr(addr, offset));
    outb((uint16_t)(PCI_DATA_PORT + (offset & 3)), val);
}

/* ── pci_find_device ─────────────────────────────────────────────────────── */

bool pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                     pci_addr_t *out)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                pci_addr_t a = { (uint8_t)bus, dev, func };

                uint16_t vendor = pci_read16(a, PCI_VENDOR_ID);
                if (vendor == 0xFFFF) continue;  /* no device */

                uint8_t cc  = pci_read8(a, PCI_CLASS_CODE);
                uint8_t sc  = pci_read8(a, PCI_SUBCLASS);
                uint8_t pi  = pci_read8(a, PCI_PROG_IF);

                if (cc == class_code && sc == subclass &&
                    (prog_if == 0xFF || pi == prog_if)) {
                    *out = a;
                    return true;
                }

                /* If not a multi-function device, skip remaining funcs */
                if (func == 0) {
                    uint8_t hdr = pci_read8(a, PCI_HEADER_TYPE);
                    if (!(hdr & 0x80)) break;
                }
            }
        }
    }
    return false;
}

/* ── pci_enable_device ───────────────────────────────────────────────────── */

void pci_enable_device(pci_addr_t addr)
{
    uint16_t cmd = pci_read16(addr, PCI_COMMAND);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_write16(addr, PCI_COMMAND, cmd);
}

/* ── pci_get_bar_io ──────────────────────────────────────────────────────── */

uint16_t pci_get_bar_io(pci_addr_t addr, uint8_t bar_index)
{
    if (bar_index > 5) return 0;
    uint8_t  offset = (uint8_t)(PCI_BAR0 + bar_index * 4);
    uint32_t bar    = pci_read32(addr, offset);

    /* Bit 0 = 1 means I/O space BAR */
    if (!(bar & 1)) return 0;

    /* Bits 31:2 are the base address; bits 1:0 are flags */
    return (uint16_t)(bar & 0xFFFC);
}
