/*
 * ChimaeraOS — PCI Configuration Space Access
 * src/drivers/usb/pci.h
 *
 * Minimal PCI config space read/write via I/O ports 0xCF8 (address) and
 * 0xCFC (data).  Supports 32-bit, 16-bit, and 8-bit accesses.
 *
 * PCI address format (32 bits):
 *   bit 31    : enable bit (must be 1)
 *   bits 23:16: bus number (0–255)
 *   bits 15:11: device number (0–31)
 *   bits 10:8 : function number (0–7)
 *   bits 7:2  : register offset (dword-aligned)
 *   bits 1:0  : always 0
 */

#ifndef PCI_H
#define PCI_H

#include "../../include/types.h"

/* ── PCI standard config register offsets ───────────────────────────────── */

#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS_CODE      0x0B
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D

/* PCI command register bits */
#define PCI_CMD_IO_SPACE    (1u << 0)   /* enable I/O space access  */
#define PCI_CMD_MEM_SPACE   (1u << 1)   /* enable memory space      */
#define PCI_CMD_BUS_MASTER  (1u << 2)   /* enable bus mastering     */

/* PCI class codes for USB */
#define PCI_CLASS_SERIAL_BUS  0x0C
#define PCI_SUBCLASS_USB      0x03
#define PCI_PROGIF_UHCI       0x00
#define PCI_PROGIF_OHCI       0x10
#define PCI_PROGIF_EHCI       0x20
#define PCI_PROGIF_XHCI       0x30

/* ── PCI device location descriptor ─────────────────────────────────────── */

typedef struct {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
} pci_addr_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

uint32_t pci_read32(pci_addr_t addr, uint8_t offset);
uint16_t pci_read16(pci_addr_t addr, uint8_t offset);
uint8_t  pci_read8 (pci_addr_t addr, uint8_t offset);

void pci_write32(pci_addr_t addr, uint8_t offset, uint32_t val);
void pci_write16(pci_addr_t addr, uint8_t offset, uint16_t val);
void pci_write8 (pci_addr_t addr, uint8_t offset, uint8_t  val);

/*
 * pci_find_device — scan all buses/devices/functions for a device with
 * the given class/subclass/prog_if.  Returns true and fills *out if found.
 * Set prog_if to 0xFF to match any prog_if.
 */
bool pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                     pci_addr_t *out);

/*
 * pci_enable_device — set the bus-master + I/O space bits in the command
 * register, enabling the device for DMA and I/O access.
 */
void pci_enable_device(pci_addr_t addr);

/*
 * pci_get_bar_io — return the I/O base address from BARn (strips the
 * I/O indicator bit).  Returns 0 if the BAR is a memory BAR or unset.
 */
uint16_t pci_get_bar_io(pci_addr_t addr, uint8_t bar_index);

#endif /* PCI_H */
