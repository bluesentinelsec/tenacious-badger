/* Private to the Linux gamepad backend and its tests: what an evdev
 * device's codes mean in the standard layout, in standard C, so that the
 * mapping is tested with events a test makes rather than a device.
 *
 * The codes are the kernel's, from linux/input-event-codes.h, which are a
 * stable interface; gamepad.c checks them against the kernel's headers. */
#ifndef TB_LINUX_GAMEPAD_EVDEV_H
#define TB_LINUX_GAMEPAD_EVDEV_H

#include "badger/gamepad.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Event types. */
#define TB_EVDEV_EV_SYN 0x00u
#define TB_EVDEV_EV_KEY 0x01u
#define TB_EVDEV_EV_ABS 0x03u
#define TB_EVDEV_EV_FF 0x15u

/* Synchronization codes. */
#define TB_EVDEV_SYN_REPORT 0u
#define TB_EVDEV_SYN_DROPPED 3u

/* Keys and buttons. */
#define TB_EVDEV_BTN_SOUTH 0x130u
#define TB_EVDEV_BTN_EAST 0x131u
#define TB_EVDEV_BTN_NORTH 0x133u /* Also BTN_X, the X button on Xbox-style drivers. */
#define TB_EVDEV_BTN_WEST 0x134u  /* Also BTN_Y. */
#define TB_EVDEV_BTN_TL 0x136u
#define TB_EVDEV_BTN_TR 0x137u
#define TB_EVDEV_BTN_TL2 0x138u
#define TB_EVDEV_BTN_TR2 0x139u
#define TB_EVDEV_BTN_SELECT 0x13au
#define TB_EVDEV_BTN_START 0x13bu
#define TB_EVDEV_BTN_MODE 0x13cu /* The guide button: never read. */
#define TB_EVDEV_BTN_THUMBL 0x13du
#define TB_EVDEV_BTN_THUMBR 0x13eu
#define TB_EVDEV_BTN_DPAD_UP 0x220u
#define TB_EVDEV_BTN_DPAD_DOWN 0x221u
#define TB_EVDEV_BTN_DPAD_LEFT 0x222u
#define TB_EVDEV_BTN_DPAD_RIGHT 0x223u
/* The Steam Deck's back buttons, from hid-steam: L4, R4, L5, R5. */
#define TB_EVDEV_BTN_GRIPL 0x224u
#define TB_EVDEV_BTN_GRIPR 0x225u
#define TB_EVDEV_BTN_GRIPL2 0x226u
#define TB_EVDEV_BTN_GRIPR2 0x227u
/* xpad's d-pad as buttons (left, right, up, down), then the Elite pad's
 * paddles (P1 upper right, P2 lower right, P3 upper left, P4 lower left). */
#define TB_EVDEV_BTN_TRIGGER_HAPPY1 0x2c0u
#define TB_EVDEV_BTN_TRIGGER_HAPPY2 0x2c1u
#define TB_EVDEV_BTN_TRIGGER_HAPPY3 0x2c2u
#define TB_EVDEV_BTN_TRIGGER_HAPPY4 0x2c3u
#define TB_EVDEV_BTN_TRIGGER_HAPPY5 0x2c4u
#define TB_EVDEV_BTN_TRIGGER_HAPPY6 0x2c5u
#define TB_EVDEV_BTN_TRIGGER_HAPPY7 0x2c6u
#define TB_EVDEV_BTN_TRIGGER_HAPPY8 0x2c7u
#define TB_EVDEV_KEY_COUNT 0x300u /* KEY_CNT */

/* Absolute axes. */
#define TB_EVDEV_ABS_X 0x00u
#define TB_EVDEV_ABS_Y 0x01u
#define TB_EVDEV_ABS_Z 0x02u
#define TB_EVDEV_ABS_RX 0x03u
#define TB_EVDEV_ABS_RY 0x04u
#define TB_EVDEV_ABS_RZ 0x05u
#define TB_EVDEV_ABS_GAS 0x09u
#define TB_EVDEV_ABS_BRAKE 0x0au
#define TB_EVDEV_ABS_HAT0X 0x10u
#define TB_EVDEV_ABS_HAT0Y 0x11u
#define TB_EVDEV_ABS_HAT2X 0x14u /* The Steam Deck's right trigger. */
#define TB_EVDEV_ABS_HAT2Y 0x15u /* The Steam Deck's left trigger. */
#define TB_EVDEV_ABS_COUNT 0x40u /* ABS_CNT */

/* Force feedback. */
#define TB_EVDEV_FF_RUMBLE 0x50u
#define TB_EVDEV_FF_COUNT 0x80u /* FF_CNT */

/* USB vendors whose drivers name the face buttons by their place, as the
 * kernel's gamepad documentation asks; others, Xbox-style, by their label. */
#define TB_EVDEV_VENDOR_SONY 0x054cu
#define TB_EVDEV_VENDOR_NINTENDO 0x057eu

/* The Switch 2 controllers, whose driver names A and B by label: the
 * Joy-Con 2 pair, the Pro Controller and the GameCube controller. */
#define TB_EVDEV_SWITCH2_JOYCON_RIGHT 0x2066u
#define TB_EVDEV_SWITCH2_JOYCON_LEFT 0x2067u
#define TB_EVDEV_SWITCH2_PRO 0x2069u
#define TB_EVDEV_SWITCH2_GAMECUBE 0x2073u

/* No axis code. */
#define TB_EVDEV_NO_AXIS 0xffffu

/* What a device says it has, as its ioctls report it. */
typedef struct tb_evdev_device
{
    uint8_t keys[TB_EVDEV_KEY_COUNT / 8u];   /* EVIOCGBIT(EV_KEY): a bit a code. */
    uint8_t axes[TB_EVDEV_ABS_COUNT / 8u];   /* EVIOCGBIT(EV_ABS). */
    uint8_t effects[TB_EVDEV_FF_COUNT / 8u]; /* EVIOCGBIT(EV_FF). */
    int32_t minimum[TB_EVDEV_ABS_COUNT];     /* EVIOCGABS: each axis's range. */
    int32_t maximum[TB_EVDEV_ABS_COUNT];
    uint16_t vendor; /* EVIOCGID. */
    uint16_t product;
} tb_evdev_device;

/* Which of a device's codes are which of the standard layout's. */
typedef struct tb_evdev_layout
{
    uint16_t axes[TB_GAMEPAD_AXIS_COUNT]; /* An axis code each, or TB_EVDEV_NO_AXIS. */
    bool positional_faces;                /* BTN_NORTH and BTN_WEST are the top and left buttons. */
    bool swapped_south_east; /* BTN_SOUTH is the right button and BTN_EAST the bottom. */
    bool digital_triggers;   /* BTN_TL2 and BTN_TR2 are the triggers, at 0 or 1. */
    bool hat_dpad;           /* ABS_HAT0X and ABS_HAT0Y are the d-pad. */
    bool happy_dpad;         /* BTN_TRIGGER_HAPPY1 to 4 are the d-pad, as xpad's. */
} tb_evdev_layout;

/* Whether a device's code is set in one of its bit arrays. */
bool tb_evdev_has(const uint8_t *bits, size_t count, uint32_t code);

/* Whether a device is a gamepad: the south button and the two axes of a
 * stick. Joysticks, wheels, keyboards and touchpads are not. */
bool tb_evdev_is_gamepad(const tb_evdev_device *device);

/* Whether a device can rumble: FF_RUMBLE. */
bool tb_evdev_can_rumble(const tb_evdev_device *device);

/* Choose which of a gamepad's codes are which. */
void tb_evdev_layout_make(const tb_evdev_device *device, tb_evdev_layout *layout);

/* Apply one event of the device to a state: a key or an absolute axis.
 * Other types, and codes the layout does not use, change nothing. */
void tb_evdev_apply(const tb_evdev_device *device, const tb_evdev_layout *layout, uint32_t type,
                    uint32_t code, int32_t value, tb_gamepad_state *state);

/* The state of keys that are down and axes' values, as EVIOCGKEY and
 * EVIOCGABS report them after a device is opened or its events were
 * dropped. */
void tb_evdev_resync(const tb_evdev_device *device, const tb_evdev_layout *layout,
                     const uint8_t *keys_down, const int32_t *values, tb_gamepad_state *state);

#endif
