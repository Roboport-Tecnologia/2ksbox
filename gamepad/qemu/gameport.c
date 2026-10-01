/*
 * gameport: the PC analog joystick port at 0x201 (M13 path B,
 * docs/tracks/m13-gamepads.md).
 *
 * Overlaid into hw/input/ by scripts/prepare-qemu.sh and built by
 * patch 27. QEMU has never had this port: nothing in hw/ answers 0x201,
 * and sb16.c models the DSP and mixer only. The gameport that sits at
 * 0x200-0x207 on a real Sound Blaster is not there. So a DOS game, which
 * reads the port itself and has no other way to find a joystick, found
 * nothing at all before this.
 *
 * A standalone ISA device rather than a member of one sound card, because
 * the family's card is a separate choice (bundle::Sound, doc 20) and a
 * joystick should not appear and vanish with it. On real hardware it was
 * usually on the card; here the bundle names it.
 *
 * The hardware is four RC one-shots and four buttons. A write to 0x201
 * triggers all four one-shots; a read returns bits 0-3 *set* while each
 * axis is still charging and bits 4-7 *clear* while each button is held.
 * The pulse is t = 24.2 us + 0.011 x R us over a 0-100 kohm pot, so 24 us
 * at one extreme and about 1124 us at the other. A game writes, then
 * counts loop iterations until the bit clears, and the count is the axis.
 *
 * No timer is needed for any of that, and none is used: the write records
 * a deadline per axis on QEMU_CLOCK_VIRTUAL and a read compares against
 * it. Exact, and it costs nothing on a port nobody is polling.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "hw/isa/isa.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"

#include "gameport.h"

#define TYPE_GAMEPORT "gameport"
OBJECT_DECLARE_SIMPLE_TYPE(GameportState, GAMEPORT)

/*
 * The one-shot, in nanoseconds, for an axis byte.
 *
 * The byte is the pot's position, so 0x00 is 0 ohm and 0xff the full
 * 100 kohm: 24.2 us + 0.011 x (byte/255 x 100000) us. Integer nanoseconds
 * because a float here would be the only one in the device and the answer
 * is exact anyway.
 *
 * The polarity is the hardware's and needs no flip: a low byte is a short
 * pulse and a small count, which is what every game and every calibration
 * routine reads as left / up. A low byte is also what gamepad::hid_axis()
 * already gives for a stick pushed left or up.
 */
#define GAMEPORT_BASE_NS 24200
#define GAMEPORT_SPAN_NS 1100000

static inline int64_t gameport_width_ns(uint8_t axis)
{
    return GAMEPORT_BASE_NS + (int64_t)axis * GAMEPORT_SPAN_NS / 255;
}

struct GameportState {
    ISADevice parent_obj;

    uint32_t iobase;

    /* The pad as the host last set it. Centred and unpressed until then,
     * which is what a machine whose owner asked for a gameport and has no
     * controller plugged in should read as. */
    uint8_t axes[GAMEPORT_AXES];
    uint8_t buttons;

    /* When each one-shot ends, on the virtual clock. 0 = never armed, and
     * a deadline in the past is simply an expired pulse: there is no state
     * to clear and nothing to do when it passes. */
    int64_t deadline[GAMEPORT_AXES];

    /*
     * What the guest is doing with the port, reported every 5 s while it
     * is doing anything. The first question about a joystick that does not
     * work is whether the guest is polling at all. A DOS game with its
     * joystick support switched off, and a 9x guest with no driver
     * installed, both touch this port exactly never, and that is not
     * visible from inside the guest either.
     */
    int64_t report_ns;
    unsigned arms;
    unsigned reads;

    PortioList port_list;
};

/* The one on this machine, or NULL. A file static for the same reason
 * dev-gamepad.c has one: the update runs on the input path, and a second
 * gameport is refused at realize so there is no ambiguity to resolve. */
static GameportState *the_gameport;

bool gameport_present(void)
{
    return the_gameport != NULL;
}

void gameport_set_state(const uint8_t axes[GAMEPORT_AXES], uint8_t hat,
                        uint16_t buttons)
{
    GameportState *s = the_gameport;

    if (!s) {
        return;
    }
    memcpy(s->axes, axes, GAMEPORT_AXES);
    /*
     * The d-pad drives the first stick's own axes to their ends.
     *
     * This is what a digital pad *is* on this port. A Gravis GamePad has
     * no pots at all. Its directions switch the one-shots between the two
     * extremes, so a DOS game has
     * no idea whether it is reading a stick or a d-pad, and no way to
     * read a d-pad any other way. Without this, the control every era
     * game is played with would do nothing on the only path that reaches
     * DOS.
     *
     * The stick wins while the d-pad is centred, and the d-pad wins when
     * it is not: it can only ask for an extreme, and an extreme is
     * further out than anything the stick can say.
     */
    if (hat < 8) {
        /* 0..7 clockwise from north */
        static const int8_t dx[8] = {  0,  1,  1,  1,  0, -1, -1, -1 };
        static const int8_t dy[8] = { -1, -1,  0,  1,  1,  1,  0, -1 };
        if (dx[hat]) {
            s->axes[0] = dx[hat] > 0 ? 0xff : 0x00;
        }
        if (dy[hat]) {
            s->axes[1] = dy[hat] > 0 ? 0xff : 0x00;
        }
    }
    /*
     * Buttons 1-4 are the four face buttons, in the order
     * gamepad::HID_BUTTONS lists them, so a control is the same number on
     * both guest paths: the button that lights up in joy.cpl on a Windows
     * machine is the button a DOS game reads here. The other eight have
     * nowhere to go on this port.
     */
    s->buttons = buttons & 0x0f;
}

/* --- the port ------------------------------------------------------- */

static void gameport_report(GameportState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->report_ns == 0) {
        s->report_ns = now;
        return;
    }
    if (now - s->report_ns < 5 * NANOSECONDS_PER_SECOND) {
        return;
    }
    if (s->reads) {
        info_report("gameport: %u polls, %u one-shot arms in 5.0 s",
                    s->reads, s->arms);
    }
    s->report_ns = now;
    s->reads = 0;
    s->arms = 0;
}

static uint32_t gameport_read(void *opaque, uint32_t addr)
{
    GameportState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t v = 0;

    for (int i = 0; i < GAMEPORT_AXES; i++) {
        if (s->deadline[i] > now) {
            v |= 1u << i;
        }
    }
    /* A held button pulls its line low, so the bit is *clear* while it is
     * pressed. An idle port with nothing pressed therefore reads 0xf0.
     * And 0xff, which is what an absent port reads off the open bus, is
     * also what this one reads in the first microseconds after a write,
     * while all four one-shots are still charging. Both are correct. */
    v |= (uint32_t)(~s->buttons & 0x0f) << 4;

    s->reads++;
    gameport_report(s);
    return v;
}

static void gameport_write(void *opaque, uint32_t addr, uint32_t val)
{
    GameportState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /* Any value: the write is a trigger, not data. All four fire, and
     * firing again while they are charging restarts them, exactly as the
     * 558 does. */
    for (int i = 0; i < GAMEPORT_AXES; i++) {
        s->deadline[i] = now + gameport_width_ns(s->axes[i]);
    }
    s->arms++;
    gameport_report(s);
}

/*
 * 0x200-0x207, every one of them the same register.
 *
 * The IBM Game Control Adapter answered 0x201 alone, but every sound card
 * that carried a gameport decoded the whole eight, and a fair amount of
 * era code reads 0x200 or writes 0x203. Answering all eight is what the
 * hardware a guest of this era expects did.
 */
static const MemoryRegionPortio gameport_portio_list[] = {
    { 0, 8, 1, .read = gameport_read, .write = gameport_write },
    PORTIO_END_OF_LIST(),
};

static void gameport_reset(DeviceState *dev)
{
    GameportState *s = GAMEPORT(dev);

    for (int i = 0; i < GAMEPORT_AXES; i++) {
        s->axes[i] = 0x80;
        s->deadline[i] = 0;
    }
    s->buttons = 0;
}

static void gameport_realizefn(DeviceState *dev, Error **errp)
{
    GameportState *s = GAMEPORT(dev);

    if (the_gameport) {
        error_setg(errp, "only one gameport is supported");
        return;
    }
    gameport_reset(dev);
    portio_list_init(&s->port_list, OBJECT(s), gameport_portio_list, s,
                     TYPE_GAMEPORT);
    portio_list_add(&s->port_list, isa_address_space_io(&s->parent_obj),
                    s->iobase);
    the_gameport = s;
}

static void gameport_unrealizefn(DeviceState *dev)
{
    if (the_gameport == GAMEPORT(dev)) {
        the_gameport = NULL;
    }
}

static const Property gameport_properties[] = {
    /* The standard base. A property because it costs nothing and the
     * eight ports are a card's decode, not an architectural constant. */
    DEFINE_PROP_UINT32("iobase", GameportState, iobase, 0x200),
};

static void gameport_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = gameport_realizefn;
    dc->unrealize = gameport_unrealizefn;
    device_class_set_legacy_reset(dc, gameport_reset);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    dc->desc = "PC analog joystick port (gameport)";
    device_class_set_props(dc, gameport_properties);
    /*
     * No vmstate, like the opl3 (patch 60): the only state here is four
     * deadlines on a clock a snapshot does not preserve and a pad the host
     * re-sends on its next published frame, so a machine resumes with the
     * port idle and is right again one frame later.
     */
}

static const TypeInfo gameport_info = {
    .name          = TYPE_GAMEPORT,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(GameportState),
    .class_init    = gameport_class_initfn,
};

static void gameport_register_types(void)
{
    type_register_static(&gameport_info);
}

type_init(gameport_register_types)
