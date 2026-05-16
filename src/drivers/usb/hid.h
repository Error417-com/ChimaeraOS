/*
 * ChimaeraOS — HID Input Layer
 * src/drivers/usb/hid.h
 *
 * Minimal input event types and ring buffer for HID keyboard and mouse.
 * The ring buffer is written by USB polling (called from the scheduler
 * tick or a dedicated polling task) and read by application code.
 */

#ifndef HID_H
#define HID_H

#include "../../include/types.h"
#include "usb_core.h"

/* ── Input event types ───────────────────────────────────────────────────── */

#define INPUT_TYPE_KEY      1   /* keyboard key press/release              */
#define INPUT_TYPE_MOUSE    2   /* mouse movement + button state           */

/* Key event flags */
#define KEY_FLAG_PRESS      0   /* key pressed                             */
#define KEY_FLAG_RELEASE    1   /* key released                            */

typedef struct {
    uint8_t  type;          /* INPUT_TYPE_KEY or INPUT_TYPE_MOUSE          */
    union {
        struct {
            uint8_t  scancode;  /* USB HID usage ID (boot protocol)        */
            uint8_t  ascii;     /* ASCII translation, 0 if not printable   */
            uint8_t  modifiers; /* modifier byte from boot report          */
            uint8_t  flags;     /* KEY_FLAG_PRESS / KEY_FLAG_RELEASE       */
        } key;
        struct {
            int8_t   dx;        /* relative X movement                     */
            int8_t   dy;        /* relative Y movement                     */
            uint8_t  buttons;   /* button bitmask (bit 0=left, 1=right, 2=mid) */
        } mouse;
    };
} input_event_t;

/* ── Ring buffer ─────────────────────────────────────────────────────────── */

#define INPUT_RING_SIZE  64     /* must be a power of 2                    */

typedef struct {
    input_event_t events[INPUT_RING_SIZE];
    uint32_t      head;         /* write index                             */
    uint32_t      tail;         /* read index                              */
} input_ring_t;

void input_ring_init(input_ring_t *ring);
bool input_ring_post(input_ring_t *ring, const input_event_t *ev);
bool input_ring_read(input_ring_t *ring, input_event_t *ev);
bool input_ring_empty(const input_ring_t *ring);

/* ── Global input ring ───────────────────────────────────────────────────── */

extern input_ring_t g_input_ring;

void input_init(void);

/* ── HID keyboard driver ─────────────────────────────────────────────────── */

/*
 * hid_kbd_init — initialise a HID boot-protocol keyboard.
 * Sends SET_PROTOCOL(boot), SET_IDLE(0), allocates a polling TD,
 * and inserts it into the UHCI frame list.
 * Returns true on success.
 */
bool hid_kbd_init(usb_device_t *dev);

/*
 * hid_kbd_poll — check the polling TD; if a new report has arrived,
 * decode it and post key events to g_input_ring.
 * Call this from the scheduler tick or a polling task.
 */
void hid_kbd_poll(void);
void hid_kbd_debug_status(void);

/* ── HID mouse driver ────────────────────────────────────────────────────── */

/*
 * hid_mouse_init — initialise a HID boot-protocol mouse.
 * Same pattern as hid_kbd_init.
 */
bool hid_mouse_init(usb_device_t *dev);

/*
 * hid_mouse_poll — check the polling TD; if a new report has arrived,
 * decode it and post a mouse event to g_input_ring.
 */
void hid_mouse_poll(void);

/* ── USB HID initialisation entry point ─────────────────────────────────── */

/*
 * usb_hid_init — called after uhci_init().  Enumerates all connected
 * devices, identifies keyboards and mice, and sets them up for polling.
 * Returns the number of HID devices successfully initialised.
 */
uint32_t usb_hid_init(void);

/*
 * usb_hid_poll — poll all active HID devices.  Call this periodically
 * (e.g., from the scheduler tick handler or a dedicated task).
 */
void usb_hid_poll(void);

#endif /* HID_H */
