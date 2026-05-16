/*
 * ChimaeraOS — HID Boot-Protocol Keyboard Driver
 * src/drivers/usb/hid_kbd.c
 *
 * Implements the HID boot-protocol keyboard driver:
 *   1. SET_PROTOCOL(boot) — switch device to 8-byte boot report format
 *   2. SET_IDLE(0)        — report only on change
 *   3. Allocate an 8-byte DMA buffer and a UHCI interrupt TD
 *   4. Insert TD into the UHCI frame list at the device's polling interval
 *   5. On each poll, check if the TD completed; if so, decode the report
 *      and post key events to the global input ring
 *
 * Boot-protocol keyboard report format (8 bytes):
 *   byte 0: modifier keys bitmask
 *           bit 0: Left Ctrl
 *           bit 1: Left Shift
 *           bit 2: Left Alt
 *           bit 3: Left GUI
 *           bit 4: Right Ctrl
 *           bit 5: Right Shift
 *           bit 6: Right Alt
 *           bit 7: Right GUI
 *   byte 1: reserved (always 0)
 *   bytes 2–7: up to 6 simultaneous key codes (USB HID usage IDs)
 *              0x00 = no key pressed in this slot
 *              0x01 = error rollover
 *
 * USB HID usage IDs for keyboard (boot protocol subset):
 *   0x04–0x1D: a–z
 *   0x1E–0x27: 1–9, 0
 *   0x28: Enter
 *   0x29: Escape
 *   0x2A: Backspace
 *   0x2B: Tab
 *   0x2C: Space
 *   0x2D: - _
 *   0x2E: = +
 *   0x2F: [ {
 *   0x30: ] }
 *   0x31: \ |
 *   0x33: ; :
 *   0x34: ' "
 *   0x35: ` ~
 *   0x36: , <
 *   0x37: . >
 *   0x38: / ?
 *   0x39: Caps Lock
 *   0x3A–0x45: F1–F12
 *   0x4F–0x52: Right, Left, Down, Up arrows
 */

#include "hid.h"
#include "uhci.h"
#include "usb_core.h"
#include "../../include/types.h"
#include "../../include/serial.h"
#include "../../include/mm.h"

/* ── Scancode → ASCII translation table ─────────────────────────────────── */
/*
 * Index = USB HID usage ID.
 * Value = ASCII character (unshifted), 0 = non-printable.
 * Covers usage IDs 0x00–0x64.
 */
static const uint8_t hid_to_ascii_unshifted[0x65] = {
    /* 0x00 */ 0,    /* no event        */
    /* 0x01 */ 0,    /* error rollover  */
    /* 0x02 */ 0,    /* POST fail       */
    /* 0x03 */ 0,    /* error undefined */
    /* 0x04 */ 'a',  /* a               */
    /* 0x05 */ 'b',
    /* 0x06 */ 'c',
    /* 0x07 */ 'd',
    /* 0x08 */ 'e',
    /* 0x09 */ 'f',
    /* 0x0A */ 'g',
    /* 0x0B */ 'h',
    /* 0x0C */ 'i',
    /* 0x0D */ 'j',
    /* 0x0E */ 'k',
    /* 0x0F */ 'l',
    /* 0x10 */ 'm',
    /* 0x11 */ 'n',
    /* 0x12 */ 'o',
    /* 0x13 */ 'p',
    /* 0x14 */ 'q',
    /* 0x15 */ 'r',
    /* 0x16 */ 's',
    /* 0x17 */ 't',
    /* 0x18 */ 'u',
    /* 0x19 */ 'v',
    /* 0x1A */ 'w',
    /* 0x1B */ 'x',
    /* 0x1C */ 'y',
    /* 0x1D */ 'z',
    /* 0x1E */ '1',
    /* 0x1F */ '2',
    /* 0x20 */ '3',
    /* 0x21 */ '4',
    /* 0x22 */ '5',
    /* 0x23 */ '6',
    /* 0x24 */ '7',
    /* 0x25 */ '8',
    /* 0x26 */ '9',
    /* 0x27 */ '0',
    /* 0x28 */ '\r', /* Enter           */
    /* 0x29 */ 0x1B, /* Escape          */
    /* 0x2A */ '\b', /* Backspace       */
    /* 0x2B */ '\t', /* Tab             */
    /* 0x2C */ ' ',  /* Space           */
    /* 0x2D */ '-',
    /* 0x2E */ '=',
    /* 0x2F */ '[',
    /* 0x30 */ ']',
    /* 0x31 */ '\\',
    /* 0x32 */ 0,    /* non-US #        */
    /* 0x33 */ ';',
    /* 0x34 */ '\'',
    /* 0x35 */ '`',
    /* 0x36 */ ',',
    /* 0x37 */ '.',
    /* 0x38 */ '/',
    /* 0x39 */ 0,    /* Caps Lock       */
    /* 0x3A */ 0,    /* F1              */
    /* 0x3B */ 0,    /* F2              */
    /* 0x3C */ 0,    /* F3              */
    /* 0x3D */ 0,    /* F4              */
    /* 0x3E */ 0,    /* F5              */
    /* 0x3F */ 0,    /* F6              */
    /* 0x40 */ 0,    /* F7              */
    /* 0x41 */ 0,    /* F8              */
    /* 0x42 */ 0,    /* F9              */
    /* 0x43 */ 0,    /* F10             */
    /* 0x44 */ 0,    /* F11             */
    /* 0x45 */ 0,    /* F12             */
    /* 0x46 */ 0,    /* Print Screen    */
    /* 0x47 */ 0,    /* Scroll Lock     */
    /* 0x48 */ 0,    /* Pause           */
    /* 0x49 */ 0,    /* Insert          */
    /* 0x4A */ 0,    /* Home            */
    /* 0x4B */ 0,    /* Page Up         */
    /* 0x4C */ 0x7F, /* Delete          */
    /* 0x4D */ 0,    /* End             */
    /* 0x4E */ 0,    /* Page Down       */
    /* 0x4F */ 0,    /* Right Arrow     */
    /* 0x50 */ 0,    /* Left Arrow      */
    /* 0x51 */ 0,    /* Down Arrow      */
    /* 0x52 */ 0,    /* Up Arrow        */
    /* 0x53 */ 0,    /* Num Lock        */
    /* 0x54 */ '/',  /* KP /            */
    /* 0x55 */ '*',  /* KP *            */
    /* 0x56 */ '-',  /* KP -            */
    /* 0x57 */ '+',  /* KP +            */
    /* 0x58 */ '\r', /* KP Enter        */
    /* 0x59 */ '1',  /* KP 1            */
    /* 0x5A */ '2',  /* KP 2            */
    /* 0x5B */ '3',  /* KP 3            */
    /* 0x5C */ '4',  /* KP 4            */
    /* 0x5D */ '5',  /* KP 5            */
    /* 0x5E */ '6',  /* KP 6            */
    /* 0x5F */ '7',  /* KP 7            */
    /* 0x60 */ '8',  /* KP 8            */
    /* 0x61 */ '9',  /* KP 9            */
    /* 0x62 */ '0',  /* KP 0            */
    /* 0x63 */ '.',  /* KP .            */
    /* 0x64 */ 0,    /* non-US \        */
};

static const uint8_t hid_to_ascii_shifted[0x65] = {
    /* 0x00 */ 0,
    /* 0x01 */ 0,
    /* 0x02 */ 0,
    /* 0x03 */ 0,
    /* 0x04 */ 'A',
    /* 0x05 */ 'B',
    /* 0x06 */ 'C',
    /* 0x07 */ 'D',
    /* 0x08 */ 'E',
    /* 0x09 */ 'F',
    /* 0x0A */ 'G',
    /* 0x0B */ 'H',
    /* 0x0C */ 'I',
    /* 0x0D */ 'J',
    /* 0x0E */ 'K',
    /* 0x0F */ 'L',
    /* 0x10 */ 'M',
    /* 0x11 */ 'N',
    /* 0x12 */ 'O',
    /* 0x13 */ 'P',
    /* 0x14 */ 'Q',
    /* 0x15 */ 'R',
    /* 0x16 */ 'S',
    /* 0x17 */ 'T',
    /* 0x18 */ 'U',
    /* 0x19 */ 'V',
    /* 0x1A */ 'W',
    /* 0x1B */ 'X',
    /* 0x1C */ 'Y',
    /* 0x1D */ 'Z',
    /* 0x1E */ '!',
    /* 0x1F */ '@',
    /* 0x20 */ '#',
    /* 0x21 */ '$',
    /* 0x22 */ '%',
    /* 0x23 */ '^',
    /* 0x24 */ '&',
    /* 0x25 */ '*',
    /* 0x26 */ '(',
    /* 0x27 */ ')',
    /* 0x28 */ '\r',
    /* 0x29 */ 0x1B,
    /* 0x2A */ '\b',
    /* 0x2B */ '\t',
    /* 0x2C */ ' ',
    /* 0x2D */ '_',
    /* 0x2E */ '+',
    /* 0x2F */ '{',
    /* 0x30 */ '}',
    /* 0x31 */ '|',
    /* 0x32 */ 0,
    /* 0x33 */ ':',
    /* 0x34 */ '"',
    /* 0x35 */ '~',
    /* 0x36 */ '<',
    /* 0x37 */ '>',
    /* 0x38 */ '?',
    /* 0x39 */ 0,
    /* 0x3A–0x64: same as unshifted for non-alpha */
    0,0,0,0,0,0,0,0,0,0,0,0,  /* F1–F12, Print, Scroll, Pause */
    0,0,0,0,0x7F,0,0,          /* Insert, Home, PgUp, Del, End, PgDn */
    0,0,0,0,                   /* arrows */
    0,'/','*','-','+','\r',    /* numpad */
    '1','2','3','4','5','6','7','8','9','0','.',0,
};

/* ── Keyboard state ──────────────────────────────────────────────────────── */

#define KBD_REPORT_SIZE  8

typedef struct {
    usb_device_t *dev;
    uhci_td_t    *poll_td;
    uint8_t      *report_buf;   /* 8-byte DMA buffer                       */
    uint8_t       prev_report[KBD_REPORT_SIZE]; /* previous report         */
    bool          active;
} hid_kbd_state_t;

static hid_kbd_state_t g_kbd;

/* ── hid_kbd_init ────────────────────────────────────────────────────────── */

bool hid_kbd_init(usb_device_t *dev)
{
    if (!dev->int_ep_valid) {
        serial_puts("[KBD] No interrupt IN endpoint found\r\n");
        return false;
    }

    /* SET_PROTOCOL(boot) */
    if (!usb_hid_set_protocol(dev, 0, HID_PROTOCOL_BOOT)) {
        serial_puts("[KBD] SET_PROTOCOL(boot) failed\r\n");
        /* Non-fatal: some devices ignore this */
    }

    /* SET_IDLE(0) — report only on change */
    usb_hid_set_idle(dev, 0, 0);

    /* Allocate report buffer (must be stable physical address) */
    uint8_t *buf = (uint8_t *)kmalloc(KBD_REPORT_SIZE);
    if (!buf) return false;
    for (int i = 0; i < KBD_REPORT_SIZE; i++) buf[i] = 0;

    /* Allocate and initialise polling TD */
    uhci_td_t *td = uhci_alloc_td();
    if (!td) return false;

    uint32_t ls_bit = dev->low_speed ? TD_STS_LS : 0;

    td->status = TD_STS_ACTIVE | TD_STS_ERRCNT(3) | TD_STS_IOC | ls_bit;
    td->token  = TD_TOKEN_PID(USB_PID_IN)
               | TD_TOKEN_DEVADDR(dev->addr)
               | TD_TOKEN_ENDPT(dev->int_ep_addr)
               | TD_TOKEN_TOGGLE(0)
               | TD_TOKEN_MAXLEN(KBD_REPORT_SIZE);
    td->buffer     = (uint32_t)buf;
    td->sw_buf_virt = (uint32_t)buf;
    td->link       = UHCI_LP_TERMINATE;

    /* Insert into frame list */
    uint8_t interval = dev->int_ep_interval;
    if (interval == 0) interval = 8;  /* default 8 ms */
    uhci_insert_periodic_td(td, interval);

    g_kbd.dev         = dev;
    g_kbd.poll_td     = td;
    g_kbd.report_buf  = buf;
    g_kbd.active      = true;

    for (int i = 0; i < KBD_REPORT_SIZE; i++) g_kbd.prev_report[i] = 0;

    serial_puts("[KBD] Boot-protocol keyboard ready (addr ");
    serial_dec(dev->addr);
    serial_puts(")\r\n");
    return true;
}

/* ── hid_kbd_debug_status ───────────────────────────────────────────────── */

void hid_kbd_debug_status(void)
{
    if (!g_kbd.active) {
        serial_puts("[KBD] not active\r\n");
        return;
    }
    uhci_td_t *td = g_kbd.poll_td;
    serial_puts("[KBD] poll_td status=0x");
    serial_hex32(td->status);
    serial_puts(" active=");
    serial_puts(uhci_td_done(td) ? "0" : "1");
    serial_puts("\r\n");
}

/* ── hid_kbd_poll ────────────────────────────────────────────────────────── */

static uint32_t g_poll_count = 0;
static uint32_t g_done_count = 0;

void hid_kbd_poll(void)
{
    if (!g_kbd.active) return;

    uhci_td_t *td = g_kbd.poll_td;
    g_poll_count++;

    /* Every 500 polls, print status for debugging */
    if ((g_poll_count % 500) == 0) {
        serial_puts("[KBD] poll=");
        serial_dec(g_poll_count);
        serial_puts(" done=");
        serial_dec(g_done_count);
        serial_puts(" td_sts=0x");
        serial_hex32(td->status);
        serial_puts(" tok=0x");
        serial_hex32(td->token);
        serial_puts("\r\n");
    }

    if (!uhci_td_done(td)) return;
    g_done_count++;

    /* Debug: print TD status on every completion */
    serial_puts("[KBD] TD#");
    serial_dec(g_done_count);
    serial_puts(" sts=0x");
    serial_hex32(td->status);
    serial_puts(" len=");
    serial_dec(uhci_td_actual_len(td));
    serial_puts(" tok=0x");
    serial_hex32(td->token);
    serial_puts("\r\n");

    /* Check for errors */
    if (uhci_td_error(td)) {
        serial_puts("[KBD] TD error bits=0x");
        serial_hex32(td->status & 0x007E0000);
        serial_puts("\r\n");
        uhci_reactivate_td(td);
        return;
    }

    uint8_t *cur  = g_kbd.report_buf;
    uint8_t *prev = g_kbd.prev_report;

    uint8_t cur_mods  = cur[0];
    bool    shifted   = !!(cur_mods & 0x22);  /* Left or Right Shift */

    /* ── Detect key releases ─────────────────────────────────────────────── */
    for (int i = 2; i < 8; i++) {
        uint8_t sc = prev[i];
        if (sc == 0 || sc == 0x01) continue;

        /* Check if this key is still held */
        bool still_held = false;
        for (int j = 2; j < 8; j++) {
            if (cur[j] == sc) { still_held = true; break; }
        }

        if (!still_held) {
            input_event_t ev;
            ev.type          = INPUT_TYPE_KEY;
            ev.key.scancode  = sc;
            ev.key.ascii     = (sc < 0x65) ? hid_to_ascii_unshifted[sc] : 0;
            ev.key.modifiers = prev[0];
            ev.key.flags     = KEY_FLAG_RELEASE;
            input_ring_post(&g_input_ring, &ev);
        }
    }

    /* ── Detect key presses ──────────────────────────────────────────────── */
    for (int i = 2; i < 8; i++) {
        uint8_t sc = cur[i];
        if (sc == 0 || sc == 0x01) continue;

        /* Check if this is a new key (not in previous report) */
        bool was_held = false;
        for (int j = 2; j < 8; j++) {
            if (prev[j] == sc) { was_held = true; break; }
        }

        if (!was_held) {
            uint8_t ascii = 0;
            if (sc < 0x65) {
                ascii = shifted ? hid_to_ascii_shifted[sc]
                                : hid_to_ascii_unshifted[sc];
            }

            input_event_t ev;
            ev.type          = INPUT_TYPE_KEY;
            ev.key.scancode  = sc;
            ev.key.ascii     = ascii;
            ev.key.modifiers = cur_mods;
            ev.key.flags     = KEY_FLAG_PRESS;
            input_ring_post(&g_input_ring, &ev);

            /* Echo to serial for debugging */
            if (ascii >= 0x20 && ascii < 0x7F) {
                char ch[2] = { (char)ascii, 0 };
                serial_puts("[KBD] key='");
                serial_puts(ch);
                serial_puts("' sc=0x");
                serial_hex8(sc);
                serial_puts("\r\n");
            } else if (ascii != 0) {
                serial_puts("[KBD] key=0x");
                serial_hex8(ascii);
                serial_puts(" sc=0x");
                serial_hex8(sc);
                serial_puts("\r\n");
            } else {
                serial_puts("[KBD] key=<special> sc=0x");
                serial_hex8(sc);
                serial_puts("\r\n");
            }
        }
    }

    /* Save current report as previous */
    for (int i = 0; i < KBD_REPORT_SIZE; i++) prev[i] = cur[i];

    /* Reactivate TD for next poll */
    uhci_reactivate_td(td);
}
