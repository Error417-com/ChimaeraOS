/*
 * ChimaeraOS — HID Boot-Protocol Mouse Driver
 * src/drivers/usb/hid_mouse.c
 *
 * Boot-protocol mouse report format (3 bytes minimum):
 *   byte 0: button bitmask
 *           bit 0: left button
 *           bit 1: right button
 *           bit 2: middle button
 *   byte 1: X displacement (signed, relative)
 *   byte 2: Y displacement (signed, relative)
 *
 * Some mice send 4 bytes (byte 3 = scroll wheel), but we ignore byte 3.
 */

#include "hid.h"
#include "uhci.h"
#include "usb_core.h"
#include "../../include/types.h"
#include "../../include/serial.h"
#include "../../include/mm.h"

/* ── Mouse state ─────────────────────────────────────────────────────────── */

#define MOUSE_REPORT_SIZE  4   /* read 4 bytes to accommodate scroll wheel */

typedef struct {
    usb_device_t *dev;
    uhci_td_t    *poll_td;
    uint8_t      *report_buf;
    bool          active;
} hid_mouse_state_t;

static hid_mouse_state_t g_mouse;

/* ── hid_mouse_init ──────────────────────────────────────────────────────── */

bool hid_mouse_init(usb_device_t *dev)
{
    if (!dev->int_ep_valid) {
        serial_puts("[MOUSE] No interrupt IN endpoint found\r\n");
        return false;
    }

    /* SET_PROTOCOL(boot) */
    if (!usb_hid_set_protocol(dev, 0, HID_PROTOCOL_BOOT)) {
        serial_puts("[MOUSE] SET_PROTOCOL(boot) failed (non-fatal)\r\n");
    }

    /* SET_IDLE(0) */
    usb_hid_set_idle(dev, 0, 0);

    /* Allocate report buffer */
    uint8_t *buf = (uint8_t *)kmalloc(MOUSE_REPORT_SIZE);
    if (!buf) return false;
    for (int i = 0; i < MOUSE_REPORT_SIZE; i++) buf[i] = 0;

    /* Allocate and initialise polling TD */
    uhci_td_t *td = uhci_alloc_td();
    if (!td) return false;

    uint32_t ls_bit = dev->low_speed ? TD_STS_LS : 0;
    uint16_t maxpkt = dev->int_ep_maxpkt;
    if (maxpkt == 0 || maxpkt > MOUSE_REPORT_SIZE)
        maxpkt = MOUSE_REPORT_SIZE;

    td->status = TD_STS_ACTIVE | TD_STS_ERRCNT(3) | TD_STS_IOC | ls_bit;
    td->token  = TD_TOKEN_PID(USB_PID_IN)
               | TD_TOKEN_DEVADDR(dev->addr)
               | TD_TOKEN_ENDPT(dev->int_ep_addr)
               | TD_TOKEN_TOGGLE(0)
               | TD_TOKEN_MAXLEN(maxpkt);
    td->buffer      = (uint32_t)buf;
    td->sw_buf_virt = (uint32_t)buf;
    td->link        = UHCI_LP_TERMINATE;

    uint8_t interval = dev->int_ep_interval;
    if (interval == 0) interval = 8;
    uhci_insert_periodic_td(td, interval);

    g_mouse.dev        = dev;
    g_mouse.poll_td    = td;
    g_mouse.report_buf = buf;
    g_mouse.active     = true;

    serial_puts("[MOUSE] Boot-protocol mouse ready (addr ");
    serial_dec(dev->addr);
    serial_puts(")\r\n");
    return true;
}

/* ── hid_mouse_poll ──────────────────────────────────────────────────────── */

void hid_mouse_poll(void)
{
    if (!g_mouse.active) return;

    uhci_td_t *td = g_mouse.poll_td;
    if (!uhci_td_done(td)) return;

    if (uhci_td_error(td)) {
        uhci_reactivate_td(td);
        return;
    }

    uint8_t *r = g_mouse.report_buf;

    uint8_t buttons = r[0] & 0x07;
    int8_t  dx      = (int8_t)r[1];
    int8_t  dy      = (int8_t)r[2];

    /* Only post event if something changed */
    if (dx != 0 || dy != 0 || buttons != 0) {
        input_event_t ev;
        ev.type         = INPUT_TYPE_MOUSE;
        ev.mouse.dx      = dx;
        ev.mouse.dy      = dy;
        ev.mouse.buttons = buttons;
        input_ring_post(&g_input_ring, &ev);

        /* Debug output */
        if (dx != 0 || dy != 0) {
            serial_puts("[MOUSE] dx=");
            if (dx < 0) { serial_puts("-"); serial_dec((uint32_t)(-dx)); }
            else         { serial_dec((uint32_t)dx); }
            serial_puts(" dy=");
            if (dy < 0) { serial_puts("-"); serial_dec((uint32_t)(-dy)); }
            else         { serial_dec((uint32_t)dy); }
            serial_puts("\r\n");
        }
        if (buttons) {
            serial_puts("[MOUSE] buttons=0x");
            serial_hex8(buttons);
            serial_puts("\r\n");
        }
    }

    uhci_reactivate_td(td);
}
