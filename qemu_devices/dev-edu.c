/*
 * QEMU Educational USB Device
 *
 * Simple USB device with:
 *   - Bulk IN endpoint: returns counter value
 *   - Bulk OUT endpoint: sets counter value
 */

#include "qemu/osdep.h"
#include "hw/usb/usb.h"
#include "hw/usb/desc.h"
#include "qemu/module.h"

#define TYPE_USB_EDU "usb-edu"
#define USB_EDU(obj) OBJECT_CHECK(USBEduState, (obj), TYPE_USB_EDU)

typedef struct {
    USBDevice dev;
    uint32_t counter;
} USBEduState;

enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT,
    STR_SERIAL,
};

static const USBDescStrings desc_strings = {
    [STR_MANUFACTURER] = "QEMU",
    [STR_PRODUCT]      = "Educational USB Device",
    [STR_SERIAL]       = "1234",
};

static const USBDescIface desc_iface = {
    .bInterfaceNumber   = 0,
    .bNumEndpoints      = 2,
    .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress = USB_DIR_IN | 0x01,
            .bmAttributes     = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize   = 64,
        },
        {
            .bEndpointAddress = USB_DIR_OUT | 0x02,
            .bmAttributes     = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize   = 64,
        },
    },
};

static const USBDescDevice desc_device = {
    .bcdUSB         = 0x0200,
    .bDeviceClass   = USB_CLASS_VENDOR_SPEC,
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces = 1,
            .bConfigurationValue = 1,
            .bmAttributes   = USB_CFG_ATT_ONE,
            .bMaxPower      = 50,
            .nif = 1,
            .ifs = &desc_iface,
        },
    },
};

static const USBDesc desc_edu = {
    .id = {
        .idVendor  = 0x1234,
        .idProduct = 0xED00,
        .bcdDevice = 0x0100,
        .iManufacturer = STR_MANUFACTURER,
        .iProduct  = STR_PRODUCT,
        .iSerialNumber = STR_SERIAL,
    },
    .full = &desc_device,
    .str  = desc_strings,
};

static void usb_edu_handle_reset(USBDevice *dev)
{
    USBEduState *s = USB_EDU(dev);
    s->counter = 0;
}

static void usb_edu_handle_control(USBDevice *dev, USBPacket *p,
                                   int request, int value,
                                   int index, int length, uint8_t *data)
{
    int ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }
    p->status = USB_RET_STALL;
}

static void usb_edu_handle_data(USBDevice *dev, USBPacket *p)
{
    USBEduState *s = USB_EDU(dev);
    uint8_t buf[64];

    switch (p->ep->nr) {
    case 1: /* Bulk IN - read counter */
        if (p->pid == USB_TOKEN_IN) {
            buf[0] = s->counter & 0xFF;
            buf[1] = (s->counter >> 8) & 0xFF;
            buf[2] = (s->counter >> 16) & 0xFF;
            buf[3] = (s->counter >> 24) & 0xFF;
            usb_packet_copy(p, buf, 4);
        }
        break;

    case 2: /* Bulk OUT - write counter */
        if (p->pid == USB_TOKEN_OUT) {
            usb_packet_copy(p, buf, 4);
            s->counter = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
        }
        break;

    default:
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_edu_realize(USBDevice *dev, Error **errp)
{
    usb_desc_init(dev);
}

static void usb_edu_class_init(ObjectClass *klass, const void *data)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_edu_realize;
    uc->handle_reset   = usb_edu_handle_reset;
    uc->handle_control = usb_edu_handle_control;
    uc->handle_data    = usb_edu_handle_data;
    uc->product_desc   = "Educational USB Device";
    uc->usb_desc       = &desc_edu;
}

static const TypeInfo usb_edu_info = {
    .name          = TYPE_USB_EDU,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBEduState),
    .class_init    = usb_edu_class_init,
};

static void usb_edu_register_types(void)
{
    type_register_static(&usb_edu_info);
}

type_init(usb_edu_register_types)
