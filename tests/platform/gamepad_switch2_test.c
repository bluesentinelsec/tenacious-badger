/* The Switch 2 Pro Controller's protocol, with reports and replies of the
 * test's own and reports captured from a pad: the commands that set a pad
 * up and their order, the sticks' calibration read from its flash, the
 * buttons by place, the system's buttons left out, and the rumble report.
 * The driver is standard C, so it is tested on every platform. */
#include "platform/shared/gamepad_switch2.h"

#include "tb_test.h"

#include <string.h>

/* A report captured from a Switch 2 Pro Controller over USB, at rest on a
 * desk, with no button down: the format every Switch 2 controller has
 * (0x05), as Steam had set the pad up, motion included. */
static const uint8_t captured_rest[TB_SWITCH2_REPORT_SIZE] = {
    0x05, 0x38, 0xdd, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x38, 0x84, 0x65, 0x48,
    0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x92, 0x0d, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xea, 0xef, 0xf2, 0x12, 0x09,
    0x00, 0x27, 0x00, 0xbe, 0xfd, 0x5c, 0x10, 0x03, 0x00, 0x05, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00};

/* A factory calibration of a stick, as published for these pads. */
static const uint8_t published_calibration[TB_SWITCH2_CALIBRATION_SIZE] = {
    0xb3, 0x67, 0x83, 0x2e, 0x66, 0x5e, 0x3a, 0x06, 0x5f};

/* ---------------------------------------------------------------------------
 * A pad's vendor channel, as the test plays it.
 * ------------------------------------------------------------------------- */

#define FAKE_COMMAND_LIMIT 32u

typedef struct fake_channel
{
    size_t commands; /* How many were sent. */
    uint8_t sent[FAKE_COMMAND_LIMIT][TB_SWITCH2_COMMAND_CAPACITY];
    size_t answered; /* Commands answered before the channel stops answering. */
    bool refuse;     /* Answer with the done bit clear. */
    bool user_calibration;
    size_t outputs;
} fake_channel;

static fake_channel channel = {0};

static void put_flash(uint32_t address, uint8_t *data)
{
    switch (address)
    {
    case TB_SWITCH2_FLASH_FACTORY_LEFT:
        memcpy(data, published_calibration, sizeof(published_calibration));
        break;
    case TB_SWITCH2_FLASH_FACTORY_RIGHT:
        /* Never written. */
        memset(data, 0xff, TB_SWITCH2_CALIBRATION_SIZE);
        break;
    case TB_SWITCH2_FLASH_USER_RIGHT:
        if (channel.user_calibration)
        {
            /* The magic number, then a calibration centered at 2000 with
             * an extent of 1000 each way. */
            const uint8_t user[] = {0xb2, 0xa1, 0xd0, 0x07, 0x7d, 0xe8,
                                    0x83, 0x3e, 0xe8, 0x83, 0x3e};
            memcpy(data, user, sizeof(user));
        }
        break;
    default:
        break;
    }
}

static size_t fake_command(void *context, const uint8_t *command, size_t length, uint8_t *reply,
                           size_t capacity)
{
    const uint8_t done = (uint8_t)(channel.refuse ? 0x00u : 0x01u);

    (void)context;
    TB_TEST_REQUIRE_UINT(length, <=, TB_SWITCH2_COMMAND_CAPACITY);
    TB_TEST_REQUIRE_UINT(capacity, >=, 48u);
    if (channel.commands < FAKE_COMMAND_LIMIT)
    {
        memcpy(channel.sent[channel.commands], command, length);
    }
    channel.commands++;
    if (channel.commands > channel.answered)
    {
        return 0;
    }
    memset(reply, 0, capacity);
    reply[0] = command[0];
    reply[1] = done;
    reply[3] = command[3];
    if (command[0] == TB_SWITCH2_COMMAND_FLASH)
    {
        const uint8_t size = command[8];
        const uint32_t address = (uint32_t)command[12] | ((uint32_t)command[13] << 8) |
                                 ((uint32_t)command[14] << 16) | ((uint32_t)command[15] << 24);
        reply[8] = size;
        memcpy(reply + 12, command + 12, 4);
        put_flash(address, reply + 16);
        return 16u + size;
    }
    return 12;
}

static bool fake_output(void *context, const uint8_t *report, size_t length)
{
    (void)context;
    (void)report;
    (void)length;
    channel.outputs++;
    return true;
}

static const tb_hid_link fake_link = {
    .context = NULL, .command = fake_command, .output = fake_output};

static void fresh_channel(size_t answered)
{
    channel = (fake_channel){.answered = answered};
}

/* ---------------------------------------------------------------------------
 * Commands.
 * ------------------------------------------------------------------------- */

/* A command is its 8-byte header, host to pad over USB, and its data. */
TB_TEST(platform_gamepad_switch2, commands_have_a_header_and_their_data)
{
    const uint8_t data[] = {0x09, 0x00, 0x00, 0x00};
    const uint8_t expected[] = {0x03, 0x91, 0x00, 0x0a, 0x00, 0x04,
                                0x00, 0x00, 0x09, 0x00, 0x00, 0x00};
    uint8_t out[TB_SWITCH2_COMMAND_CAPACITY] = {0};

    TB_TEST_REQUIRE_UINT(tb_switch2_command(TB_SWITCH2_COMMAND_INIT, TB_SWITCH2_INIT_SELECT_REPORT,
                                            data, sizeof(data), out, sizeof(out)),
                         ==, sizeof(expected));
    TB_TEST_EXPECT_BYTES(out, expected, sizeof(expected));
    TB_TEST_EXPECT_UINT(tb_switch2_command(0x11, 0x01, NULL, 0, out, sizeof(out)), ==, 8u);
    /* What does not fit is not made. */
    TB_TEST_EXPECT_UINT(tb_switch2_command(0x11, 0x01, data, sizeof(data), out, 11), ==, 0u);
    TB_TEST_EXPECT_UINT(tb_switch2_command(0x11, 0x01, NULL, 0, out, 7), ==, 0u);
}

/* A reply answers its command, by ID and subcommand, with the done bit. */
TB_TEST(platform_gamepad_switch2, replies_are_checked)
{
    const uint8_t ok[] = {0x03, 0x01, 0x00, 0x0a, 0x00, 0xf8, 0x00, 0x00};
    const uint8_t refused[] = {0x03, 0x04, 0x00, 0x0a, 0x00, 0xf8, 0x00, 0x00};

    TB_TEST_EXPECT(tb_switch2_reply_ok(ok, sizeof(ok), 0x03, 0x0a));
    TB_TEST_EXPECT(!tb_switch2_reply_ok(ok, sizeof(ok), 0x03, 0x0d));
    TB_TEST_EXPECT(!tb_switch2_reply_ok(ok, sizeof(ok), 0x02, 0x0a));
    TB_TEST_EXPECT(!tb_switch2_reply_ok(ok, 7, 0x03, 0x0a));
    TB_TEST_EXPECT(!tb_switch2_reply_ok(refused, sizeof(refused), 0x03, 0x0a));
}

/* ---------------------------------------------------------------------------
 * Calibration.
 * ------------------------------------------------------------------------- */

/* A calibration is three pairs of 12-bit values: the center, the extent
 * above it, the extent below; flash never written is refused. */
TB_TEST(platform_gamepad_switch2, calibration_is_unpacked)
{
    tb_switch2_stick stick = {0};
    const tb_switch2_stick before = {{1, 2, 3}, {4, 5, 6}};
    uint8_t blank[TB_SWITCH2_CALIBRATION_SIZE] = {0};

    TB_TEST_REQUIRE(tb_switch2_parse_calibration(published_calibration, &stick));
    TB_TEST_EXPECT_INT(stick.x.neutral, ==, 0x7b3);
    TB_TEST_EXPECT_INT(stick.y.neutral, ==, 0x836);
    TB_TEST_EXPECT_INT(stick.x.positive, ==, 0x62e);
    TB_TEST_EXPECT_INT(stick.y.positive, ==, 0x5e6);
    TB_TEST_EXPECT_INT(stick.x.negative, ==, 0x63a);
    TB_TEST_EXPECT_INT(stick.y.negative, ==, 0x5f0);

    stick = before;
    memset(blank, 0xff, sizeof(blank));
    TB_TEST_EXPECT(!tb_switch2_parse_calibration(blank, &stick));
    memset(blank, 0, sizeof(blank));
    TB_TEST_EXPECT(!tb_switch2_parse_calibration(blank, &stick));
    TB_TEST_EXPECT_INT(stick.x.neutral, ==, 1);
    TB_TEST_EXPECT_INT(stick.y.positive, ==, 6);
}

/* Each side of the center scales by its own extent, and past it is 1. */
TB_TEST(platform_gamepad_switch2, axes_scale_by_their_calibration)
{
    const tb_switch2_axis axis = {2000, 1000, 500};
    const tb_switch2_axis empty = {2000, 0, 0};

    TB_TEST_EXPECT_FLOAT(tb_switch2_axis_value(&axis, 2000), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_switch2_axis_value(&axis, 2500), ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_switch2_axis_value(&axis, 2250), ==, 0.5f);
    TB_TEST_EXPECT_FLOAT(tb_switch2_axis_value(&axis, 1500), ==, -0.5f);
    TB_TEST_EXPECT_FLOAT(tb_switch2_axis_value(&axis, 4095), ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_switch2_axis_value(&axis, 0), ==, -1.0f);
    TB_TEST_EXPECT_FLOAT(tb_switch2_axis_value(&empty, 2500), ==, 0.0f);
}

/* ---------------------------------------------------------------------------
 * Setting a pad up.
 * ------------------------------------------------------------------------- */

/* The calibrations are read first, then the features are chosen, rumble
 * set up, the Pro Controller's report chosen, the first player light lit,
 * and the reports started. The factory calibration of the left stick is
 * used; the right's was never written, so it keeps the default. */
TB_TEST(platform_gamepad_switch2, start_sends_the_setup_in_order)
{
    static const struct
    {
        uint8_t command;
        uint8_t subcommand;
        uint8_t first_data;
    } expected[] = {
        {TB_SWITCH2_COMMAND_FLASH, TB_SWITCH2_FLASH_READ, TB_SWITCH2_CALIBRATION_SIZE},
        {TB_SWITCH2_COMMAND_FLASH, TB_SWITCH2_FLASH_READ, TB_SWITCH2_USER_CALIBRATION_SIZE},
        {TB_SWITCH2_COMMAND_FLASH, TB_SWITCH2_FLASH_READ, TB_SWITCH2_CALIBRATION_SIZE},
        {TB_SWITCH2_COMMAND_FLASH, TB_SWITCH2_FLASH_READ, TB_SWITCH2_USER_CALIBRATION_SIZE},
        {TB_SWITCH2_COMMAND_FEATURES, TB_SWITCH2_FEATURES_SET_MASK, 0x23},
        {TB_SWITCH2_COMMAND_FEATURES, TB_SWITCH2_FEATURES_ENABLE, 0x23},
        {TB_SWITCH2_COMMAND_RUMBLE_SETUP, TB_SWITCH2_RUMBLE_SETUP_QUERY, 0x00},
        {TB_SWITCH2_COMMAND_INIT, TB_SWITCH2_INIT_SELECT_REPORT, TB_SWITCH2_REPORT_PRO},
        {TB_SWITCH2_COMMAND_LED, TB_SWITCH2_LED_PATTERN, 0x01},
        {TB_SWITCH2_COMMAND_INIT, TB_SWITCH2_INIT_USB, 0x01},
    };
    const uint32_t addresses[] = {TB_SWITCH2_FLASH_FACTORY_LEFT, TB_SWITCH2_FLASH_USER_LEFT,
                                  TB_SWITCH2_FLASH_FACTORY_RIGHT, TB_SWITCH2_FLASH_USER_RIGHT};
    tb_switch2 pad = {0};

    fresh_channel(SIZE_MAX);
    tb_switch2_driver.start(&pad, &fake_link);
    TB_TEST_REQUIRE_UINT(channel.commands, ==, TB_TEST_COUNT_OF(expected));
    for (size_t index = 0; index < TB_TEST_COUNT_OF(expected); index++)
    {
        TB_TEST_CONTEXT("command %zu", index);
        TB_TEST_EXPECT_HEX(channel.sent[index][0], ==, expected[index].command);
        TB_TEST_EXPECT_HEX(channel.sent[index][1], ==, 0x91);
        TB_TEST_EXPECT_HEX(channel.sent[index][3], ==, expected[index].subcommand);
        TB_TEST_EXPECT_HEX(channel.sent[index][8], ==, expected[index].first_data);
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(addresses); index++)
    {
        const uint8_t *const sent = channel.sent[index];
        TB_TEST_CONTEXT("read %zu", index);
        TB_TEST_EXPECT_HEX(sent[9], ==, 0x7e);
        TB_TEST_EXPECT_HEX((uint32_t)sent[12] | ((uint32_t)sent[13] << 8) |
                               ((uint32_t)sent[14] << 16) | ((uint32_t)sent[15] << 24),
                           ==, addresses[index]);
    }
    TB_TEST_EXPECT(pad.configured);
    TB_TEST_EXPECT_INT(pad.sticks[0].x.neutral, ==, 0x7b3);
    TB_TEST_EXPECT_INT(pad.sticks[1].x.neutral, ==, TB_SWITCH2_STICK_CENTER);
    TB_TEST_EXPECT_INT(pad.sticks[1].y.positive, ==, TB_SWITCH2_STICK_DEFAULT_EXTENT);
    TB_TEST_EXPECT_UINT(channel.outputs, ==, 0u);
}

/* A player's calibration, behind its magic number, wins over the
 * factory's. */
TB_TEST(platform_gamepad_switch2, user_calibration_wins)
{
    tb_switch2 pad = {0};

    fresh_channel(SIZE_MAX);
    channel.user_calibration = true;
    tb_switch2_driver.start(&pad, &fake_link);
    TB_TEST_EXPECT_INT(pad.sticks[1].x.neutral, ==, 2000);
    TB_TEST_EXPECT_INT(pad.sticks[1].y.neutral, ==, 2000);
    TB_TEST_EXPECT_INT(pad.sticks[1].x.positive, ==, 1000);
    TB_TEST_EXPECT_INT(pad.sticks[1].y.negative, ==, 1000);
}

/* Without a channel, as when Steam has it, nothing more is sent after the
 * first read fails, and the sticks keep the default calibration. */
TB_TEST(platform_gamepad_switch2, no_channel_keeps_the_defaults)
{
    tb_switch2 pad = {0};

    fresh_channel(0);
    tb_switch2_driver.start(&pad, &fake_link);
    TB_TEST_EXPECT_UINT(channel.commands, ==, 1u);
    TB_TEST_EXPECT(!pad.configured);
    TB_TEST_EXPECT_INT(pad.sticks[0].x.neutral, ==, TB_SWITCH2_STICK_CENTER);
    TB_TEST_EXPECT_INT(pad.sticks[0].x.negative, ==, TB_SWITCH2_STICK_DEFAULT_EXTENT);

    /* A pad that refuses every command, the same. */
    fresh_channel(SIZE_MAX);
    channel.refuse = true;
    tb_switch2_driver.start(&pad, &fake_link);
    TB_TEST_EXPECT_UINT(channel.commands, ==, 1u);
}

/* A channel that stops answering partway leaves the pad not configured,
 * and the setup goes on to the end. */
TB_TEST(platform_gamepad_switch2, a_failed_step_is_remembered)
{
    tb_switch2 pad = {0};

    fresh_channel(6);
    tb_switch2_driver.start(&pad, &fake_link);
    TB_TEST_EXPECT_UINT(channel.commands, ==, 10u);
    TB_TEST_EXPECT(!pad.configured);
}

/* ---------------------------------------------------------------------------
 * Reports.
 * ------------------------------------------------------------------------- */

static tb_switch2 calibrated_pad(void)
{
    tb_switch2 pad = {0};

    fresh_channel(0);
    tb_switch2_driver.start(&pad, &fake_link);
    return pad;
}

/* The pad at rest, as captured: connected, no button down, the sticks
 * near their centers, the triggers up. */
TB_TEST(platform_gamepad_switch2, captured_report_at_rest)
{
    tb_switch2 pad = calibrated_pad();
    tb_gamepad_state state = {0};

    TB_TEST_REQUIRE(tb_switch2_driver.parse(&pad, captured_rest, sizeof(captured_rest), &state));
    TB_TEST_EXPECT(state.connected);
    TB_TEST_EXPECT_HEX(state.buttons, ==, 0);
    for (size_t axis = 0; axis < TB_GAMEPAD_AXIS_COUNT; axis++)
    {
        TB_TEST_CONTEXT("axis %zu", axis);
        TB_TEST_EXPECT_FLOAT_NEAR(state.axes[axis], 0.0f, 0.1f);
    }
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], ==, 0.0f);
}

typedef struct button_row
{
    uint8_t byte;
    uint8_t mask;
    uint32_t buttons; /* What the layout reads; 0 for a button it leaves out. */
} button_row;

static void expect_rows(tb_switch2 *pad, uint8_t id, const button_row *rows, size_t count)
{
    for (size_t index = 0; index < count; index++)
    {
        uint8_t report[TB_SWITCH2_REPORT_SIZE] = {0};
        tb_gamepad_state state = {0};
        TB_TEST_CONTEXT("report 0x%02x byte %u bit 0x%02x", (unsigned)id,
                        (unsigned)rows[index].byte, (unsigned)rows[index].mask);
        report[0] = id;
        report[rows[index].byte] = rows[index].mask;
        TB_TEST_REQUIRE(tb_switch2_driver.parse(pad, report, sizeof(report), &state));
        TB_TEST_EXPECT_HEX(state.buttons, ==, rows[index].buttons);
    }
}

#define BIT(button) (1u << (unsigned)(button))

/* The Pro Controller's own report: every button by its place, Home,
 * Capture and C left out, ZL and ZR the triggers. */
TB_TEST(platform_gamepad_switch2, pro_report_buttons_by_place)
{
    static const button_row rows[] = {
        {3, 0x01, BIT(TB_GAMEPAD_BUTTON_SOUTH)}, /* B */
        {3, 0x02, BIT(TB_GAMEPAD_BUTTON_EAST)},  /* A */
        {3, 0x04, BIT(TB_GAMEPAD_BUTTON_WEST)},  /* Y */
        {3, 0x08, BIT(TB_GAMEPAD_BUTTON_NORTH)}, /* X */
        {3, 0x10, BIT(TB_GAMEPAD_BUTTON_RIGHT_SHOULDER)},
        {3, 0x20, 0}, /* ZR, a trigger */
        {3, 0x40, BIT(TB_GAMEPAD_BUTTON_START)},
        {3, 0x80, BIT(TB_GAMEPAD_BUTTON_RIGHT_STICK)},
        {4, 0x01, BIT(TB_GAMEPAD_BUTTON_DPAD_DOWN)},
        {4, 0x02, BIT(TB_GAMEPAD_BUTTON_DPAD_RIGHT)},
        {4, 0x04, BIT(TB_GAMEPAD_BUTTON_DPAD_LEFT)},
        {4, 0x08, BIT(TB_GAMEPAD_BUTTON_DPAD_UP)},
        {4, 0x10, BIT(TB_GAMEPAD_BUTTON_LEFT_SHOULDER)},
        {4, 0x20, 0}, /* ZL, a trigger */
        {4, 0x40, BIT(TB_GAMEPAD_BUTTON_BACK)},
        {4, 0x80, BIT(TB_GAMEPAD_BUTTON_LEFT_STICK)},
        {5, 0x01, 0},                                     /* Home */
        {5, 0x02, 0},                                     /* Capture */
        {5, 0x04, BIT(TB_GAMEPAD_BUTTON_RIGHT_PADDLE_1)}, /* GR */
        {5, 0x08, BIT(TB_GAMEPAD_BUTTON_LEFT_PADDLE_1)},  /* GL */
        {5, 0x10, 0},                                     /* C */
    };
    tb_switch2 pad = calibrated_pad();
    uint8_t report[TB_SWITCH2_REPORT_SIZE] = {TB_SWITCH2_REPORT_PRO};
    tb_gamepad_state state = {0};

    expect_rows(&pad, TB_SWITCH2_REPORT_PRO, rows, TB_TEST_COUNT_OF(rows));
    report[3] = 0x20;
    report[4] = 0x20;
    TB_TEST_REQUIRE(tb_switch2_driver.parse(&pad, report, sizeof(report), &state));
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], ==, 1.0f);
}

/* The format every Switch 2 controller has, which Steam selects: the same
 * buttons in other places. */
TB_TEST(platform_gamepad_switch2, common_report_buttons_by_place)
{
    static const button_row rows[] = {
        {5, 0x01, BIT(TB_GAMEPAD_BUTTON_WEST)},  /* Y */
        {5, 0x02, BIT(TB_GAMEPAD_BUTTON_NORTH)}, /* X */
        {5, 0x04, BIT(TB_GAMEPAD_BUTTON_SOUTH)}, /* B */
        {5, 0x08, BIT(TB_GAMEPAD_BUTTON_EAST)},  /* A */
        {5, 0x40, BIT(TB_GAMEPAD_BUTTON_RIGHT_SHOULDER)},
        {5, 0x80, 0}, /* ZR */
        {6, 0x01, BIT(TB_GAMEPAD_BUTTON_BACK)},
        {6, 0x02, BIT(TB_GAMEPAD_BUTTON_START)},
        {6, 0x04, BIT(TB_GAMEPAD_BUTTON_RIGHT_STICK)},
        {6, 0x08, BIT(TB_GAMEPAD_BUTTON_LEFT_STICK)},
        {6, 0x10, 0}, /* Home */
        {6, 0x20, 0}, /* Capture */
        {6, 0x40, 0}, /* C */
        {7, 0x01, BIT(TB_GAMEPAD_BUTTON_DPAD_DOWN)},
        {7, 0x02, BIT(TB_GAMEPAD_BUTTON_DPAD_UP)},
        {7, 0x04, BIT(TB_GAMEPAD_BUTTON_DPAD_RIGHT)},
        {7, 0x08, BIT(TB_GAMEPAD_BUTTON_DPAD_LEFT)},
        {7, 0x40, BIT(TB_GAMEPAD_BUTTON_LEFT_SHOULDER)},
        {7, 0x80, 0},                                     /* ZL */
        {8, 0x01, BIT(TB_GAMEPAD_BUTTON_RIGHT_PADDLE_1)}, /* GR */
        {8, 0x02, BIT(TB_GAMEPAD_BUTTON_LEFT_PADDLE_1)},  /* GL */
    };
    tb_switch2 pad = calibrated_pad();
    uint8_t report[TB_SWITCH2_REPORT_SIZE] = {TB_SWITCH2_REPORT_COMMON};
    tb_gamepad_state state = {0};

    expect_rows(&pad, TB_SWITCH2_REPORT_COMMON, rows, TB_TEST_COUNT_OF(rows));
    report[5] = 0x80;
    TB_TEST_REQUIRE(tb_switch2_driver.parse(&pad, report, sizeof(report), &state));
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], ==, 0.0f);
}

/* Sticks are 12-bit pairs, up positive, scaled by the calibration read. */
TB_TEST(platform_gamepad_switch2, sticks_through_the_calibration)
{
    tb_switch2 pad = {0};
    uint8_t report[TB_SWITCH2_REPORT_SIZE] = {TB_SWITCH2_REPORT_PRO};
    tb_gamepad_state state = {0};

    fresh_channel(SIZE_MAX);
    tb_switch2_driver.start(&pad, &fake_link);
    /* Left: X at its center plus its upper extent, Y at its center less its
     * lower extent. Right (default): X 4095, Y 0. */
    {
        const uint32_t x = 0x7b3 + 0x62e;
        const uint32_t y = 0x836 - 0x5f0;
        report[6] = (uint8_t)(x & 0xffu);
        report[7] = (uint8_t)(((x >> 8) & 0x0fu) | ((y & 0x0fu) << 4));
        report[8] = (uint8_t)(y >> 4);
    }
    report[9] = 0xff;
    report[10] = 0x0f;
    report[11] = 0x00;
    TB_TEST_REQUIRE(tb_switch2_driver.parse(&pad, report, sizeof(report), &state));
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_X], ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_Y], ==, -1.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_X], ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_Y], ==, -1.0f);
}

/* A report of another kind, or too short, is not a state, and changes
 * nothing. */
TB_TEST(platform_gamepad_switch2, other_reports_are_passed_over)
{
    tb_switch2 pad = calibrated_pad();
    uint8_t report[TB_SWITCH2_REPORT_SIZE] = {0x30, 0xff, 0xff, 0xff};
    tb_gamepad_state state = {.buttons = 7};

    TB_TEST_EXPECT(!tb_switch2_driver.parse(&pad, report, sizeof(report), &state));
    report[0] = TB_SWITCH2_REPORT_PRO;
    TB_TEST_EXPECT(!tb_switch2_driver.parse(&pad, report, 11, &state));
    report[0] = TB_SWITCH2_REPORT_COMMON;
    TB_TEST_EXPECT(!tb_switch2_driver.parse(&pad, report, 16, &state));
    TB_TEST_EXPECT(!tb_switch2_driver.parse(&pad, NULL, 0, &state));
    TB_TEST_EXPECT_HEX(state.buttons, ==, 7);
}

/* ---------------------------------------------------------------------------
 * Rumble.
 * ------------------------------------------------------------------------- */

/* The rumble report: ID 0x02, then a sample for each grip at 0x01 and
 * 0x11, each the header 0x50 with the sequence number, the high frequency
 * and its amplitude, the low frequency and its, 10 bits each. Full
 * strength is held to 450 of the pad's 1023. */
TB_TEST(platform_gamepad_switch2, rumble_report_packs_both_grips)
{
    tb_switch2 pad = {0};
    uint8_t report[TB_SWITCH2_REPORT_SIZE] = {0};
    /* High 0x187 at 450 (0x1c2), low 0x112 at 0. */
    const uint8_t high_only[] = {0x50, 0x87, 0x09, 0x27, 0x11, 0x00};
    /* High 0x187 at 0, low 0x112 at 450. */
    const uint8_t low_only[] = {0x51, 0x87, 0x01, 0x20, 0x91, 0x70};
    const uint8_t stopped[] = {0x52, 0x87, 0x01, 0x20, 0x11, 0x00};

    TB_TEST_REQUIRE_UINT(tb_switch2_driver.rumble(&pad, 0.0f, 1.0f, report, sizeof(report)), ==,
                         TB_SWITCH2_REPORT_SIZE);
    TB_TEST_EXPECT_HEX(report[0], ==, TB_SWITCH2_REPORT_RUMBLE);
    TB_TEST_EXPECT_BYTES(report + 0x01, high_only, sizeof(high_only));
    TB_TEST_EXPECT_BYTES(report + 0x11, high_only, sizeof(high_only));
    TB_TEST_EXPECT_HEX(report[0x07], ==, 0);
    TB_TEST_EXPECT_HEX(report[0x3f], ==, 0);
    (void)tb_switch2_driver.rumble(&pad, 2.0f, -1.0f, report, sizeof(report));
    TB_TEST_EXPECT_BYTES(report + 0x01, low_only, sizeof(low_only));
    TB_TEST_EXPECT_BYTES(report + 0x11, low_only, sizeof(low_only));
    (void)tb_switch2_driver.rumble(&pad, 0.0f, 0.0f, report, sizeof(report));
    TB_TEST_EXPECT_BYTES(report + 0x01, stopped, sizeof(stopped));
    /* The sequence number counts to 15 and wraps. */
    for (size_t count = 0; count < 13; count++)
    {
        (void)tb_switch2_driver.rumble(&pad, 0.0f, 0.0f, report, sizeof(report));
    }
    TB_TEST_EXPECT_HEX(report[0x01], ==, 0x5f);
    (void)tb_switch2_driver.rumble(&pad, 0.0f, 0.0f, report, sizeof(report));
    TB_TEST_EXPECT_HEX(report[0x01], ==, 0x50);
}

/* The driver is found by the Pro Controller's IDs, and by no others. */
TB_TEST(platform_gamepad_switch2, driver_is_found_by_its_ids)
{
    TB_TEST_EXPECT_POINTER(tb_hid_driver_find(TB_SWITCH2_VENDOR, TB_SWITCH2_PRODUCT_PRO), ==,
                           &tb_switch2_driver);
    TB_TEST_EXPECT_POINTER(tb_hid_driver_find(TB_SWITCH2_VENDOR, 0x2009), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_hid_driver_find(0x045e, TB_SWITCH2_PRODUCT_PRO), ==, NULL);
    TB_TEST_EXPECT_STRING(tb_switch2_driver.name, ==, "Switch 2 Pro Controller");
    TB_TEST_EXPECT_HEX(tb_switch2_driver.capabilities, ==, TB_GAMEPAD_CAPABILITY_RUMBLE);
    TB_TEST_EXPECT_UINT(tb_switch2_driver.report_size, ==, 64u);
}
