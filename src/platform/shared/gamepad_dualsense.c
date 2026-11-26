/* The protocol of Sony's DualSense and DualSense Edge over USB and
 * Bluetooth, in standard C. The facts are from the drivers that use it,
 * Linux's hid-playstation and SDL's, and from reports read from a real
 * pad; the code is the engine's own.
 *
 * **Reports.** Over USB the pad sends its full input report, 0x01, from
 * the start. Over Bluetooth it sends a simple report with the same ID,
 * sticks, buttons and triggers only, until a host reads its calibration
 * feature report (0x05); from then on it sends the full report as 0x31,
 * two bytes later and ending in a CRC-32. tb_dualsense_driver's start reads
 * it, and parse reads all three. Over Bluetooth every report ends in a
 * CRC-32 of a seed byte, which says what kind of report it is, and the
 * report before the CRC; a full report whose CRC is wrong is dropped.
 *
 * **Buttons by place.** Cross is south, Circle east, Square west and
 * Triangle north; L1 and R1 are the shoulders, L2 and R2 the triggers,
 * read from their analog values; L3 and R3 the sticks; Options is start and
 * Create, the one button left of the touchpad where other pads have Back,
 * View or Select, is back. The Edge's back buttons are the upper paddles.
 * The PS button is the system's, as the guide button is; the mute button
 * mutes the pad's microphone in its firmware, and sits where the Xbox pad
 * has its share button: neither is read. The touchpad's click has no place
 * in the layout, nor do the Edge's Fn buttons, which switch the pad's own
 * profiles: neither is read either.
 *
 * **Rumble and lights.** The pad has two voice-coil actuators, which its
 * firmware drives as an older pad's two motors when an output report asks
 * it to: the left grip's as the low-frequency motor, the right's as the
 * high. The motors run until the next report says otherwise. The same
 * reports set the light bar and the five player lights: each report says
 * which of its parts it sets, and the others stay as they were. */
#include "gamepad_dualsense.h"

#include "gamepad.h"

#include "badger/assert.h"
#include "badger/checksum.h"

#include <string.h>

/* Where the full report has what the layout reads, after its ID: over USB
 * at once, over Bluetooth after a byte that numbers the reports. */
#define TB_DUALSENSE_USB_DATA 1u
#define TB_DUALSENSE_BLUETOOTH_DATA 2u

/* Bytes of the full report's data that the layout reads: the sticks, the
 * triggers, a counter and the three bytes of buttons. */
#define TB_DUALSENSE_DATA_SIZE 10u

/* The low four bits of the first byte of buttons: the d-pad as a hat, 0
 * up and on clockwise to 7, and 8 or more at rest. */
#define TB_DUALSENSE_HAT_MASK 0x0fu
#define TB_DUALSENSE_HAT_DIRECTIONS 8u

/* The largest value of a stick's axis or a trigger's. */
#define TB_DUALSENSE_AXIS_MAX 255

/* Where each report has its parts, after its ID. */
typedef struct tb_dualsense_format
{
    size_t data;       /* The first byte of the parts below. */
    size_t sticks;     /* Left X and Y, right X and Y: up is 0. */
    size_t triggers;   /* Left, right. */
    size_t buttons;    /* Three bytes. */
    bool back_buttons; /* The third byte has the Edge's back buttons. */
} tb_dualsense_format;

static const tb_dualsense_format tb_dualsense_full_usb = {TB_DUALSENSE_USB_DATA, 0, 4, 7, true};
static const tb_dualsense_format tb_dualsense_full_bluetooth = {TB_DUALSENSE_BLUETOOTH_DATA, 0, 4,
                                                                7, true};
/* The simple report's third byte of buttons counts reports in its high six
 * bits. */
static const tb_dualsense_format tb_dualsense_simple = {TB_DUALSENSE_USB_DATA, 0, 7, 4, false};

/* A button of a report: which of the three bytes, its bit, and the button
 * of the layout. */
typedef struct tb_dualsense_bit
{
    uint8_t byte;
    uint8_t mask;
    tb_gamepad_button button;
} tb_dualsense_bit;

static const tb_dualsense_bit tb_dualsense_bits[] = {
    {0, 0x10, TB_GAMEPAD_BUTTON_WEST},  /* Square */
    {0, 0x20, TB_GAMEPAD_BUTTON_SOUTH}, /* Cross */
    {0, 0x40, TB_GAMEPAD_BUTTON_EAST},  /* Circle */
    {0, 0x80, TB_GAMEPAD_BUTTON_NORTH}, /* Triangle */
    {1, 0x01, TB_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {1, 0x02, TB_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    /* 1: 0x04 L2 and 0x08 R2, which the analog triggers give. */
    {1, 0x10, TB_GAMEPAD_BUTTON_BACK},  /* Create */
    {1, 0x20, TB_GAMEPAD_BUTTON_START}, /* Options */
    {1, 0x40, TB_GAMEPAD_BUTTON_LEFT_STICK},
    {1, 0x80, TB_GAMEPAD_BUTTON_RIGHT_STICK},
    /* 2: 0x01 PS and 0x04 mute, the system's; 0x02 the touchpad's click. */
};

/* The Edge's, in the third byte: 0x10 and 0x20 are its Fn buttons. */
static const tb_dualsense_bit tb_dualsense_edge_bits[] = {
    {2, 0x40, TB_GAMEPAD_BUTTON_LEFT_PADDLE_1},
    {2, 0x80, TB_GAMEPAD_BUTTON_RIGHT_PADDLE_1},
};

/* The d-pad's buttons of each direction of the hat, from up clockwise. */
static const uint32_t tb_dualsense_hat[TB_DUALSENSE_HAT_DIRECTIONS] = {
    1u << TB_GAMEPAD_BUTTON_DPAD_UP,
    (1u << TB_GAMEPAD_BUTTON_DPAD_UP) | (1u << TB_GAMEPAD_BUTTON_DPAD_RIGHT),
    1u << TB_GAMEPAD_BUTTON_DPAD_RIGHT,
    (1u << TB_GAMEPAD_BUTTON_DPAD_DOWN) | (1u << TB_GAMEPAD_BUTTON_DPAD_RIGHT),
    1u << TB_GAMEPAD_BUTTON_DPAD_DOWN,
    (1u << TB_GAMEPAD_BUTTON_DPAD_DOWN) | (1u << TB_GAMEPAD_BUTTON_DPAD_LEFT),
    1u << TB_GAMEPAD_BUTTON_DPAD_LEFT,
    (1u << TB_GAMEPAD_BUTTON_DPAD_UP) | (1u << TB_GAMEPAD_BUTTON_DPAD_LEFT),
};

/* ---------------------------------------------------------------------------
 * The output report.
 * ------------------------------------------------------------------------- */

/* The part every output report has, over USB after its ID and over
 * Bluetooth after its ID and two bytes more, and where in it each setting
 * is. */
#define TB_DUALSENSE_COMMON_SIZE 47u
#define TB_DUALSENSE_FLAGS0 0u
#define TB_DUALSENSE_FLAGS1 1u
#define TB_DUALSENSE_MOTOR_RIGHT 2u
#define TB_DUALSENSE_MOTOR_LEFT 3u
#define TB_DUALSENSE_FLAGS2 38u
#define TB_DUALSENSE_LIGHT_SETUP 41u
#define TB_DUALSENSE_PLAYER_LIGHTS 43u
#define TB_DUALSENSE_LIGHT_BAR 44u /* Red, green, blue. */

/* What a report sets, as bits of its three bytes of flags. */
#define TB_DUALSENSE_FLAGS0_VIBRATION 0x01u      /* The two-motor emulation. */
#define TB_DUALSENSE_FLAGS0_HAPTICS_SELECT 0x02u /* Its motors, not audio, drive the actuators. */
#define TB_DUALSENSE_FLAGS1_LIGHT_BAR 0x04u      /* The light bar's color. */
#define TB_DUALSENSE_FLAGS1_PLAYER_LIGHTS 0x10u  /* The player lights. */
#define TB_DUALSENSE_FLAGS2_LIGHT_SETUP 0x02u    /* The light bar's setup. */
#define TB_DUALSENSE_FLAGS2_VIBRATION 0x04u      /* The newer two-motor emulation. */
/* The light bar's setup that ends the firmware's own fade of it, so that
 * the host's color shows. */
#define TB_DUALSENSE_LIGHT_OUT 0x02u

/* Over Bluetooth: the byte after the ID numbers the report in its high
 * four bits, and the next is a tag the pad wants. */
#define TB_DUALSENSE_BLUETOOTH_COMMON 3u
#define TB_DUALSENSE_SEQUENCE_SHIFT 4u
#define TB_DUALSENSE_SEQUENCE_MASK 0x0fu
#define TB_DUALSENSE_BLUETOOTH_TAG 0x10u

/* Bytes of a CRC-32 at a Bluetooth report's end. */
#define TB_DUALSENSE_CRC_SIZE 4u

/* The firmware's update version in its feature report, little-endian, and
 * the first that takes the newer emulation flag: 2.21. */
#define TB_DUALSENSE_FIRMWARE_UPDATE 44u
#define TB_DUALSENSE_VIBRATION_V2_UPDATE 0x0221u

/* The player lights of each player, from the first, as PlayStation
 * consoles light them: the middle one, then two, three and four from the
 * middle out, then all five. Players past the fifth start again. */
static const uint8_t tb_dualsense_player_lights[] = {0x04, 0x0a, 0x15, 0x1b, 0x1f};
#define TB_DUALSENSE_PLAYER_PATTERNS (sizeof(tb_dualsense_player_lights))

/* The light bar's color of each slot, dim: the first four as PlayStation
 * consoles color players 1 to 4 (blue, red, green, pink), then orange,
 * teal, yellow and white. */
static const uint8_t tb_dualsense_colors[TB_GAMEPAD_SLOT_COUNT][3] = {
    {0x00, 0x00, 0x40}, {0x40, 0x00, 0x00}, {0x00, 0x40, 0x00}, {0x20, 0x00, 0x20},
    {0x20, 0x10, 0x00}, {0x00, 0x10, 0x10}, {0x20, 0x20, 0x00}, {0x10, 0x10, 0x10},
};

uint32_t tb_dualsense_crc(uint8_t seed, const uint8_t *report, size_t length)
{
    uint32_t crc = tb_crc32(&seed, 1);

    TB_ASSERT(report != NULL || length == 0);
    return tb_crc32_update(crc, report, length);
}

static uint32_t tb_dualsense_load32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void tb_dualsense_store32(uint32_t value, uint8_t *bytes)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
    bytes[2] = (uint8_t)((value >> 16) & 0xffu);
    bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

bool tb_dualsense_crc_ok(uint8_t seed, const uint8_t *report, size_t size)
{
    TB_ASSERT(report != NULL);
    if (size <= TB_DUALSENSE_CRC_SIZE)
    {
        return false;
    }
    return tb_dualsense_crc(seed, report, size - TB_DUALSENSE_CRC_SIZE) ==
           tb_dualsense_load32(report + size - TB_DUALSENSE_CRC_SIZE);
}

/* An output report around its common part: over USB its ID and the part;
 * over Bluetooth its ID, its number, the tag, the part, zeros and the
 * CRC-32. Returns its length. */
static size_t tb_dualsense_output(tb_dualsense *pad, const uint8_t *common, uint8_t *report,
                                  size_t capacity)
{
    TB_ASSERT_MSG(capacity >= TB_DUALSENSE_OUTPUT_BLUETOOTH_SIZE, "room for a report");
    (void)capacity;
    if (!pad->bluetooth)
    {
        memset(report, 0, TB_DUALSENSE_OUTPUT_USB_SIZE);
        report[0] = TB_DUALSENSE_OUTPUT_USB;
        memcpy(report + 1, common, TB_DUALSENSE_COMMON_SIZE);
        return TB_DUALSENSE_OUTPUT_USB_SIZE;
    }
    memset(report, 0, TB_DUALSENSE_OUTPUT_BLUETOOTH_SIZE);
    report[0] = TB_DUALSENSE_OUTPUT_BLUETOOTH;
    report[1] = (uint8_t)(pad->sequence << TB_DUALSENSE_SEQUENCE_SHIFT);
    report[2] = TB_DUALSENSE_BLUETOOTH_TAG;
    memcpy(report + TB_DUALSENSE_BLUETOOTH_COMMON, common, TB_DUALSENSE_COMMON_SIZE);
    tb_dualsense_store32(
        tb_dualsense_crc(TB_DUALSENSE_SEED_OUTPUT, report,
                         TB_DUALSENSE_OUTPUT_BLUETOOTH_SIZE - TB_DUALSENSE_CRC_SIZE),
        report + TB_DUALSENSE_OUTPUT_BLUETOOTH_SIZE - TB_DUALSENSE_CRC_SIZE);
    pad->sequence = (uint8_t)((pad->sequence + 1u) & TB_DUALSENSE_SEQUENCE_MASK);
    return TB_DUALSENSE_OUTPUT_BLUETOOTH_SIZE;
}

/* ---------------------------------------------------------------------------
 * Starting a pad.
 * ------------------------------------------------------------------------- */

/* Read a feature report of `size` bytes, its CRC checked over Bluetooth. */
static bool tb_dualsense_feature(const tb_dualsense *pad, const tb_hid_link *link, uint8_t id,
                                 uint8_t *report, size_t size)
{
    const size_t received = link->feature(link->context, id, report, size);

    if (received < size || report[0] != id)
    {
        return false;
    }
    return !pad->bluetooth || tb_dualsense_crc_ok(TB_DUALSENSE_SEED_FEATURE, report, size);
}

static void tb_dualsense_start(tb_dualsense *pad, const tb_hid_link *link, bool edge)
{
    uint8_t report[TB_HID_REPORT_CAPACITY] = {0};
    uint8_t common[TB_DUALSENSE_COMMON_SIZE] = {0};

    TB_ASSERT(pad != NULL && link != NULL);
    pad->bluetooth = link->bluetooth;
    pad->edge = edge;
    pad->vibration_v2 = edge;
    if (pad->bluetooth)
    {
        /* Reading it is what turns on the full reports. */
        (void)tb_dualsense_feature(pad, link, TB_DUALSENSE_FEATURE_CALIBRATION, report,
                                   TB_DUALSENSE_CALIBRATION_SIZE);
    }
    if (tb_dualsense_feature(pad, link, TB_DUALSENSE_FEATURE_PAIRING, report,
                             TB_DUALSENSE_PAIRING_SIZE))
    {
        /* The address, its last byte first. */
        for (size_t index = TB_DUALSENSE_ADDRESS_SIZE; index > 0; index--)
        {
            pad->address = (pad->address << 8) | report[index];
        }
    }
    if (!edge && tb_dualsense_feature(pad, link, TB_DUALSENSE_FEATURE_FIRMWARE, report,
                                      TB_DUALSENSE_FIRMWARE_SIZE))
    {
        const uint32_t update = (uint32_t)report[TB_DUALSENSE_FIRMWARE_UPDATE] |
                                ((uint32_t)report[TB_DUALSENSE_FIRMWARE_UPDATE + 1] << 8);
        pad->vibration_v2 = update >= TB_DUALSENSE_VIBRATION_V2_UPDATE;
    }
    /* End the firmware's fade of the light bar, which would otherwise hide
     * the player's color. */
    common[TB_DUALSENSE_FLAGS2] = TB_DUALSENSE_FLAGS2_LIGHT_SETUP;
    common[TB_DUALSENSE_LIGHT_SETUP] = TB_DUALSENSE_LIGHT_OUT;
    (void)link->output(link->context, report,
                       tb_dualsense_output(pad, common, report, sizeof(report)));
}

static void tb_dualsense_start_standard(void *state, const tb_hid_link *link)
{
    tb_dualsense_start(state, link, false);
}

static void tb_dualsense_start_edge(void *state, const tb_hid_link *link)
{
    tb_dualsense_start(state, link, true);
}

static uint64_t tb_dualsense_identity(const void *state)
{
    const tb_dualsense *const pad = state;

    TB_ASSERT(pad != NULL);
    return pad->address;
}

/* ---------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

static uint32_t tb_dualsense_read_bits(const uint8_t *buttons, const tb_dualsense_bit *bits,
                                       size_t count)
{
    uint32_t down = 0;

    for (size_t index = 0; index < count; index++)
    {
        if ((buttons[bits[index].byte] & bits[index].mask) != 0)
        {
            down |= 1u << (unsigned)bits[index].button;
        }
    }
    return down;
}

static void tb_dualsense_read_format(const tb_dualsense *driver, const tb_dualsense_format *format,
                                     const uint8_t *report, tb_gamepad_state *pad)
{
    const uint8_t *const data = report + format->data;
    const uint8_t *const sticks = data + format->sticks;
    const uint8_t *const buttons = data + format->buttons;
    const unsigned hat = buttons[0] & TB_DUALSENSE_HAT_MASK;

    *pad = (tb_gamepad_state){.connected = true};
    pad->buttons = tb_dualsense_read_bits(buttons, tb_dualsense_bits,
                                          sizeof(tb_dualsense_bits) / sizeof(tb_dualsense_bits[0]));
    if (driver->edge && format->back_buttons)
    {
        pad->buttons |= tb_dualsense_read_bits(buttons, tb_dualsense_edge_bits,
                                               sizeof(tb_dualsense_edge_bits) /
                                                   sizeof(tb_dualsense_edge_bits[0]));
    }
    if (hat < TB_DUALSENSE_HAT_DIRECTIONS)
    {
        pad->buttons |= tb_dualsense_hat[hat];
    }
    /* The pad's Y is down positive. */
    pad->axes[TB_GAMEPAD_AXIS_LEFT_X] =
        tb_gamepad_stick_value(sticks[0], 0, TB_DUALSENSE_AXIS_MAX, false);
    pad->axes[TB_GAMEPAD_AXIS_LEFT_Y] =
        tb_gamepad_stick_value(sticks[1], 0, TB_DUALSENSE_AXIS_MAX, true);
    pad->axes[TB_GAMEPAD_AXIS_RIGHT_X] =
        tb_gamepad_stick_value(sticks[2], 0, TB_DUALSENSE_AXIS_MAX, false);
    pad->axes[TB_GAMEPAD_AXIS_RIGHT_Y] =
        tb_gamepad_stick_value(sticks[3], 0, TB_DUALSENSE_AXIS_MAX, true);
    pad->axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER] =
        tb_gamepad_trigger_value(data[format->triggers], 0, TB_DUALSENSE_AXIS_MAX);
    pad->axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER] =
        tb_gamepad_trigger_value(data[format->triggers + 1], 0, TB_DUALSENSE_AXIS_MAX);
}

/* Which format a report is, or NULL for none the driver reads. */
static const tb_dualsense_format *tb_dualsense_format_of(const tb_dualsense *driver,
                                                         const uint8_t *report, size_t length)
{
    if (report[0] == TB_DUALSENSE_REPORT_USB && !driver->bluetooth &&
        length >= TB_DUALSENSE_USB_DATA + TB_DUALSENSE_DATA_SIZE)
    {
        return &tb_dualsense_full_usb;
    }
    if (report[0] == TB_DUALSENSE_REPORT_SIMPLE && driver->bluetooth &&
        length >= TB_DUALSENSE_SIMPLE_SIZE)
    {
        return &tb_dualsense_simple;
    }
    if (report[0] == TB_DUALSENSE_REPORT_BLUETOOTH && driver->bluetooth &&
        length >= TB_DUALSENSE_BLUETOOTH_SIZE &&
        tb_dualsense_crc_ok(TB_DUALSENSE_SEED_INPUT, report, TB_DUALSENSE_BLUETOOTH_SIZE))
    {
        return &tb_dualsense_full_bluetooth;
    }
    return NULL;
}

static bool tb_dualsense_parse(void *state, const uint8_t *report, size_t length,
                               tb_gamepad_state *pad)
{
    const tb_dualsense *const driver = state;
    const tb_dualsense_format *format = NULL;

    TB_ASSERT(driver != NULL && pad != NULL);
    TB_ASSERT(report != NULL || length == 0);
    if (report == NULL || length == 0)
    {
        return false;
    }
    format = tb_dualsense_format_of(driver, report, length);
    if (format == NULL)
    {
        return false;
    }
    tb_dualsense_read_format(driver, format, report, pad);
    return true;
}

/* ---------------------------------------------------------------------------
 * Rumble and lights.
 * ------------------------------------------------------------------------- */

/* A motor's strength, from 0 to 1, as the report's byte. */
static uint8_t tb_dualsense_motor(float strength)
{
    if (!(strength > 0.0f))
    {
        return 0;
    }
    if (strength > 1.0f)
    {
        strength = 1.0f;
    }
    return (uint8_t)(strength * (float)TB_DUALSENSE_AXIS_MAX + 0.5f);
}

static size_t tb_dualsense_rumble(void *state, float low_frequency, float high_frequency,
                                  uint8_t *report, size_t capacity)
{
    tb_dualsense *const pad = state;
    uint8_t common[TB_DUALSENSE_COMMON_SIZE] = {0};

    TB_ASSERT(pad != NULL && report != NULL);
    common[TB_DUALSENSE_FLAGS0] = TB_DUALSENSE_FLAGS0_HAPTICS_SELECT;
    if (pad->vibration_v2)
    {
        common[TB_DUALSENSE_FLAGS2] = TB_DUALSENSE_FLAGS2_VIBRATION;
    }
    else
    {
        common[TB_DUALSENSE_FLAGS0] |= TB_DUALSENSE_FLAGS0_VIBRATION;
    }
    common[TB_DUALSENSE_MOTOR_LEFT] = tb_dualsense_motor(low_frequency);
    common[TB_DUALSENSE_MOTOR_RIGHT] = tb_dualsense_motor(high_frequency);
    return tb_dualsense_output(pad, common, report, capacity);
}

static size_t tb_dualsense_player(void *state, size_t player, uint8_t *report, size_t capacity)
{
    tb_dualsense *const pad = state;
    uint8_t common[TB_DUALSENSE_COMMON_SIZE] = {0};
    const uint8_t *const color = tb_dualsense_colors[player % TB_GAMEPAD_SLOT_COUNT];

    TB_ASSERT(pad != NULL && report != NULL);
    common[TB_DUALSENSE_FLAGS1] = TB_DUALSENSE_FLAGS1_LIGHT_BAR | TB_DUALSENSE_FLAGS1_PLAYER_LIGHTS;
    common[TB_DUALSENSE_PLAYER_LIGHTS] =
        tb_dualsense_player_lights[player % TB_DUALSENSE_PLAYER_PATTERNS];
    memcpy(common + TB_DUALSENSE_LIGHT_BAR, color, 3);
    return tb_dualsense_output(pad, common, report, capacity);
}

static bool tb_dualsense_matches(uint16_t vendor, uint16_t product)
{
    return vendor == TB_DUALSENSE_VENDOR && product == TB_DUALSENSE_PRODUCT;
}

static bool tb_dualsense_edge_matches(uint16_t vendor, uint16_t product)
{
    return vendor == TB_DUALSENSE_VENDOR && product == TB_DUALSENSE_PRODUCT_EDGE;
}

const tb_hid_driver tb_dualsense_driver = {
    .name = "DualSense Wireless Controller",
    .capabilities = TB_GAMEPAD_CAPABILITY_RUMBLE,
    .state_size = sizeof(tb_dualsense),
    .channel_class = {0},
    .rumble_period = 0,
    .matches = tb_dualsense_matches,
    .start = tb_dualsense_start_standard,
    .identity = tb_dualsense_identity,
    .parse = tb_dualsense_parse,
    .rumble = tb_dualsense_rumble,
    .player = tb_dualsense_player,
};

const tb_hid_driver tb_dualsense_edge_driver = {
    .name = "DualSense Edge Wireless Controller",
    .capabilities = TB_GAMEPAD_CAPABILITY_RUMBLE,
    .state_size = sizeof(tb_dualsense),
    .channel_class = {0},
    .rumble_period = 0,
    .matches = tb_dualsense_edge_matches,
    .start = tb_dualsense_start_edge,
    .identity = tb_dualsense_identity,
    .parse = tb_dualsense_parse,
    .rumble = tb_dualsense_rumble,
    .player = tb_dualsense_player,
};
