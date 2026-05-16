/*
 * ChimaeraOS — USB Core Layer
 * src/drivers/usb/usb_core.c
 *
 * USB device enumeration: SET_ADDRESS, GET_DESCRIPTOR (device + config),
 * SET_CONFIGURATION, HID SET_PROTOCOL, HID SET_IDLE.
 *
 * Address assignment: we use a simple counter starting at 1.
 * The first device gets address 1, the second gets address 2, etc.
 */

#include "usb_core.h"
#include "uhci.h"
#include "../../include/types.h"
#include "../../include/serial.h"
#include "../../include/mm.h"
#include "../../include/timer.h"

/* ── Address counter ─────────────────────────────────────────────────────── */

static uint8_t g_next_addr = 1;

/* ── Setup packet helpers ────────────────────────────────────────────────── */

static void make_setup(usb_setup_t *s, uint8_t type, uint8_t req,
                       uint16_t value, uint16_t index, uint16_t len)
{
    s->bmRequestType = type;
    s->bRequest      = req;
    s->wValue        = value;
    s->wIndex        = index;
    s->wLength       = len;
}

/* ── usb_enumerate_port ──────────────────────────────────────────────────── */

bool usb_enumerate_port(uint8_t port, bool low_speed, usb_device_t *dev)
{
    usb_setup_t setup;
    uint8_t     buf[256];

    /* Zero the device record */
    uint8_t *dp = (uint8_t *)dev;
    for (uint32_t i = 0; i < sizeof(usb_device_t); i++) dp[i] = 0;

    dev->port      = port;
    dev->low_speed = low_speed;
    dev->addr      = 0;   /* default address before SET_ADDRESS */

    /* ── Step 1: GET_DESCRIPTOR(Device, 8 bytes) at address 0 ──────────── */
    /* Get just the first 8 bytes to find bMaxPacketSize0 */

    make_setup(&setup,
               USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
               USB_REQ_GET_DESCRIPTOR,
               (uint16_t)(USB_DESC_DEVICE << 8),
               0, 8);

    if (!uhci_control_transfer(0, low_speed, (uint8_t *)&setup, buf, 8, true)) {
        serial_puts("[USB] GET_DESCRIPTOR(8) failed at addr 0\r\n");
        return false;
    }

    dev->max_packet0 = buf[7];  /* bMaxPacketSize0 */
    serial_puts("[USB] bMaxPacketSize0=");
    serial_dec(dev->max_packet0);
    serial_puts("\r\n");

    /* ── Step 2: SET_ADDRESS ────────────────────────────────────────────── */

    uint8_t new_addr = g_next_addr++;

    make_setup(&setup,
               USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
               USB_REQ_SET_ADDRESS,
               new_addr, 0, 0);

    if (!uhci_control_transfer(0, low_speed, (uint8_t *)&setup, NULL, 0, false)) {
        serial_puts("[USB] SET_ADDRESS failed\r\n");
        return false;
    }

    /* USB spec: device needs 2 ms to process SET_ADDRESS */
    uint32_t end = timer_ms() + 2;
    while (timer_ms() < end) __asm__ volatile ("pause");

    dev->addr = new_addr;
    serial_puts("[USB] Assigned address ");
    serial_dec(new_addr);
    serial_puts("\r\n");

    /* ── Step 3: GET_DESCRIPTOR(Device, 18 bytes) at new address ────────── */

    make_setup(&setup,
               USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
               USB_REQ_GET_DESCRIPTOR,
               (uint16_t)(USB_DESC_DEVICE << 8),
               0, 18);

    if (!uhci_control_transfer(new_addr, low_speed, (uint8_t *)&setup,
                               buf, 18, true)) {
        serial_puts("[USB] GET_DESCRIPTOR(Device) failed\r\n");
        return false;
    }

    usb_device_desc_t *dd = (usb_device_desc_t *)buf;
    dev->class_code = dd->bDeviceClass;
    dev->subclass   = dd->bDeviceSubClass;
    dev->protocol   = dd->bDeviceProtocol;

    serial_puts("[USB] VID="); serial_hex16(dd->idVendor);
    serial_puts(" PID=");      serial_hex16(dd->idProduct);
    serial_puts(" class=");    serial_hex8(dd->bDeviceClass);
    serial_puts("/");          serial_hex8(dd->bDeviceSubClass);
    serial_puts("/");          serial_hex8(dd->bDeviceProtocol);
    serial_puts("\r\n");

    /* ── Step 4: GET_DESCRIPTOR(Config, 9 bytes) ─────────────────────────── */

    make_setup(&setup,
               USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
               USB_REQ_GET_DESCRIPTOR,
               (uint16_t)(USB_DESC_CONFIG << 8),
               0, 9);

    if (!uhci_control_transfer(new_addr, low_speed, (uint8_t *)&setup,
                               buf, 9, true)) {
        serial_puts("[USB] GET_DESCRIPTOR(Config,9) failed\r\n");
        return false;
    }

    usb_config_desc_t *cd = (usb_config_desc_t *)buf;
    uint16_t total_len = cd->wTotalLength;
    uint8_t  config_val = cd->bConfigurationValue;

    if (total_len > 255) total_len = 255;

    /* ── Step 5: GET_DESCRIPTOR(Config, full) ────────────────────────────── */

    make_setup(&setup,
               USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
               USB_REQ_GET_DESCRIPTOR,
               (uint16_t)(USB_DESC_CONFIG << 8),
               0, total_len);

    if (!uhci_control_transfer(new_addr, low_speed, (uint8_t *)&setup,
                               buf, total_len, true)) {
        serial_puts("[USB] GET_DESCRIPTOR(Config,full) failed\r\n");
        return false;
    }

    /* ── Step 6: Parse interface and endpoint descriptors ────────────────── */

    uint8_t *p        = buf;
    uint8_t *desc_end  = buf + total_len;

    while (p < desc_end && (p + 2) <= desc_end) {
        uint8_t len  = p[0];
        uint8_t type = p[1];

        if (len < 2 || p + len > desc_end) break;

        if (type == USB_DESC_INTERFACE && len >= 9) {
            usb_iface_desc_t *id = (usb_iface_desc_t *)p;
            /* If device class is 0 (composite), use interface class */
            if (dev->class_code == 0) {
                dev->class_code = id->bInterfaceClass;
                dev->subclass   = id->bInterfaceSubClass;
                dev->protocol   = id->bInterfaceProtocol;
            }
            serial_puts("[USB] Interface class=");
            serial_hex8(id->bInterfaceClass);
            serial_puts("/"); serial_hex8(id->bInterfaceSubClass);
            serial_puts("/"); serial_hex8(id->bInterfaceProtocol);
            serial_puts("\r\n");
        }

        if (type == USB_DESC_ENDPOINT && len >= 7) {
            usb_ep_desc_t *ep = (usb_ep_desc_t *)p;
            uint8_t ep_type = ep->bmAttributes & 0x03;

            if (ep_type == EP_TYPE_INT && EP_ADDR_IN(ep->bEndpointAddress)) {
                dev->int_ep_addr     = EP_ADDR_NUM(ep->bEndpointAddress);
                dev->int_ep_interval = ep->bInterval;
                dev->int_ep_maxpkt   = ep->wMaxPacketSize;
                dev->int_ep_valid    = true;

                serial_puts("[USB] Interrupt IN EP=");
                serial_dec(dev->int_ep_addr);
                serial_puts(" interval=");
                serial_dec(dev->int_ep_interval);
                serial_puts(" maxpkt=");
                serial_dec(dev->int_ep_maxpkt);
                serial_puts("\r\n");
            }
        }

        p += len;
    }

    /* ── Step 7: SET_CONFIGURATION ───────────────────────────────────────── */

    if (!usb_set_configuration(dev, config_val)) {
        serial_puts("[USB] SET_CONFIGURATION failed\r\n");
        return false;
    }

    serial_puts("[USB] Enumeration complete for addr ");
    serial_dec(new_addr);
    serial_puts("\r\n");
    return true;
}

/* ── usb_set_configuration ───────────────────────────────────────────────── */

bool usb_set_configuration(usb_device_t *dev, uint8_t config_value)
{
    usb_setup_t setup;
    make_setup(&setup,
               USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
               USB_REQ_SET_CONFIGURATION,
               config_value, 0, 0);

    return uhci_control_transfer(dev->addr, dev->low_speed,
                                 (uint8_t *)&setup, NULL, 0, false);
}

/* ── usb_hid_set_protocol ────────────────────────────────────────────────── */

bool usb_hid_set_protocol(usb_device_t *dev, uint8_t iface, uint8_t protocol)
{
    usb_setup_t setup;
    make_setup(&setup,
               USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
               USB_REQ_HID_SET_PROTOCOL,
               protocol, iface, 0);

    return uhci_control_transfer(dev->addr, dev->low_speed,
                                 (uint8_t *)&setup, NULL, 0, false);
}

/* ── usb_hid_set_idle ────────────────────────────────────────────────────── */

bool usb_hid_set_idle(usb_device_t *dev, uint8_t iface, uint8_t duration)
{
    usb_setup_t setup;
    make_setup(&setup,
               USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
               USB_REQ_HID_SET_IDLE,
               (uint16_t)(duration << 8), iface, 0);

    return uhci_control_transfer(dev->addr, dev->low_speed,
                                 (uint8_t *)&setup, NULL, 0, false);
}
