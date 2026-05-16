/*
 * ChimaeraOS — HID Input Layer + USB HID Init
 * src/drivers/usb/hid.c
 *
 * Implements the global input ring buffer and the USB HID initialisation
 * entry point that enumerates all connected devices and sets up keyboard
 * and mouse drivers.
 */

#include "hid.h"
#include "uhci.h"
#include "usb_core.h"
#include "../../include/types.h"
#include "../../include/serial.h"
#include "../../include/mm.h"

/* ── Global input ring ───────────────────────────────────────────────────── */

input_ring_t g_input_ring;

void input_init(void)
{
    g_input_ring.head = 0;
    g_input_ring.tail = 0;
}

void input_ring_init(input_ring_t *ring)
{
    ring->head = 0;
    ring->tail = 0;
}

bool input_ring_post(input_ring_t *ring, const input_event_t *ev)
{
    uint32_t next = (ring->head + 1) & (INPUT_RING_SIZE - 1);
    if (next == ring->tail) return false;  /* ring full, drop event */
    ring->events[ring->head] = *ev;
    ring->head = next;
    return true;
}

bool input_ring_read(input_ring_t *ring, input_event_t *ev)
{
    if (ring->tail == ring->head) return false;  /* empty */
    *ev = ring->events[ring->tail];
    ring->tail = (ring->tail + 1) & (INPUT_RING_SIZE - 1);
    return true;
}

bool input_ring_empty(const input_ring_t *ring)
{
    return ring->tail == ring->head;
}

/* ── Device table ────────────────────────────────────────────────────────── */

#define USB_MAX_DEVICES  4

static usb_device_t g_devices[USB_MAX_DEVICES];
static uint32_t     g_device_count = 0;

/* ── usb_hid_init ────────────────────────────────────────────────────────── */

uint32_t usb_hid_init(void)
{
    uhci_ctrl_t *ctrl = uhci_get();
    if (!ctrl) {
        serial_puts("[HID] No UHCI controller available\r\n");
        return 0;
    }

    uint32_t hid_count = 0;

    for (uint8_t p = 0; p < ctrl->port_count && p < UHCI_MAX_PORTS; p++) {
        if (!ctrl->port_connected[p]) continue;
        if (g_device_count >= USB_MAX_DEVICES) break;

        usb_device_t *dev = &g_devices[g_device_count];

        serial_puts("[HID] Enumerating port ");
        serial_dec(p + 1);
        serial_puts("...\r\n");

        if (!usb_enumerate_port(p, ctrl->port_speed[p] == 1, dev)) {
            serial_puts("[HID] Enumeration failed on port ");
            serial_dec(p + 1);
            serial_puts("\r\n");
            continue;
        }

        g_device_count++;

        /* Identify device type */
        uint8_t cls = dev->class_code;
        uint8_t sub = dev->subclass;
        uint8_t pro = dev->protocol;

        if (cls == USB_CLASS_HID && sub == USB_SUBCLASS_BOOT) {
            if (pro == USB_PROTOCOL_KEYBOARD) {
                serial_puts("[HID] Keyboard detected\r\n");
                if (hid_kbd_init(dev)) hid_count++;
            } else if (pro == USB_PROTOCOL_MOUSE) {
                serial_puts("[HID] Mouse detected\r\n");
                if (hid_mouse_init(dev)) hid_count++;
            } else {
                serial_puts("[HID] Unknown HID boot device (protocol=");
                serial_hex8(pro);
                serial_puts(")\r\n");
            }
        } else {
            serial_puts("[HID] Non-HID device (class=");
            serial_hex8(cls);
            serial_puts("), skipping\r\n");
        }
    }

    serial_puts("[HID] Initialised ");
    serial_dec(hid_count);
    serial_puts(" HID device(s)\r\n");
    return hid_count;
}

/* ── usb_hid_poll ────────────────────────────────────────────────────────── */

void usb_hid_poll(void)
{
    hid_kbd_poll();
    hid_mouse_poll();
}
