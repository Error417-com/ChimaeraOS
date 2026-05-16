/*
 * ChimaeraOS — UHCI Host Controller Driver
 * src/drivers/usb/uhci.h
 *
 * Universal Host Controller Interface (UHCI) register definitions,
 * Transfer Descriptor (TD) and Queue Head (QH) structures, and the
 * public API for the UHCI driver.
 *
 * Reference: Intel UHCI Design Guide, Revision 1.1
 *
 * UHCI I/O register map (base = BAR4 I/O base)
 * ─────────────────────────────────────────────
 * USBCMD   +0x00  2 bytes  USB Command
 * USBSTS   +0x02  2 bytes  USB Status
 * USBINTR  +0x04  2 bytes  USB Interrupt Enable
 * FRNUM    +0x06  2 bytes  Frame Number
 * FLBASEADD+0x08  4 bytes  Frame List Base Address (physical, 4 KiB aligned)
 * SOFMOD   +0x0C  1 byte   Start Of Frame Modify
 * PORTSC0  +0x10  2 bytes  Port 1 Status/Control
 * PORTSC1  +0x12  2 bytes  Port 2 Status/Control
 *
 * USBCMD bits
 * ───────────
 * bit 0: Run/Stop (RS)     — 1 = run, 0 = stop
 * bit 1: Host Controller Reset (HCRESET)
 * bit 2: Global Reset (GRESET)
 * bit 3: Enter Global Suspend Mode (EGSM)
 * bit 4: Force Global Resume (FGR)
 * bit 5: Software Debug (SWDBG)
 * bit 6: Configure Flag (CF)
 * bit 7: Max Packet (MAXP) — 0 = 32 bytes, 1 = 64 bytes
 *
 * USBSTS bits
 * ───────────
 * bit 0: USB Interrupt (USBINT)
 * bit 1: USB Error Interrupt (USBERR)
 * bit 2: Resume Detect (RD)
 * bit 3: Host System Error (HSE)
 * bit 4: Host Controller Process Error (HCPE)
 * bit 5: HC Halted (HCH)
 *
 * PORTSC bits
 * ───────────
 * bit 0:  Current Connect Status (CCS)
 * bit 1:  Connect Status Change (CSC)  — write 1 to clear
 * bit 2:  Port Enabled/Disabled (PED)
 * bit 3:  Port Enable/Disable Change (PEDC)  — write 1 to clear
 * bit 4:  Line Status D+ (LS)
 * bit 8:  Port Reset (PR)
 * bit 9:  Suspend (SUSP)
 * bit 10: Low Speed Device Attached (LSDA)
 */

#ifndef UHCI_H
#define UHCI_H

#include "../../include/types.h"
#include "pci.h"

/* ── UHCI I/O register offsets ───────────────────────────────────────────── */

#define UHCI_USBCMD     0x00
#define UHCI_USBSTS     0x02
#define UHCI_USBINTR    0x04
#define UHCI_FRNUM      0x06
#define UHCI_FLBASEADD  0x08
#define UHCI_SOFMOD     0x0C
#define UHCI_PORTSC0    0x10
#define UHCI_PORTSC1    0x12

/* USBCMD bits */
#define UHCI_CMD_RS         (1u << 0)
#define UHCI_CMD_HCRESET    (1u << 1)
#define UHCI_CMD_GRESET     (1u << 2)
#define UHCI_CMD_MAXP       (1u << 7)

/* USBSTS bits */
#define UHCI_STS_USBINT     (1u << 0)
#define UHCI_STS_USBERR     (1u << 1)
#define UHCI_STS_RD         (1u << 2)
#define UHCI_STS_HSE        (1u << 3)
#define UHCI_STS_HCPE       (1u << 4)
#define UHCI_STS_HCH        (1u << 5)

/* PORTSC bits */
#define UHCI_PORT_CCS       (1u << 0)   /* device connected         */
#define UHCI_PORT_CSC       (1u << 1)   /* connect status changed   */
#define UHCI_PORT_PED       (1u << 2)   /* port enabled             */
#define UHCI_PORT_PEDC      (1u << 3)   /* port enable changed      */
#define UHCI_PORT_LSDA      (1u << 8)   /* low-speed device         */
#define UHCI_PORT_RESET     (1u << 9)   /* port reset               */

/* ── Frame List ──────────────────────────────────────────────────────────── */

#define UHCI_FRAME_COUNT    1024        /* number of frame list entries */

/* Frame list entry / link pointer bits */
#define UHCI_LP_TERMINATE   (1u << 0)   /* no next entry            */
#define UHCI_LP_QH          (1u << 1)   /* pointer is to a QH       */
#define UHCI_LP_DEPTH       (1u << 2)   /* depth-first traversal    */

/* ── Transfer Descriptor (TD) ────────────────────────────────────────────── */
/*
 * Each TD is 32 bytes (16 bytes used by hardware, 16 bytes software).
 * Must be 16-byte aligned.
 *
 * td_link:   physical address of next TD/QH, or UHCI_LP_TERMINATE
 * td_status: status bits (written by HC)
 * td_token:  PID, device address, endpoint, data toggle, max length
 * td_buffer: physical address of data buffer
 */

typedef struct __attribute__((packed, aligned(16))) {
    /* Hardware fields (bytes 0–15) */
    uint32_t  link;         /* next TD/QH link pointer                   */
    uint32_t  status;       /* control/status                            */
    uint32_t  token;        /* PID | devaddr | endpt | toggle | maxlen   */
    uint32_t  buffer;       /* physical address of data buffer           */
    /* Software fields (bytes 16–31) */
    uint32_t  sw_next;      /* software next pointer (for free list)     */
    uint32_t  sw_buf_virt;  /* virtual address of data buffer            */
    uint32_t  sw_pad[2];    /* padding to 32 bytes                       */
} uhci_td_t;

/* td_status bits */
#define TD_STS_ACTIVE       (1u << 23)  /* HC owns this TD              */
#define TD_STS_STALLED      (1u << 22)  /* endpoint stalled             */
#define TD_STS_DBUFERR      (1u << 21)  /* data buffer error            */
#define TD_STS_BABBLE       (1u << 20)  /* babble detected              */
#define TD_STS_NAK          (1u << 19)  /* NAK received                 */
#define TD_STS_CRCTO        (1u << 18)  /* CRC/timeout error            */
#define TD_STS_BITSTUFF     (1u << 17)  /* bit stuffing error           */
#define TD_STS_IOC          (1u << 24)  /* interrupt on complete        */
#define TD_STS_ISO          (1u << 25)  /* isochronous TD               */
#define TD_STS_LS           (1u << 26)  /* low-speed device             */
#define TD_STS_ERRCNT(n)    (((n) & 3u) << 27) /* error counter (0–3)  */
#define TD_STS_SPD          (1u << 29)  /* short packet detect          */

/* td_token fields */
#define TD_TOKEN_PID(p)     ((p) & 0xFF)
#define TD_TOKEN_DEVADDR(a) (((a) & 0x7F) << 8)
#define TD_TOKEN_ENDPT(e)   (((e) & 0x0F) << 15)
#define TD_TOKEN_TOGGLE(t)  (((t) & 0x01) << 19)
#define TD_TOKEN_MAXLEN(n)  ((((n) - 1) & 0x7FF) << 21)
#define TD_TOKEN_MAXLEN0    (0x7FFu << 21)  /* maxlen=0 (null data)     */

/* USB PIDs */
#define USB_PID_SETUP   0x2D
#define USB_PID_IN      0x69
#define USB_PID_OUT     0xE1

/* ── Queue Head (QH) ─────────────────────────────────────────────────────── */
/*
 * Each QH is 16 bytes, 16-byte aligned.
 *
 * qh_link:    horizontal link pointer (next QH in frame list)
 * qh_element: vertical link pointer (first TD in this QH)
 */

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t  link;         /* horizontal link (next QH/TD)              */
    uint32_t  element;      /* vertical link (first TD in queue)         */
    /* Software fields */
    uint32_t  sw_pad[2];
} uhci_qh_t;

/* ── UHCI controller state ───────────────────────────────────────────────── */

#define UHCI_MAX_PORTS  2

typedef struct {
    uint16_t    iobase;                     /* I/O base address from BAR4  */
    pci_addr_t  pci;                        /* PCI location                */
    uint32_t   *frame_list;                 /* virtual addr of frame list  */
    uint8_t     port_count;                 /* number of ports (1–2)       */
    bool        port_connected[UHCI_MAX_PORTS]; /* device present?         */
    uint8_t     port_speed[UHCI_MAX_PORTS]; /* 0=full, 1=low              */
} uhci_ctrl_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * uhci_init — find the UHCI controller on PCI, initialise it, reset all
 * ports, and enumerate connected devices.
 * Returns true on success.
 */
bool uhci_init(void);

/*
 * uhci_get — return a pointer to the global controller state.
 * Returns NULL if uhci_init() has not been called or failed.
 */
uhci_ctrl_t *uhci_get(void);

/*
 * uhci_control_transfer — perform a USB control transfer (SETUP + optional
 * DATA + STATUS) on the given device/endpoint.
 *
 * setup_pkt: 8-byte USB setup packet
 * data:      data buffer (IN or OUT), or NULL for zero-length transfers
 * data_len:  length of data buffer
 * is_in:     true = IN transfer (device → host), false = OUT
 * dev_addr:  USB device address (0–127)
 * low_speed: true if the device is low-speed
 *
 * Returns true on success (STATUS ACK received).
 */
bool uhci_control_transfer(uint8_t dev_addr, bool low_speed,
                           const uint8_t *setup_pkt,
                           uint8_t *data, uint16_t data_len,
                           bool is_in);

/*
 * uhci_alloc_td — allocate a Transfer Descriptor from the heap.
 * Returns NULL on allocation failure.
 */
uhci_td_t *uhci_alloc_td(void);

/*
 * uhci_alloc_qh — allocate a Queue Head from the heap.
 */
uhci_qh_t *uhci_alloc_qh(void);

/*
 * uhci_insert_periodic_td — insert a TD into every Nth frame list entry
 * for periodic (interrupt) transfers.  interval is the polling interval
 * in frames (1–255).
 */
void uhci_insert_periodic_td(uhci_td_t *td, uint8_t interval);

/*
 * uhci_remove_periodic_td — remove a TD from the frame list.
 */
void uhci_remove_periodic_td(uhci_td_t *td);

/*
 * uhci_td_done — return true if the TD has completed (ACTIVE bit clear).
 */
bool uhci_td_done(const uhci_td_t *td);

/*
 * uhci_td_error — return true if the TD completed with an error.
 */
bool uhci_td_error(const uhci_td_t *td);

/*
 * uhci_td_actual_len — return the actual number of bytes transferred.
 * Valid only after uhci_td_done() returns true.
 */
uint16_t uhci_td_actual_len(const uhci_td_t *td);

/*
 * uhci_reactivate_td — mark a completed TD as ACTIVE again for re-use
 * in periodic polling.
 */
void uhci_reactivate_td(uhci_td_t *td);

#endif /* UHCI_H */
