/* The protocol of Nintendo's Switch 2 Pro Controller over USB, in standard
 * C. The facts are from the community's research of the pads
 * (ndeadly's switch2_controller_research) and the drivers that use it,
 * Linux's hid-nintendo and SDL's; the code is the engine's own.
 *
 * Over USB the pad is a composite device. Its HID interface carries input
 * reports and, as output reports, rumble; its vendor interface beside it
 * takes commands, each an 8-byte header and its data, and answers each.
 * The pad sends no input reports until a host has sent it the setup
 * commands: tb_switch2_driver's start sends them on the link's vendor
 * channel. When another program has the channel, as Steam does while it
 * runs, it has set the pad up already, and the pad's reports are read with
 * the sticks' default calibration.
 *
 * **Buttons by place.** B is south, A east, Y west and X north, the places
 * they have on the pad; L and R are the shoulders; ZL and ZR, which are
 * digital, are the triggers at 0 or 1; + is start and - is back; GL and GR,
 * the back buttons, are the upper left and right paddles. Home and Capture
 * belong to the system, as the guide and share buttons do, and the C
 * button opens the console's chat: none of the three is read. */
#include "gamepad_switch2.h"

#include "badger/assert.h"

#include <string.h>

/* A command's direction and transport: host to pad, over USB. */
#define TB_SWITCH2_REQUEST 0x91u
#define TB_SWITCH2_TRANSPORT_USB 0x00u
/* The bit of a reply's second byte that says the command was done. */
#define TB_SWITCH2_REPLY_DONE 0x01u

/* A flash read's request: the length, then a byte the pad wants, then the
 * address; the reply repeats the request before the data. */
#define TB_SWITCH2_FLASH_READ_MARK 0x7eu
#define TB_SWITCH2_FLASH_REQUEST_SIZE 8u
#define TB_SWITCH2_FLASH_DATA_OFFSET (TB_SWITCH2_HEADER_SIZE + TB_SWITCH2_FLASH_REQUEST_SIZE)

/* The magic number before a player's calibration: 0xa1b2, little-endian. */
#define TB_SWITCH2_USER_MAGIC_LOW 0xb2u
#define TB_SWITCH2_USER_MAGIC_HIGH 0xa1u

/* What the pad reports: its buttons, its sticks, and rumble. Leaving out
 * motion keeps its reports small. */
#define TB_SWITCH2_FEATURE_BUTTONS 0x01u
#define TB_SWITCH2_FEATURE_STICKS 0x02u
#define TB_SWITCH2_FEATURE_RUMBLE 0x20u
#define TB_SWITCH2_FEATURES                                                                        \
    (TB_SWITCH2_FEATURE_BUTTONS | TB_SWITCH2_FEATURE_STICKS | TB_SWITCH2_FEATURE_RUMBLE)

/* The player lights: the first, as a console lights for player 1. */
#define TB_SWITCH2_LIGHT_FIRST 0x01u

/* The largest raw value of a stick's axis, and the mask of 12 bits. */
#define TB_SWITCH2_STICK_MAX 4095
#define TB_SWITCH2_LOW_NIBBLE 0x0fu

/* Rumble: the pad has a linear resonant actuator in each grip, driven by
 * a low and a high frequency, each with an amplitude of 10 bits. The two
 * frequencies make the feel of an Xbox pad's two motors, the large one low
 * and the small one high; Linux and SDL use the same. The largest
 * amplitude is held below the pad's 1023, which is strong enough to be
 * uncomfortable and, the drivers warn, to harm the pad. */
#define TB_SWITCH2_RUMBLE_LOW_FREQUENCY 0x112u
#define TB_SWITCH2_RUMBLE_HIGH_FREQUENCY 0x187u
#define TB_SWITCH2_RUMBLE_MAX_AMPLITUDE 450.0f
/* A sample's header: 0x50 and the report's sequence number. */
#define TB_SWITCH2_RUMBLE_HEADER 0x50u
#define TB_SWITCH2_RUMBLE_SEQUENCE_MASK 0x0fu
/* Where each actuator's sample starts in the report: the left, the right. */
#define TB_SWITCH2_RUMBLE_LEFT 0x01u
#define TB_SWITCH2_RUMBLE_RIGHT 0x11u
/* A rumble report lasts some milliseconds: the pad plays what it was sent
 * and then stops, so the backend sends it again every 8 milliseconds while
 * the motors run. Linux sends every 4 and SDL every 12. */
#define TB_SWITCH2_RUMBLE_PERIOD_NANOSECONDS 8000000u

/* A button of a report: the byte, its bit, and the button of the layout. */
typedef struct tb_switch2_bit
{
    uint8_t byte;
    uint8_t mask;
    tb_gamepad_button button;
} tb_switch2_bit;

/* Where a report format has what the layout reads. */
typedef struct tb_switch2_format
{
    uint8_t id;
    size_t length; /* The shortest report that holds all of it. */
    const tb_switch2_bit *bits;
    size_t bit_count;
    tb_switch2_bit left_trigger; /* ZL; its button is not used. */
    tb_switch2_bit right_trigger;
    uint8_t left_stick; /* The first byte of each stick's three. */
    uint8_t right_stick;
} tb_switch2_format;

/* The Pro Controller's report, 0x09. */
static const tb_switch2_bit tb_switch2_pro_bits[] = {
    {3, 0x01, TB_GAMEPAD_BUTTON_SOUTH},          /* B */
    {3, 0x02, TB_GAMEPAD_BUTTON_EAST},           /* A */
    {3, 0x04, TB_GAMEPAD_BUTTON_WEST},           /* Y */
    {3, 0x08, TB_GAMEPAD_BUTTON_NORTH},          /* X */
    {3, 0x10, TB_GAMEPAD_BUTTON_RIGHT_SHOULDER}, /* R */
    {3, 0x40, TB_GAMEPAD_BUTTON_START},          /* + */
    {3, 0x80, TB_GAMEPAD_BUTTON_RIGHT_STICK},
    {4, 0x01, TB_GAMEPAD_BUTTON_DPAD_DOWN},
    {4, 0x02, TB_GAMEPAD_BUTTON_DPAD_RIGHT},
    {4, 0x04, TB_GAMEPAD_BUTTON_DPAD_LEFT},
    {4, 0x08, TB_GAMEPAD_BUTTON_DPAD_UP},
    {4, 0x10, TB_GAMEPAD_BUTTON_LEFT_SHOULDER}, /* L */
    {4, 0x40, TB_GAMEPAD_BUTTON_BACK},          /* - */
    {4, 0x80, TB_GAMEPAD_BUTTON_LEFT_STICK},
    {5, 0x04, TB_GAMEPAD_BUTTON_RIGHT_PADDLE_1}, /* GR */
    {5, 0x08, TB_GAMEPAD_BUTTON_LEFT_PADDLE_1},  /* GL */
    /* 5: 0x01 Home, 0x02 Capture, 0x10 C: the system's. */
};

/* The report every Switch 2 controller has, 0x05. */
static const tb_switch2_bit tb_switch2_common_bits[] = {
    {5, 0x01, TB_GAMEPAD_BUTTON_WEST},           /* Y */
    {5, 0x02, TB_GAMEPAD_BUTTON_NORTH},          /* X */
    {5, 0x04, TB_GAMEPAD_BUTTON_SOUTH},          /* B */
    {5, 0x08, TB_GAMEPAD_BUTTON_EAST},           /* A */
    {5, 0x40, TB_GAMEPAD_BUTTON_RIGHT_SHOULDER}, /* R */
    {6, 0x01, TB_GAMEPAD_BUTTON_BACK},           /* - */
    {6, 0x02, TB_GAMEPAD_BUTTON_START},          /* + */
    {6, 0x04, TB_GAMEPAD_BUTTON_RIGHT_STICK},
    {6, 0x08, TB_GAMEPAD_BUTTON_LEFT_STICK},
    /* 6: 0x10 Home, 0x20 Capture, 0x40 C: the system's. */
    {7, 0x01, TB_GAMEPAD_BUTTON_DPAD_DOWN},
    {7, 0x02, TB_GAMEPAD_BUTTON_DPAD_UP},
    {7, 0x04, TB_GAMEPAD_BUTTON_DPAD_RIGHT},
    {7, 0x08, TB_GAMEPAD_BUTTON_DPAD_LEFT},
    {7, 0x40, TB_GAMEPAD_BUTTON_LEFT_SHOULDER},  /* L */
    {8, 0x01, TB_GAMEPAD_BUTTON_RIGHT_PADDLE_1}, /* GR */
    {8, 0x02, TB_GAMEPAD_BUTTON_LEFT_PADDLE_1},  /* GL */
};

static const tb_switch2_format tb_switch2_formats[] = {
    {
        .id = TB_SWITCH2_REPORT_PRO,
        .length = 12,
        .bits = tb_switch2_pro_bits,
        .bit_count = sizeof(tb_switch2_pro_bits) / sizeof(tb_switch2_pro_bits[0]),
        .left_trigger = {4, 0x20, TB_GAMEPAD_BUTTON_COUNT},
        .right_trigger = {3, 0x20, TB_GAMEPAD_BUTTON_COUNT},
        .left_stick = 6,
        .right_stick = 9,
    },
    {
        .id = TB_SWITCH2_REPORT_COMMON,
        .length = 17,
        .bits = tb_switch2_common_bits,
        .bit_count = sizeof(tb_switch2_common_bits) / sizeof(tb_switch2_common_bits[0]),
        .left_trigger = {7, 0x80, TB_GAMEPAD_BUTTON_COUNT},
        .right_trigger = {5, 0x80, TB_GAMEPAD_BUTTON_COUNT},
        .left_stick = 11,
        .right_stick = 14,
    },
};

/* ---------------------------------------------------------------------------
 * Commands.
 * ------------------------------------------------------------------------- */

size_t tb_switch2_command(uint8_t command, uint8_t subcommand, const uint8_t *data, size_t length,
                          uint8_t *out, size_t capacity)
{
    const uint8_t header[TB_SWITCH2_HEADER_SIZE] = {
        command, TB_SWITCH2_REQUEST, TB_SWITCH2_TRANSPORT_USB, subcommand, 0, (uint8_t)length, 0,
        0};

    TB_ASSERT(out != NULL);
    TB_ASSERT(data != NULL || length == 0);
    if (capacity < TB_SWITCH2_HEADER_SIZE || length > capacity - TB_SWITCH2_HEADER_SIZE ||
        length > UINT8_MAX)
    {
        return 0;
    }
    memcpy(out, header, sizeof(header));
    if (length != 0)
    {
        memcpy(out + TB_SWITCH2_HEADER_SIZE, data, length);
    }
    return TB_SWITCH2_HEADER_SIZE + length;
}

bool tb_switch2_reply_ok(const uint8_t *reply, size_t length, uint8_t command, uint8_t subcommand)
{
    TB_ASSERT(reply != NULL || length == 0);
    return length >= TB_SWITCH2_HEADER_SIZE && reply[0] == command && reply[3] == subcommand &&
           (reply[1] & TB_SWITCH2_REPLY_DONE) != 0;
}

/* Send a command and check its reply; the reply is copied out, when asked,
 * with its length. */
static bool tb_switch2_send(const tb_hid_link *link, uint8_t command, uint8_t subcommand,
                            const uint8_t *data, size_t length, uint8_t *reply, size_t *received)
{
    uint8_t message[TB_SWITCH2_COMMAND_CAPACITY] = {0};
    uint8_t answer[TB_SWITCH2_COMMAND_CAPACITY] = {0};
    const size_t size =
        tb_switch2_command(command, subcommand, data, length, message, sizeof(message));
    size_t got = 0;

    TB_ASSERT(size != 0);
    got = link->command(link->context, message, size, answer, sizeof(answer));
    if (got > sizeof(answer) || !tb_switch2_reply_ok(answer, got, command, subcommand))
    {
        return false;
    }
    if (reply != NULL)
    {
        memcpy(reply, answer, got);
        *received = got;
    }
    return true;
}

/* Read `size` bytes of the pad's flash. */
static bool tb_switch2_read_flash(const tb_hid_link *link, uint32_t address, uint8_t size,
                                  uint8_t *data)
{
    const uint8_t request[TB_SWITCH2_FLASH_REQUEST_SIZE] = {size,
                                                            TB_SWITCH2_FLASH_READ_MARK,
                                                            0,
                                                            0,
                                                            (uint8_t)(address & 0xffu),
                                                            (uint8_t)((address >> 8) & 0xffu),
                                                            (uint8_t)((address >> 16) & 0xffu),
                                                            (uint8_t)((address >> 24) & 0xffu)};
    uint8_t reply[TB_SWITCH2_COMMAND_CAPACITY] = {0};
    size_t received = 0;

    if (!tb_switch2_send(link, TB_SWITCH2_COMMAND_FLASH, TB_SWITCH2_FLASH_READ, request,
                         sizeof(request), reply, &received))
    {
        return false;
    }
    /* The reply repeats the length and the address before the data. */
    if (received < TB_SWITCH2_FLASH_DATA_OFFSET + size || reply[TB_SWITCH2_HEADER_SIZE] != size ||
        memcmp(reply + TB_SWITCH2_HEADER_SIZE + 4, request + 4, 4) != 0)
    {
        return false;
    }
    memcpy(data, reply + TB_SWITCH2_FLASH_DATA_OFFSET, size);
    return true;
}

/* ---------------------------------------------------------------------------
 * Calibration.
 * ------------------------------------------------------------------------- */

/* The two 12-bit values packed in three bytes: the first in the low 12
 * bits. */
static void tb_switch2_unpack(const uint8_t *bytes, int32_t *first, int32_t *second)
{
    *first = (int32_t)((uint32_t)bytes[0] | (((uint32_t)bytes[1] & TB_SWITCH2_LOW_NIBBLE) << 8));
    *second = (int32_t)(((uint32_t)bytes[1] >> 4) | ((uint32_t)bytes[2] << 4));
}

static bool tb_switch2_axis_valid(const tb_switch2_axis *axis)
{
    return axis->neutral > 0 && axis->neutral < TB_SWITCH2_STICK_MAX && axis->negative > 0 &&
           axis->positive > 0;
}

bool tb_switch2_parse_calibration(const uint8_t *data, tb_switch2_stick *stick)
{
    tb_switch2_stick read = {0};

    TB_ASSERT(data != NULL && stick != NULL);
    /* The center, then how far the stick reaches above it, then below. */
    tb_switch2_unpack(data, &read.x.neutral, &read.y.neutral);
    tb_switch2_unpack(data + 3, &read.x.positive, &read.y.positive);
    tb_switch2_unpack(data + 6, &read.x.negative, &read.y.negative);
    /* Flash that was never written is all 0xff, which reads as a center
     * at the stick's edge. */
    if (!tb_switch2_axis_valid(&read.x) || !tb_switch2_axis_valid(&read.y))
    {
        return false;
    }
    *stick = read;
    return true;
}

float tb_switch2_axis_value(const tb_switch2_axis *axis, int32_t raw)
{
    const int32_t offset = raw - axis->neutral;
    const int32_t extent = offset >= 0 ? axis->positive : axis->negative;
    float value = 0.0f;

    if (extent <= 0)
    {
        return 0.0f;
    }
    value = (float)offset / (float)extent;
    if (value > 1.0f)
    {
        return 1.0f;
    }
    if (value < -1.0f)
    {
        return -1.0f;
    }
    return value;
}

static void tb_switch2_default_calibration(tb_switch2 *pad)
{
    const tb_switch2_axis axis = {TB_SWITCH2_STICK_CENTER, TB_SWITCH2_STICK_DEFAULT_EXTENT,
                                  TB_SWITCH2_STICK_DEFAULT_EXTENT};

    for (size_t stick = 0; stick < 2; stick++)
    {
        pad->sticks[stick] = (tb_switch2_stick){axis, axis};
    }
}

/* A stick's calibration from the flash: the player's where a console wrote
 * one, else the factory's, else the default. Whether the channel answered. */
static bool tb_switch2_read_stick(const tb_hid_link *link, uint32_t factory, uint32_t user,
                                  tb_switch2_stick *stick)
{
    uint8_t data[TB_SWITCH2_USER_CALIBRATION_SIZE] = {0};

    if (!tb_switch2_read_flash(link, factory, TB_SWITCH2_CALIBRATION_SIZE, data))
    {
        return false;
    }
    (void)tb_switch2_parse_calibration(data, stick);
    if (tb_switch2_read_flash(link, user, TB_SWITCH2_USER_CALIBRATION_SIZE, data) &&
        data[0] == TB_SWITCH2_USER_MAGIC_LOW && data[1] == TB_SWITCH2_USER_MAGIC_HIGH)
    {
        (void)tb_switch2_parse_calibration(data + 2, stick);
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * The driver.
 * ------------------------------------------------------------------------- */

/* The commands that make the pad report, in order, after its calibration
 * is read. */
typedef struct tb_switch2_step
{
    uint8_t command;
    uint8_t subcommand;
    uint8_t data[8];
    size_t length;
} tb_switch2_step;

static const tb_switch2_step tb_switch2_setup[] = {
    /* The features its reports carry, and which of them are on. */
    {TB_SWITCH2_COMMAND_FEATURES, TB_SWITCH2_FEATURES_SET_MASK, {TB_SWITCH2_FEATURES}, 4},
    {TB_SWITCH2_COMMAND_FEATURES, TB_SWITCH2_FEATURES_ENABLE, {TB_SWITCH2_FEATURES}, 4},
    /* A query whose answer is not known, which rumble needs to run
     * reliably. */
    {TB_SWITCH2_COMMAND_RUMBLE_SETUP, TB_SWITCH2_RUMBLE_SETUP_QUERY, {0}, 0},
    {TB_SWITCH2_COMMAND_INIT, TB_SWITCH2_INIT_SELECT_REPORT, {TB_SWITCH2_REPORT_PRO}, 4},
    {TB_SWITCH2_COMMAND_LED, TB_SWITCH2_LED_PATTERN, {TB_SWITCH2_LIGHT_FIRST}, 8},
    /* Start the reports. The last six bytes are the host's Bluetooth
     * address, which USB does not need. */
    {TB_SWITCH2_COMMAND_INIT,
     TB_SWITCH2_INIT_USB,
     {0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
     8},
};

static void tb_switch2_start(void *state, const tb_hid_link *link)
{
    tb_switch2 *const pad = state;

    TB_ASSERT(pad != NULL && link != NULL);
    tb_switch2_default_calibration(pad);
    /* No answer to the first read: there is no channel, or another program
     * has it and has set the pad up already. */
    if (!tb_switch2_read_stick(link, TB_SWITCH2_FLASH_FACTORY_LEFT, TB_SWITCH2_FLASH_USER_LEFT,
                               &pad->sticks[0]))
    {
        return;
    }
    (void)tb_switch2_read_stick(link, TB_SWITCH2_FLASH_FACTORY_RIGHT, TB_SWITCH2_FLASH_USER_RIGHT,
                                &pad->sticks[1]);
    pad->configured = true;
    for (size_t index = 0; index < sizeof(tb_switch2_setup) / sizeof(tb_switch2_setup[0]); index++)
    {
        const tb_switch2_step *const step = &tb_switch2_setup[index];
        if (!tb_switch2_send(link, step->command, step->subcommand, step->data, step->length, NULL,
                             NULL))
        {
            pad->configured = false;
        }
    }
}

static bool tb_switch2_down(const uint8_t *report, const tb_switch2_bit *bit)
{
    return (report[bit->byte] & bit->mask) != 0;
}

static void tb_switch2_read_stick_values(const tb_switch2_stick *stick, const uint8_t *bytes,
                                         float *x, float *y)
{
    int32_t raw_x = 0;
    int32_t raw_y = 0;

    /* The pad's Y is up positive, as the layout's is. */
    tb_switch2_unpack(bytes, &raw_x, &raw_y);
    *x = tb_switch2_axis_value(&stick->x, raw_x);
    *y = tb_switch2_axis_value(&stick->y, raw_y);
}

static bool tb_switch2_read(void *state, const uint8_t *report, size_t length,
                            tb_gamepad_state *pad)
{
    const tb_switch2 *const driver = state;
    const tb_switch2_format *format = NULL;

    TB_ASSERT(driver != NULL && pad != NULL);
    TB_ASSERT(report != NULL || length == 0);
    if (report == NULL || length == 0)
    {
        return false;
    }
    for (size_t index = 0; index < sizeof(tb_switch2_formats) / sizeof(tb_switch2_formats[0]);
         index++)
    {
        if (length >= tb_switch2_formats[index].length && report[0] == tb_switch2_formats[index].id)
        {
            format = &tb_switch2_formats[index];
        }
    }
    if (format == NULL)
    {
        return false;
    }
    *pad = (tb_gamepad_state){.connected = true};
    for (size_t index = 0; index < format->bit_count; index++)
    {
        if (tb_switch2_down(report, &format->bits[index]))
        {
            pad->buttons |= 1u << (unsigned)format->bits[index].button;
        }
    }
    pad->axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER] =
        tb_switch2_down(report, &format->left_trigger) ? 1.0f : 0.0f;
    pad->axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER] =
        tb_switch2_down(report, &format->right_trigger) ? 1.0f : 0.0f;
    tb_switch2_read_stick_values(&driver->sticks[0], report + format->left_stick,
                                 &pad->axes[TB_GAMEPAD_AXIS_LEFT_X],
                                 &pad->axes[TB_GAMEPAD_AXIS_LEFT_Y]);
    tb_switch2_read_stick_values(&driver->sticks[1], report + format->right_stick,
                                 &pad->axes[TB_GAMEPAD_AXIS_RIGHT_X],
                                 &pad->axes[TB_GAMEPAD_AXIS_RIGHT_Y]);
    return true;
}

/* A motor's strength, from 0 to 1, as an amplitude of 10 bits. */
static uint32_t tb_switch2_amplitude(float strength)
{
    if (!(strength > 0.0f))
    {
        return 0;
    }
    if (strength > 1.0f)
    {
        strength = 1.0f;
    }
    return (uint32_t)(strength * TB_SWITCH2_RUMBLE_MAX_AMPLITUDE + 0.5f);
}

/* An actuator's sample: the header, then the high frequency and its
 * amplitude and the low frequency and its, 10 bits each, packed. */
static void tb_switch2_sample(uint8_t sequence, uint32_t low, uint32_t high, uint8_t *out)
{
    const uint32_t high_frequency = TB_SWITCH2_RUMBLE_HIGH_FREQUENCY;
    const uint32_t low_frequency = TB_SWITCH2_RUMBLE_LOW_FREQUENCY;

    out[0] = (uint8_t)(TB_SWITCH2_RUMBLE_HEADER | sequence);
    out[1] = (uint8_t)(high_frequency & 0xffu);
    out[2] = (uint8_t)(((high_frequency >> 8) | (high << 2)) & 0xffu);
    out[3] = (uint8_t)(((high >> 6) | (low_frequency << 4)) & 0xffu);
    out[4] = (uint8_t)(((low_frequency >> 4) | (low << 6)) & 0xffu);
    out[5] = (uint8_t)((low >> 2) & 0xffu);
}

static size_t tb_switch2_rumble(void *state, float low_frequency, float high_frequency,
                                uint8_t *report, size_t capacity)
{
    tb_switch2 *const pad = state;
    const uint32_t low = tb_switch2_amplitude(low_frequency);
    const uint32_t high = tb_switch2_amplitude(high_frequency);

    TB_ASSERT(pad != NULL && report != NULL);
    TB_ASSERT_MSG(capacity >= TB_SWITCH2_REPORT_SIZE, "room for a report");
    (void)capacity;
    memset(report, 0, TB_SWITCH2_REPORT_SIZE);
    report[0] = TB_SWITCH2_REPORT_RUMBLE;
    /* The same to both grips: each plays both frequencies. */
    tb_switch2_sample(pad->sequence, low, high, report + TB_SWITCH2_RUMBLE_LEFT);
    tb_switch2_sample(pad->sequence, low, high, report + TB_SWITCH2_RUMBLE_RIGHT);
    pad->sequence = (uint8_t)((pad->sequence + 1u) & TB_SWITCH2_RUMBLE_SEQUENCE_MASK);
    return TB_SWITCH2_REPORT_SIZE;
}

static bool tb_switch2_matches(uint16_t vendor, uint16_t product)
{
    return vendor == TB_SWITCH2_VENDOR && product == TB_SWITCH2_PRODUCT_PRO;
}

const tb_hid_driver tb_switch2_driver = {
    .name = "Switch 2 Pro Controller",
    .capabilities = TB_GAMEPAD_CAPABILITY_RUMBLE,
    .state_size = sizeof(tb_switch2),
    .report_size = TB_SWITCH2_REPORT_SIZE,
    /* {6F13725E-EF0E-4FD3-AE5F-B2DE989EC825}, from the pad's Microsoft OS
     * descriptors, which bind its vendor interface to WinUSB. */
    .channel_class = {0x6f13725eu,
                      0xef0eu,
                      0x4fd3u,
                      {0xae, 0x5f, 0xb2, 0xde, 0x98, 0x9e, 0xc8, 0x25}},
    .rumble_period = TB_SWITCH2_RUMBLE_PERIOD_NANOSECONDS,
    .matches = tb_switch2_matches,
    .start = tb_switch2_start,
    .parse = tb_switch2_read,
    .rumble = tb_switch2_rumble,
};
