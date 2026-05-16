/*
 * ChimaeraOS — USB Core Layer
 * src/drivers/usb/usb_core.h
 *
 * USB standard descriptor structures, setup packet format, and the
 * device enumeration API.  Only the subset needed for HID boot-protocol
 * keyboard and mouse is implemented.
 *
 * Reference: USB 2.0 Specification, Chapter 9
 */

#ifndef USB_CORE_H
#define USB_CORE_H

#include "../../include/types.h"

/* ── USB standard request codes ─────────────────────────────────────────── */

#define USB_REQ_GET_STATUS          0x00
#define USB_REQ_CLEAR_FEATURE       0x01
#define USB_REQ_SET_FEATURE         0x03
#define USB_REQ_SET_ADDRESS         0x05
#define USB_REQ_GET_DESCRIPTOR      0x06
#define USB_REQ_SET_DESCRIPTOR      0x07
#define USB_REQ_GET_CONFIGURATION   0x08
#define USB_REQ_SET_CONFIGURATION   0x09
#define USB_REQ_GET_INTERFACE       0x0A
#define USB_REQ_SET_INTERFACE       0x0B

/* HID class requests */
#define USB_REQ_HID_GET_REPORT      0x01
#define USB_REQ_HID_GET_IDLE        0x02
#define USB_REQ_HID_GET_PROTOCOL    0x03
#define USB_REQ_HID_SET_REPORT      0x09
#define USB_REQ_HID_SET_IDLE        0x0A
#define USB_REQ_HID_SET_PROTOCOL    0x0B

/* ── USB descriptor types ────────────────────────────────────────────────── */

#define USB_DESC_DEVICE             0x01
#define USB_DESC_CONFIG             0x02
#define USB_DESC_STRING             0x03
#define USB_DESC_INTERFACE          0x04
#define USB_DESC_ENDPOINT           0x05
#define USB_DESC_HID                0x21
#define USB_DESC_REPORT             0x22

/* ── USB bmRequestType fields ────────────────────────────────────────────── */

/* Direction */
#define USB_DIR_OUT                 0x00
#define USB_DIR_IN                  0x80

/* Type */
#define USB_TYPE_STANDARD           0x00
#define USB_TYPE_CLASS              0x20
#define USB_TYPE_VENDOR             0x40

/* Recipient */
#define USB_RECIP_DEVICE            0x00
#define USB_RECIP_INTERFACE         0x01
#define USB_RECIP_ENDPOINT          0x02

/* ── USB class codes ─────────────────────────────────────────────────────── */

#define USB_CLASS_HID               0x03
#define USB_SUBCLASS_BOOT           0x01
#define USB_PROTOCOL_KEYBOARD       0x01
#define USB_PROTOCOL_MOUSE          0x02

/* HID protocol values for SET_PROTOCOL */
#define HID_PROTOCOL_BOOT           0x00
#define HID_PROTOCOL_REPORT         0x01

/* ── USB setup packet (8 bytes) ──────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

/* ── USB Device Descriptor (18 bytes) ────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_desc_t;

/* ── USB Configuration Descriptor (9 bytes header) ──────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_desc_t;

/* ── USB Interface Descriptor (9 bytes) ──────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} usb_iface_desc_t;

/* ── USB Endpoint Descriptor (7 bytes) ───────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;  /* bit 7: 1=IN, 0=OUT; bits 3:0: EP number */
    uint8_t  bmAttributes;      /* bits 1:0: 0=ctrl, 1=iso, 2=bulk, 3=int  */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;         /* polling interval in frames (1–255)       */
} usb_ep_desc_t;

/* Endpoint address helpers */
#define EP_ADDR_NUM(a)  ((a) & 0x0F)
#define EP_ADDR_IN(a)   (!!((a) & 0x80))
#define EP_TYPE_INT     0x03

/* ── USB device record ───────────────────────────────────────────────────── */

#define USB_MAX_ENDPOINTS  4

typedef struct {
    uint8_t  addr;              /* assigned USB address (1–127)            */
    bool     low_speed;         /* true = low-speed device                 */
    uint8_t  port;              /* UHCI port index (0 or 1)                */

    /* Parsed from descriptors */
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    uint8_t  max_packet0;       /* max packet size for EP0                 */

    /* Interrupt endpoint (for HID polling) */
    uint8_t  int_ep_addr;       /* endpoint address (1–15)                 */
    uint8_t  int_ep_interval;   /* polling interval in frames              */
    uint16_t int_ep_maxpkt;     /* max packet size                         */
    bool     int_ep_valid;      /* true if an interrupt IN endpoint found  */
} usb_device_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/*
 * usb_enumerate_port — enumerate the device on the given UHCI port.
 * Assigns an address, fetches device and configuration descriptors,
 * and fills in *dev.
 * Returns true on success.
 */
bool usb_enumerate_port(uint8_t port, bool low_speed, usb_device_t *dev);

/*
 * usb_set_configuration — send SET_CONFIGURATION(1) to activate the
 * first configuration.
 */
bool usb_set_configuration(usb_device_t *dev, uint8_t config_value);

/*
 * usb_hid_set_protocol — send HID SET_PROTOCOL to switch to boot protocol.
 * protocol: HID_PROTOCOL_BOOT (0) or HID_PROTOCOL_REPORT (1)
 */
bool usb_hid_set_protocol(usb_device_t *dev, uint8_t iface, uint8_t protocol);

/*
 * usb_hid_set_idle — send HID SET_IDLE to set the idle rate.
 * duration=0 means "report only on change".
 */
bool usb_hid_set_idle(usb_device_t *dev, uint8_t iface, uint8_t duration);

#endif /* USB_CORE_H */
