/*
 * usb-gamepad: a USB HID gamepad (M13 path A, docs/tracks/m13-gamepads.md)
 *
 * QEMU has no gamepad of any kind: hw/input/hid.h knows HID_MOUSE,
 * HID_TABLET and HID_KEYBOARD and nothing else, and the input core has no
 * axis event a joystick could ride. This is that device.
 *
 * Its own file rather than another kind in dev-hid.c, because every
 * machine this project makes already has a usb-tablet on that code and a
 * gamepad has no business perturbing it. What is shared is the shape:
 * the descriptor tables, the control handler and the interrupt-IN
 * polling here are dev-hid.c's, with a gamepad report descriptor and
 * packer in place of the pointer one.
 *
 * The guest side needs nothing installed. Windows XP, 98 SE and Me all
 * bind their in-box HID stack to a Generic Desktop / Gamepad collection
 * and expose it to DirectInput and joy.cpl. Windows 98 FE (pre-SE) has a
 * much weaker USB stack and may want the USB supplement; DOS has no USB
 * stack at all and is why path B (the gameport) exists.
 *
 * Copyright: GPL-2.0-only, as the rest of QEMU.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/usb/usb.h"
#include "hw/usb/hid.h"
#include "migration/vmstate.h"
#include "desc.h"
#include "usb-gamepad.h"

#define TYPE_USB_GAMEPAD "usb-gamepad"
OBJECT_DECLARE_SIMPLE_TYPE(USBGamepadState, USB_GAMEPAD)

struct USBGamepadState {
    USBDevice dev;

    /* the pad, exactly as the report carries it */
    uint8_t axes[USB_GAMEPAD_AXES];
    uint8_t hat;
    uint16_t buttons;

    /*
     * Set when the state changed and the guest has not been told yet.
     * The interrupt endpoint NAKs while it is clear, which is what stops
     * a motionless pad from waking the guest sixty times a second. It is the
     * same rule dev-hid.c's hid_has_events() enforces for the pointer.
     */
    bool changed;

    uint8_t idle;
};

/*
 * The one on this machine, or NULL.
 *
 * A file static rather than an object_resolve_path_type() walk on every
 * update: the update runs on the input path, which is the one place in
 * this project where latency is measured (doc 03's budget), and a second
 * gamepad is refused at realize so there is never an ambiguity to
 * resolve. Refusing is better than silently driving whichever one the
 * walk happened to find first.
 */
static USBGamepadState *the_gamepad;

enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CONFIG,
};

static const USBDescStrings desc_strings = {
    [STR_MANUFACTURER] = "2ksbox",
    [STR_PRODUCT]      = "2ksbox USB Gamepad",
    [STR_SERIAL]       = "1",
    [STR_CONFIG]       = "Gamepad",
};

/*
 * A textbook gamepad: two sticks as X/Y and Z/Rz, an 8-way hat and
 * twelve buttons, in six bytes.
 *
 * Two details are not decoration. The hat is declared with a **null
 * state** and a physical range in degrees, which is what makes a
 * released hat mean "centred" rather than "north" to DirectInput. And
 * the axes are eight bits with logical 0..255: period drivers and
 * DirectInput both handle wider axes, but eight bits is what the pads of
 * this era reported, it keeps the report byte-aligned, and it is more
 * resolution than any guest here can use.
 */
static const uint8_t qemu_gamepad_hid_report_descriptor[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop)     */
    0x09, 0x05,        /* Usage (Gamepad)                  */
    0xa1, 0x01,        /* Collection (Application)         */
    0xa1, 0x00,        /*   Collection (Physical)          */
    0x09, 0x30,        /*     Usage (X)                    */
    0x09, 0x31,        /*     Usage (Y)                    */
    0x09, 0x32,        /*     Usage (Z)                    */
    0x09, 0x35,        /*     Usage (Rz)                   */
    0x15, 0x00,        /*     Logical Minimum (0)          */
    0x26, 0xff, 0x00,  /*     Logical Maximum (255)        */
    0x75, 0x08,        /*     Report Size (8)              */
    0x95, 0x04,        /*     Report Count (4)             */
    0x81, 0x02,        /*     Input (Data, Var, Abs)       */

    0x09, 0x39,        /*     Usage (Hat switch)           */
    0x15, 0x00,        /*     Logical Minimum (0)          */
    0x25, 0x07,        /*     Logical Maximum (7)          */
    0x35, 0x00,        /*     Physical Minimum (0)         */
    0x46, 0x3b, 0x01,  /*     Physical Maximum (315)       */
    0x65, 0x14,        /*     Unit (Degrees)               */
    0x75, 0x04,        /*     Report Size (4)              */
    0x95, 0x01,        /*     Report Count (1)             */
    0x81, 0x42,        /*     Input (Data, Var, Abs, Null) */

    0x65, 0x00,        /*     Unit (None)                  */
    0x05, 0x09,        /*     Usage Page (Button)          */
    0x19, 0x01,        /*     Usage Minimum (Button 1)     */
    0x29, 0x0c,        /*     Usage Maximum (Button 12)    */
    0x15, 0x00,        /*     Logical Minimum (0)          */
    0x25, 0x01,        /*     Logical Maximum (1)          */
    0x75, 0x01,        /*     Report Size (1)              */
    0x95, 0x0c,        /*     Report Count (12)            */
    0x81, 0x02,        /*     Input (Data, Var, Abs)       */
    0xc0,              /*   End Collection                 */
    0xc0,              /* End Collection                   */
};

#define GAMEPAD_REPORT_LEN 6

static const USBDescIface desc_iface_gamepad = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    /*
     * Subclass 0 and protocol 0 on purpose: the boot subclass exists only
     * for keyboards and mice, and claiming it for a gamepad makes a BIOS
     * try to drive this as one.
     */
    .bInterfaceSubClass            = 0x00,
    .bInterfaceProtocol            = 0x00,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x01, 0x00,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                sizeof(qemu_gamepad_hid_report_descriptor), 0, /* u16 len */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 8,
            /*
             * 10 ms, the same interval the tablet asks for. A guest of
             * this era polls DirectInput at its own frame rate and never
             * approaches it, and a shorter one only spends host time
             * answering NAKs.
             */
            .bInterval             = 0x0a,
        },
    },
};

static const USBDescDevice desc_device_gamepad = {
    .bcdUSB                        = 0x0100,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_gamepad,
        },
    },
};

static const USBDesc desc_gamepad = {
    .id = {
        /*
         * QEMU's own vendor id, and a product id of ours after the three
         * dev-hid.c already uses (mouse/tablet 0x0001, keyboard 0x0002).
         */
        .idVendor          = 0x0627,
        .idProduct         = 0x0005,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT,
        .iSerialNumber     = STR_SERIAL,
    },
    .full = &desc_device_gamepad,
    .str  = desc_strings,
};

/* --- the state the host sets ------------------------------------- */

static void usb_gamepad_reset_state(USBGamepadState *s)
{
    for (int i = 0; i < USB_GAMEPAD_AXES; i++) {
        s->axes[i] = 0x80;
    }
    s->hat = USB_GAMEPAD_HAT_NULL;
    s->buttons = 0;
    /*
     * True, not false: the guest must be told the pad is centred at
     * least once. A driver that has had no report yet shows whatever it
     * initialised its cache to, and for a joystick that is usually the
     * bottom-left corner of the axis range.
     */
    s->changed = true;
}

bool usb_gamepad_present(void)
{
    return the_gamepad != NULL;
}

void usb_gamepad_set_state(const uint8_t axes[USB_GAMEPAD_AXES],
                           uint8_t hat, uint16_t buttons)
{
    USBGamepadState *s = the_gamepad;

    if (!s) {
        return;
    }
    if (hat > USB_GAMEPAD_HAT_NULL) {
        hat = USB_GAMEPAD_HAT_NULL;
    }
    buttons &= (1u << USB_GAMEPAD_BUTTONS) - 1;
    if (!memcmp(s->axes, axes, USB_GAMEPAD_AXES) &&
        s->hat == hat && s->buttons == buttons) {
        return;
    }
    memcpy(s->axes, axes, USB_GAMEPAD_AXES);
    s->hat = hat;
    s->buttons = buttons;
    s->changed = true;
    /*
     * A guest that suspended the port because nothing had moved for a
     * while will not poll again until it is woken; without this the pad
     * comes back only when something else on the bus does.
     */
    usb_wakeup(s->dev.ep_in + 1, 0);
}

static int usb_gamepad_poll(USBGamepadState *s, uint8_t *buf, int len)
{
    if (len < GAMEPAD_REPORT_LEN) {
        return 0;
    }
    memcpy(buf, s->axes, USB_GAMEPAD_AXES);
    /* low nibble the hat, high nibble buttons 1-4 */
    buf[4] = (s->hat & 0x0f) | ((s->buttons & 0x0f) << 4);
    /* buttons 5-12 */
    buf[5] = (s->buttons >> 4) & 0xff;
    s->changed = false;
    return GAMEPAD_REPORT_LEN;
}

/* --- USB plumbing -------------------------------------------------- */

static void usb_gamepad_handle_reset(USBDevice *dev)
{
    usb_gamepad_reset_state(USB_GAMEPAD(dev));
}

static void usb_gamepad_handle_control(USBDevice *dev, USBPacket *p,
                                       int request, int value, int index,
                                       int length, uint8_t *data)
{
    USBGamepadState *s = USB_GAMEPAD(dev);
    int ret;

    ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }

    switch (request) {
    case InterfaceRequest | USB_REQ_GET_DESCRIPTOR:
        if ((value >> 8) != 0x22) {
            goto fail;
        }
        if (length > (int)sizeof(qemu_gamepad_hid_report_descriptor)) {
            length = sizeof(qemu_gamepad_hid_report_descriptor);
        }
        memcpy(data, qemu_gamepad_hid_report_descriptor, length);
        p->actual_length = length;
        break;
    case HID_GET_REPORT:
        p->actual_length = usb_gamepad_poll(s, data, length);
        break;
    case HID_GET_IDLE:
        data[0] = s->idle;
        p->actual_length = 1;
        break;
    case HID_SET_IDLE:
        s->idle = (uint8_t)(value >> 8);
        break;
    /*
     * No GET/SET_PROTOCOL: those are the boot protocol's, and this
     * interface does not claim the boot subclass (see desc_iface_gamepad).
     * No SET_REPORT: nothing here has an LED or a rumble motor.
     */
    default:
    fail:
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_gamepad_handle_data(USBDevice *dev, USBPacket *p)
{
    USBGamepadState *s = USB_GAMEPAD(dev);
    uint8_t buf[GAMEPAD_REPORT_LEN];
    int len;

    switch (p->pid) {
    case USB_TOKEN_IN:
        if (p->ep->nr != 1) {
            goto fail;
        }
        if (!s->changed) {
            p->status = USB_RET_NAK;
            return;
        }
        len = usb_gamepad_poll(s, buf, sizeof(buf));
        usb_packet_copy(p, buf, len);
        break;
    default:
    fail:
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_gamepad_realize(USBDevice *dev, Error **errp)
{
    USBGamepadState *s = USB_GAMEPAD(dev);

    if (the_gamepad) {
        error_setg(errp, "only one usb-gamepad is supported");
        return;
    }
    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    usb_gamepad_reset_state(s);
    the_gamepad = s;
}

static void usb_gamepad_unrealize(USBDevice *dev)
{
    if (the_gamepad == USB_GAMEPAD(dev)) {
        the_gamepad = NULL;
    }
}

static const VMStateDescription vmstate_usb_gamepad = {
    .name = "usb-gamepad",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_USB_DEVICE(dev, USBGamepadState),
        VMSTATE_UINT8_ARRAY(axes, USBGamepadState, USB_GAMEPAD_AXES),
        VMSTATE_UINT8(hat, USBGamepadState),
        VMSTATE_UINT16(buttons, USBGamepadState),
        VMSTATE_BOOL(changed, USBGamepadState),
        VMSTATE_UINT8(idle, USBGamepadState),
        VMSTATE_END_OF_LIST()
    }
};

static void usb_gamepad_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_gamepad_realize;
    uc->unrealize      = usb_gamepad_unrealize;
    uc->product_desc   = "2ksbox USB Gamepad";
    uc->usb_desc       = &desc_gamepad;
    uc->handle_reset   = usb_gamepad_handle_reset;
    uc->handle_control = usb_gamepad_handle_control;
    uc->handle_data    = usb_gamepad_handle_data;
    uc->handle_attach  = usb_desc_attach;
    dc->vmsd           = &vmstate_usb_gamepad;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usb_gamepad_info = {
    .name          = TYPE_USB_GAMEPAD,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBGamepadState),
    .class_init    = usb_gamepad_class_init,
};

static void usb_gamepad_register_types(void)
{
    type_register_static(&usb_gamepad_info);
}

type_init(usb_gamepad_register_types)
