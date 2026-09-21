/*
 * QEMU Educational USB Serial Device
 *
 * Emulates a USB CDC ACM serial port.
 * Shows up as /dev/ttyACM0 in Linux.
 * Echo back whatever is sent, with counter prefix.
 */

#include "qemu/osdep.h"
#include "hw/usb/usb.h"
#include "hw/usb/desc.h"
#include "qemu/module.h"

#define TYPE_USB_EDU_SERIAL "usb-edu-serial"
#define USB_EDU_SERIAL(obj) OBJECT_CHECK(USBEduSerialState, (obj), TYPE_USB_EDU_SERIAL)

typedef struct {
    USBDevice dev;
    uint32_t counter;
    uint8_t rx_buf[64];
    int rx_len;
} USBEduSerialState;

enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT,
    STR_SERIAL,
};

static const USBDescStrings desc_strings = {
    [STR_MANUFACTURER] = "QEMU",
    [STR_PRODUCT]      = "Edu Serial",
    [STR_SERIAL]       = "12345",
};

static const USBDescIface desc_ifaces[] = {
    {
        .bInterfaceNumber   = 0,
        .bNumEndpoints      = 1,
        .bInterfaceClass    = 0x02,  /* CDC */
        .bInterfaceSubClass = 0x02,  /* ACM */
        .bInterfaceProtocol = 0x01,  /* AT commands */
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress = USB_DIR_IN | 0x01,
                .bmAttributes     = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize   = 8,
                .bInterval        = 255,
            },
        },
    },
    {
        .bInterfaceNumber   = 1,
        .bNumEndpoints      = 2,
        .bInterfaceClass    = 0x0A,  /* CDC Data */
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress = USB_DIR_IN | 0x02,
                .bmAttributes     = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize   = 64,
            },
            {
                .bEndpointAddress = USB_DIR_OUT | 0x03,
                .bmAttributes     = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize   = 64,
            },
        },
    },
};

static const USBDescDevice desc_device = {
    .bcdUSB         = 0x0200,
    .bDeviceClass   = 0x02,  /* CDC */
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces = 2,
            .bConfigurationValue = 1,
            .bmAttributes   = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER,
            .bMaxPower      = 50,
            .nif = 2,
            .ifs = desc_ifaces,
        },
    },
};

static const USBDesc desc_serial = {
    .id = {
        .idVendor  = 0x1234,
        .idProduct = 0xED02,
        .bcdDevice = 0x0100,
        .iManufacturer = STR_MANUFACTURER,
        .iProduct  = STR_PRODUCT,
        .iSerialNumber = STR_SERIAL,
    },
    .full = &desc_device,
    .str  = desc_strings,
};

static void usb_edu_serial_reset(USBDevice *dev)
{
    USBEduSerialState *s = USB_EDU_SERIAL(dev);
    s->counter = 0;
    s->rx_len = 0;
}

static void usb_edu_serial_handle_control(USBDevice *dev, USBPacket *p,
                                          int request, int value,
                                          int index, int length, uint8_t *data)
{
    int ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }
    
    /* Handle CDC ACM class requests */
    switch (request) {
    case 0x2021:  /* SET_LINE_CODING */
    case 0x2022:  /* SET_CONTROL_LINE_STATE */
        p->actual_length = 0;
        break;
    case 0x21A1:  /* GET_LINE_CODING */
        /* Return 115200 8N1 */
        data[0] = 0x00; data[1] = 0xC2; data[2] = 0x01; data[3] = 0x00; /* 115200 */
        data[4] = 0;    /* 1 stop bit */
        data[5] = 0;    /* no parity */
        data[6] = 8;    /* 8 data bits */
        p->actual_length = 7;
        break;
    default:
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_edu_serial_handle_data(USBDevice *dev, USBPacket *p)
{
    USBEduSerialState *s = USB_EDU_SERIAL(dev);
    uint8_t buf[64];
    int len;

    switch (p->ep->nr) {
    case 1:  /* Interrupt IN - status (not used) */
        p->status = USB_RET_NAK;
        break;

    case 2:  /* Bulk IN - send data to host */
        if (s->rx_len > 0) {
            len = s->rx_len > p->iov.size ? p->iov.size : s->rx_len;
            usb_packet_copy(p, s->rx_buf, len);
            s->rx_len = 0;
            fprintf(stderr, "USB-EDU-SERIAL: sent %d bytes to host\n", len);
        } else {
            p->status = USB_RET_NAK;
        }
        break;

    case 3:  /* Bulk OUT - receive data from host */
        len = p->iov.size;
        usb_packet_copy(p, buf, len);
        buf[len] = '\0';
        fprintf(stderr, "USB-EDU-SERIAL: received '%s'\n", buf);
        
        /* Echo back with counter */
        s->counter++;
        s->rx_len = snprintf((char*)s->rx_buf, sizeof(s->rx_buf),
                             "[%d] %s", s->counter, buf);
        break;

    default:
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_edu_serial_realize(USBDevice *dev, Error **errp)
{
    usb_desc_init(dev);
}

static void usb_edu_serial_class_init(ObjectClass *klass, const void *data)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_edu_serial_realize;
    uc->handle_reset   = usb_edu_serial_reset;
    uc->handle_control = usb_edu_serial_handle_control;
    uc->handle_data    = usb_edu_serial_handle_data;
    uc->product_desc   = "Edu Serial";
    uc->usb_desc       = &desc_serial;
}

static const TypeInfo usb_edu_serial_info = {
    .name          = TYPE_USB_EDU_SERIAL,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBEduSerialState),
    .class_init    = usb_edu_serial_class_init,
};

static void usb_edu_serial_register(void)
{
    type_register_static(&usb_edu_serial_info);
}

type_init(usb_edu_serial_register)
