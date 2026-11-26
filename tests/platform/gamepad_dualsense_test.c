/* The DualSense's protocol, with reports of the test's own and reports
 * captured from a pad: what a pad is asked when it starts, over USB and
 * over Bluetooth; its reports in the layout, buttons by place and the
 * system's buttons left out; the CRC-32 of Bluetooth's reports, and reports
 * whose CRC is wrong dropped; and the output reports of rumble and the
 * lights. The driver is standard C, so it is tested on every platform. */
#include "platform/shared/gamepad_dualsense.h"

#include "tb_test.h"

#include <string.h>

/* An input report captured from a DualSense over USB, at rest on a desk,
 * with no button down. */
static const uint8_t captured_usb_rest[TB_DUALSENSE_USB_SIZE] = {
    0x01, 0x81, 0x7e, 0x81, 0x80, 0x00, 0x00, 0x2a, 0x08, 0x00, 0x00, 0x00, 0x54, 0xfa, 0xcf, 0x75,
    0x05, 0x00, 0x0b, 0x00, 0xff, 0xff, 0xfb, 0xfe, 0x4e, 0x1f, 0x43, 0x06, 0xb7, 0xd1, 0xbc, 0x17,
    0x0f, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x27, 0xe7, 0xbc, 0x17, 0x29, 0x08, 0x00, 0x33, 0x81, 0x64, 0x58, 0xae, 0x2f, 0x3d, 0xce};

/* The same state as Bluetooth's full report carries it: the ID, a byte
 * that numbers the report, the USB report's data, and the CRC-32 of 0xa1
 * and the rest, 0x3abe4e24, as zlib computes it. */
static const uint8_t bluetooth_rest[TB_DUALSENSE_BLUETOOTH_SIZE] = {
    0x31, 0x40, 0x81, 0x7e, 0x81, 0x80, 0x00, 0x00, 0x2a, 0x08, 0x00, 0x00, 0x00, 0x54, 0xfa, 0xcf,
    0x75, 0x05, 0x00, 0x0b, 0x00, 0xff, 0xff, 0xfb, 0xfe, 0x4e, 0x1f, 0x43, 0x06, 0xb7, 0xd1, 0xbc,
    0x17, 0x0f, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x27, 0xe7, 0xbc, 0x17, 0x29, 0x08, 0x00, 0x33, 0x81, 0x64, 0x58, 0xae, 0x2f, 0x3d,
    0xce, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x4e, 0xbe, 0x3a};

/* The firmware feature report captured from the same pad: built on 4 July
 * 2025, update version 0x0630 at byte 44. */
static const uint8_t captured_firmware[TB_DUALSENSE_FIRMWARE_SIZE] = {
    0x20, 0x4a, 0x75, 0x6c, 0x20, 0x20, 0x34, 0x20, 0x32, 0x30, 0x32, 0x35, 0x31, 0x30, 0x3a, 0x33,
    0x38, 0x3a, 0x34, 0x30, 0x03, 0x00, 0x0b, 0x00, 0x11, 0x11, 0x00, 0x00, 0x2a, 0x00, 0x10, 0x01,
    0x00, 0x08, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x06, 0x00, 0x00,
    0x01, 0x00, 0x03, 0x00, 0x10, 0x10, 0x03, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/* A pad's Bluetooth address, as the pairing report gives it, its last byte
 * first. The pad the reports were captured from has its own; this one is
 * made up. */
static const uint8_t made_up_address[TB_DUALSENSE_ADDRESS_SIZE] = {0x66, 0x55, 0x44,
                                                                   0x33, 0x22, 0x11};
#define MADE_UP_IDENTITY 0x112233445566u

/* ---------------------------------------------------------------------------
 * A pad, as the test plays it.
 * ------------------------------------------------------------------------- */

#define FAKE_FEATURE_LIMIT 8u

typedef struct fake_pad
{
    bool bluetooth;
    bool refuse;       /* Every feature report is refused. */
    bool bad_crc;      /* Bluetooth's feature reports end in a wrong CRC. */
    bool old_firmware; /* Update 2.20, before the newer emulation flag. */
    uint8_t asked[FAKE_FEATURE_LIMIT];
    size_t features;
    size_t outputs;
    uint8_t output[TB_HID_REPORT_CAPACITY];
    size_t output_length;
} fake_pad;

static fake_pad fake = {0};

static size_t fake_feature(void *context, uint8_t id, uint8_t *report, size_t capacity)
{
    size_t size = 0;

    (void)context;
    if (fake.features < FAKE_FEATURE_LIMIT)
    {
        fake.asked[fake.features] = id;
    }
    fake.features++;
    if (fake.refuse)
    {
        return 0;
    }
    memset(report, 0, capacity);
    switch (id)
    {
    case TB_DUALSENSE_FEATURE_CALIBRATION:
        size = TB_DUALSENSE_CALIBRATION_SIZE;
        break;
    case TB_DUALSENSE_FEATURE_PAIRING:
        size = TB_DUALSENSE_PAIRING_SIZE;
        memcpy(report + 1, made_up_address, sizeof(made_up_address));
        break;
    case TB_DUALSENSE_FEATURE_FIRMWARE:
        size = TB_DUALSENSE_FIRMWARE_SIZE;
        memcpy(report, captured_firmware, sizeof(captured_firmware));
        if (fake.old_firmware)
        {
            report[44] = 0x20;
            report[45] = 0x02;
        }
        break;
    default:
        return 0;
    }
    TB_TEST_REQUIRE_UINT(capacity, >=, size);
    report[0] = id;
    if (fake.bluetooth)
    {
        uint32_t crc = tb_dualsense_crc(TB_DUALSENSE_SEED_FEATURE, report, size - 4u);
        if (fake.bad_crc)
        {
            crc ^= 1u;
        }
        for (size_t index = 0; index < 4u; index++)
        {
            report[size - 4u + index] = (uint8_t)(crc >> (8u * index));
        }
    }
    /* As Windows gives it: the length of the device's longest. */
    return capacity;
}

static bool fake_output(void *context, const uint8_t *report, size_t length)
{
    (void)context;
    TB_TEST_REQUIRE_UINT(length, <=, sizeof(fake.output));
    fake.outputs++;
    memcpy(fake.output, report, length);
    fake.output_length = length;
    return true;
}

/* Start a pad of a driver over a transport. */
static tb_dualsense start(const tb_hid_driver *driver, bool bluetooth)
{
    tb_dualsense pad = {0};
    const tb_hid_link link = {
        .bluetooth = bluetooth, .output = fake_output, .feature = fake_feature};

    fake.bluetooth = bluetooth;
    driver->start(&pad, &link);
    return pad;
}

static tb_gamepad_state parse(tb_dualsense *pad, const uint8_t *report, size_t length, bool *read)
{
    tb_gamepad_state state = {0};

    *read = tb_dualsense_driver.parse(pad, report, length, &state);
    return state;
}

/* A USB report at rest, with the given bytes of buttons and the hat at
 * rest unless they say otherwise. */
static void usb_report(uint8_t *report, uint8_t buttons0, uint8_t buttons1, uint8_t buttons2)
{
    memcpy(report, captured_usb_rest, sizeof(captured_usb_rest));
    report[8] = buttons0;
    report[9] = buttons1;
    report[10] = buttons2;
}

static uint32_t bit(tb_gamepad_button button)
{
    return 1u << (unsigned)button;
}

/* ---------------------------------------------------------------------------
 * Starting.
 * ------------------------------------------------------------------------- */

/* Over USB the pad is asked its address and its firmware, never its
 * calibration, which Bluetooth alone needs; then the firmware's own fade
 * of the light bar is ended. */
TB_TEST(platform_gamepad_dualsense, start_over_usb)
{
    tb_dualsense pad = {0};

    fake = (fake_pad){0};
    pad = start(&tb_dualsense_driver, false);
    TB_TEST_REQUIRE_UINT(fake.features, ==, 2u);
    TB_TEST_EXPECT_HEX(fake.asked[0], ==, TB_DUALSENSE_FEATURE_PAIRING);
    TB_TEST_EXPECT_HEX(fake.asked[1], ==, TB_DUALSENSE_FEATURE_FIRMWARE);
    TB_TEST_EXPECT(!pad.bluetooth);
    TB_TEST_EXPECT(!pad.edge);
    TB_TEST_EXPECT_HEX(tb_dualsense_driver.identity(&pad), ==, MADE_UP_IDENTITY);
    /* Update 0x0630 is past 2.21. */
    TB_TEST_EXPECT(pad.vibration_v2);
    TB_TEST_REQUIRE_UINT(fake.outputs, ==, 1u);
    TB_TEST_REQUIRE_UINT(fake.output_length, ==, TB_DUALSENSE_OUTPUT_USB_SIZE);
    TB_TEST_EXPECT_HEX(fake.output[0], ==, TB_DUALSENSE_OUTPUT_USB);
    TB_TEST_EXPECT_HEX(fake.output[1 + 38], ==, 0x02); /* The light bar's setup... */
    TB_TEST_EXPECT_HEX(fake.output[1 + 41], ==, 0x02); /* ...ends the fade. */
    TB_TEST_EXPECT_HEX(fake.output[1], ==, 0x00);      /* Nothing else is set. */
    TB_TEST_EXPECT_HEX(fake.output[2], ==, 0x00);
}

/* Over Bluetooth the calibration is read first, which turns on the full
 * reports, and every feature report's CRC is checked; the output report
 * has its number, its tag and its CRC. */
TB_TEST(platform_gamepad_dualsense, start_over_bluetooth)
{
    tb_dualsense pad = {0};

    fake = (fake_pad){0};
    pad = start(&tb_dualsense_driver, true);
    TB_TEST_REQUIRE_UINT(fake.features, ==, 3u);
    TB_TEST_EXPECT_HEX(fake.asked[0], ==, TB_DUALSENSE_FEATURE_CALIBRATION);
    TB_TEST_EXPECT_HEX(fake.asked[1], ==, TB_DUALSENSE_FEATURE_PAIRING);
    TB_TEST_EXPECT_HEX(fake.asked[2], ==, TB_DUALSENSE_FEATURE_FIRMWARE);
    TB_TEST_EXPECT(pad.bluetooth);
    TB_TEST_EXPECT_HEX(pad.address, ==, MADE_UP_IDENTITY);
    TB_TEST_EXPECT(pad.vibration_v2);
    TB_TEST_REQUIRE_UINT(fake.output_length, ==, TB_DUALSENSE_OUTPUT_BLUETOOTH_SIZE);
    TB_TEST_EXPECT_HEX(fake.output[0], ==, TB_DUALSENSE_OUTPUT_BLUETOOTH);
    TB_TEST_EXPECT_HEX(fake.output[1], ==, 0x00); /* The first report. */
    TB_TEST_EXPECT_HEX(fake.output[2], ==, 0x10);
    TB_TEST_EXPECT_HEX(fake.output[3 + 38], ==, 0x02);
    TB_TEST_EXPECT_HEX(fake.output[3 + 41], ==, 0x02);
    /* The CRC of 0xa2 and the rest, as zlib computes it. */
    TB_TEST_EXPECT_HEX(fake.output[74], ==, 0x43);
    TB_TEST_EXPECT_HEX(fake.output[75], ==, 0xe6);
    TB_TEST_EXPECT_HEX(fake.output[76], ==, 0x31);
    TB_TEST_EXPECT_HEX(fake.output[77], ==, 0x36);
    TB_TEST_EXPECT_UINT(pad.sequence, ==, 1u);
}

/* Feature reports whose CRC is wrong, or that the pad refuses, leave the
 * address unknown and the older emulation flag in use. */
TB_TEST(platform_gamepad_dualsense, unread_features_are_not_known)
{
    tb_dualsense pad = {0};

    fake = (fake_pad){.bad_crc = true};
    pad = start(&tb_dualsense_driver, true);
    TB_TEST_EXPECT_HEX(tb_dualsense_driver.identity(&pad), ==, 0u);
    TB_TEST_EXPECT(!pad.vibration_v2);

    fake = (fake_pad){.refuse = true};
    pad = start(&tb_dualsense_driver, false);
    TB_TEST_EXPECT_HEX(tb_dualsense_driver.identity(&pad), ==, 0u);
    TB_TEST_EXPECT(!pad.vibration_v2);
    TB_TEST_EXPECT_UINT(fake.outputs, ==, 1u);

    /* Update 2.20 takes the older flag; every Edge the newer, unasked. */
    fake = (fake_pad){.old_firmware = true};
    pad = start(&tb_dualsense_driver, false);
    TB_TEST_EXPECT(!pad.vibration_v2);
    fake = (fake_pad){.old_firmware = true};
    pad = start(&tb_dualsense_edge_driver, false);
    TB_TEST_EXPECT(pad.edge);
    TB_TEST_EXPECT(pad.vibration_v2);
    TB_TEST_EXPECT_UINT(fake.features, ==, 1u);
}

/* ---------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

/* The captured report: connected, no button down, the sticks near their
 * centers and the triggers at 0. */
TB_TEST(platform_gamepad_dualsense, captured_usb_report_at_rest)
{
    tb_dualsense pad = {0};
    bool read = false;
    const tb_gamepad_state state = parse(&pad, captured_usb_rest, sizeof(captured_usb_rest), &read);

    TB_TEST_REQUIRE(read);
    TB_TEST_EXPECT(state.connected);
    TB_TEST_EXPECT_HEX(state.buttons, ==, 0);
    for (size_t axis = 0; axis < TB_GAMEPAD_AXIS_LEFT_TRIGGER; axis++)
    {
        TB_TEST_CONTEXT("axis %zu", axis);
        TB_TEST_EXPECT_FLOAT_NEAR(state.axes[axis], 0.0f, 0.02f);
    }
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], ==, 0.0f);
}

/* Each button's bit is its button by place; the PS button, the mute button
 * and the touchpad's click are not read, nor are L2's and R2's bits, which
 * the analog triggers give. */
TB_TEST(platform_gamepad_dualsense, buttons_by_place)
{
    static const struct
    {
        uint8_t byte;
        uint8_t mask;
        uint32_t buttons;
    } cases[] = {
        {8, 0x18, 1u << TB_GAMEPAD_BUTTON_WEST},  /* Square, the hat at rest. */
        {8, 0x28, 1u << TB_GAMEPAD_BUTTON_SOUTH}, /* Cross */
        {8, 0x48, 1u << TB_GAMEPAD_BUTTON_EAST},  /* Circle */
        {8, 0x88, 1u << TB_GAMEPAD_BUTTON_NORTH}, /* Triangle */
        {9, 0x01, 1u << TB_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {9, 0x02, 1u << TB_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {9, 0x04, 0},                             /* L2 */
        {9, 0x08, 0},                             /* R2 */
        {9, 0x10, 1u << TB_GAMEPAD_BUTTON_BACK},  /* Create */
        {9, 0x20, 1u << TB_GAMEPAD_BUTTON_START}, /* Options */
        {9, 0x40, 1u << TB_GAMEPAD_BUTTON_LEFT_STICK},
        {9, 0x80, 1u << TB_GAMEPAD_BUTTON_RIGHT_STICK},
        {10, 0x01, 0}, /* PS */
        {10, 0x02, 0}, /* The touchpad's click */
        {10, 0x04, 0}, /* Mute */
        {10, 0xf0, 0}, /* The Edge's, on a DualSense. */
    };
    tb_dualsense pad = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        uint8_t report[TB_DUALSENSE_USB_SIZE] = {0};
        bool read = false;
        tb_gamepad_state state = {0};
        TB_TEST_CONTEXT("byte %u, mask 0x%02x", (unsigned)cases[index].byte,
                        (unsigned)cases[index].mask);
        usb_report(report, 0x08, 0, 0);
        report[cases[index].byte] = cases[index].mask;
        state = parse(&pad, report, sizeof(report), &read);
        TB_TEST_EXPECT(read);
        TB_TEST_EXPECT_HEX(state.buttons, ==, cases[index].buttons);
    }
}

/* The hat's eight directions, from up clockwise, and its rest. */
TB_TEST(platform_gamepad_dualsense, hat_is_the_dpad)
{
    const uint32_t up = bit(TB_GAMEPAD_BUTTON_DPAD_UP);
    const uint32_t down = bit(TB_GAMEPAD_BUTTON_DPAD_DOWN);
    const uint32_t left = bit(TB_GAMEPAD_BUTTON_DPAD_LEFT);
    const uint32_t right = bit(TB_GAMEPAD_BUTTON_DPAD_RIGHT);
    const uint32_t expected[] = {
        up, up | right, right, down | right, down, down | left, left, up | left, 0, 0};
    tb_dualsense pad = {0};

    for (size_t hat = 0; hat < TB_TEST_COUNT_OF(expected); hat++)
    {
        uint8_t report[TB_DUALSENSE_USB_SIZE] = {0};
        bool read = false;
        TB_TEST_CONTEXT("hat %zu", hat);
        usb_report(report, (uint8_t)(hat == 9 ? 0x0f : hat), 0, 0);
        TB_TEST_EXPECT_HEX(parse(&pad, report, sizeof(report), &read).buttons, ==, expected[hat]);
    }
}

/* The sticks reach -1 and 1, up positive, and the triggers 0 to 1. */
TB_TEST(platform_gamepad_dualsense, sticks_and_triggers_scale)
{
    tb_dualsense pad = {0};
    uint8_t report[TB_DUALSENSE_USB_SIZE] = {0};
    bool read = false;
    tb_gamepad_state state = {0};

    usb_report(report, 0x08, 0, 0);
    report[1] = 0x00; /* Left X: left. */
    report[2] = 0x00; /* Left Y: up. */
    report[3] = 0xff; /* Right X: right. */
    report[4] = 0xff; /* Right Y: down. */
    report[5] = 0xff;
    report[6] = 0x80;
    state = parse(&pad, report, sizeof(report), &read);
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_LEFT_X], -1.0f, 0.001f);
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_LEFT_Y], 1.0f, 0.001f);
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_RIGHT_X], 1.0f, 0.001f);
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_RIGHT_Y], -1.0f, 0.001f);
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], 1.0f, 0.001f);
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], 128.0f / 255.0f, 0.001f);
}

/* The Edge's back buttons are the upper paddles; its Fn buttons, which
 * switch its own profiles, are not read. */
TB_TEST(platform_gamepad_dualsense, edge_back_buttons_are_paddles)
{
    tb_dualsense pad = {.edge = true};
    uint8_t report[TB_DUALSENSE_USB_SIZE] = {0};
    tb_gamepad_state state = {0};

    usb_report(report, 0x08, 0, 0x40);
    TB_TEST_REQUIRE(tb_dualsense_edge_driver.parse(&pad, report, sizeof(report), &state));
    TB_TEST_EXPECT_HEX(state.buttons, ==, bit(TB_GAMEPAD_BUTTON_LEFT_PADDLE_1));
    usb_report(report, 0x08, 0, 0x80);
    (void)tb_dualsense_edge_driver.parse(&pad, report, sizeof(report), &state);
    TB_TEST_EXPECT_HEX(state.buttons, ==, bit(TB_GAMEPAD_BUTTON_RIGHT_PADDLE_1));
    usb_report(report, 0x08, 0, 0x30);
    (void)tb_dualsense_edge_driver.parse(&pad, report, sizeof(report), &state);
    TB_TEST_EXPECT_HEX(state.buttons, ==, 0);
}

/* Over Bluetooth, before the calibration is read: the simple report, its
 * buttons after the sticks and its triggers last; the high bits of its
 * third byte count reports, and are no buttons. */
TB_TEST(platform_gamepad_dualsense, bluetooth_simple_report)
{
    tb_dualsense pad = {.bluetooth = true, .edge = true};
    const uint8_t report[TB_DUALSENSE_SIMPLE_SIZE] = {0x01, 0x80, 0x00, 0x80, 0x80,
                                                      0x22, 0x20, 0xfc, 0xff, 0x00};
    bool read = false;
    const tb_gamepad_state state = parse(&pad, report, sizeof(report), &read);

    TB_TEST_REQUIRE(read);
    TB_TEST_EXPECT_HEX(state.buttons, ==,
                       bit(TB_GAMEPAD_BUTTON_SOUTH) | bit(TB_GAMEPAD_BUTTON_DPAD_RIGHT) |
                           bit(TB_GAMEPAD_BUTTON_START));
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_LEFT_Y], 1.0f, 0.001f);
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], 1.0f, 0.001f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], ==, 0.0f);
    /* Too short. */
    (void)parse(&pad, report, sizeof(report) - 1u, &read);
    TB_TEST_EXPECT(!read);
}

/* Over Bluetooth, the full report reads as USB's does, two bytes on; one
 * whose CRC is wrong is dropped. */
TB_TEST(platform_gamepad_dualsense, bluetooth_full_report_checks_its_crc)
{
    tb_dualsense pad = {.bluetooth = true};
    uint8_t report[TB_DUALSENSE_BLUETOOTH_SIZE + 8] = {0};
    tb_gamepad_state state = {0};
    bool read = false;

    TB_TEST_EXPECT(
        tb_dualsense_crc_ok(TB_DUALSENSE_SEED_INPUT, bluetooth_rest, sizeof(bluetooth_rest)));
    /* Followed by zeros, as Windows reads it. */
    memcpy(report, bluetooth_rest, sizeof(bluetooth_rest));
    state = parse(&pad, report, sizeof(report), &read);
    TB_TEST_REQUIRE(read);
    TB_TEST_EXPECT_HEX(state.buttons, ==, 0);
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_LEFT_X], 0.0f, 0.02f);
    report[2 + 7] = 0x28; /* Cross. */
    (void)parse(&pad, report, sizeof(report), &read);
    TB_TEST_EXPECT(!read);
    report[74] ^= 0xffu;
    (void)parse(&pad, report, sizeof(report), &read);
    TB_TEST_EXPECT(!read);
    /* Too short to hold its CRC. */
    (void)parse(&pad, bluetooth_rest, sizeof(bluetooth_rest) - 1u, &read);
    TB_TEST_EXPECT(!read);
}

/* Each transport's reports alone: USB's full report is not Bluetooth's
 * simple one, Bluetooth's full report is not read over USB, and other
 * reports and empty ones are passed over. */
TB_TEST(platform_gamepad_dualsense, reports_of_the_other_transport_are_passed_over)
{
    tb_dualsense usb = {0};
    tb_dualsense bluetooth = {.bluetooth = true};
    const uint8_t other[TB_DUALSENSE_USB_SIZE] = {0x02};
    bool read = false;
    tb_gamepad_state state = {0};

    (void)parse(&usb, bluetooth_rest, sizeof(bluetooth_rest), &read);
    TB_TEST_EXPECT(!read);
    (void)parse(&usb, captured_usb_rest, 10, &read);
    TB_TEST_EXPECT(!read);
    (void)parse(&usb, other, sizeof(other), &read);
    TB_TEST_EXPECT(!read);
    TB_TEST_EXPECT(!tb_dualsense_driver.parse(&usb, NULL, 0, &state));
    /* Over Bluetooth a report 0x01 is the simple one, whatever its length. */
    (void)parse(&bluetooth, captured_usb_rest, sizeof(captured_usb_rest), &read);
    TB_TEST_EXPECT(read);
}

/* ---------------------------------------------------------------------------
 * Rumble and lights.
 * ------------------------------------------------------------------------- */

/* Rumble selects the motors' emulation, with the newer flag where the
 * firmware takes it: the left grip's motor low, the right's high. */
TB_TEST(platform_gamepad_dualsense, rumble_report_over_usb)
{
    tb_dualsense pad = {.vibration_v2 = true};
    uint8_t report[TB_HID_REPORT_CAPACITY] = {0};

    TB_TEST_REQUIRE_UINT(tb_dualsense_driver.rumble(&pad, 1.0f, 0.5f, report, sizeof(report)), ==,
                         TB_DUALSENSE_OUTPUT_USB_SIZE);
    TB_TEST_EXPECT_HEX(report[0], ==, TB_DUALSENSE_OUTPUT_USB);
    TB_TEST_EXPECT_HEX(report[1], ==, 0x02);
    TB_TEST_EXPECT_HEX(report[2], ==, 0x00);
    TB_TEST_EXPECT_HEX(report[3], ==, 0x80); /* Right: high. */
    TB_TEST_EXPECT_HEX(report[4], ==, 0xff); /* Left: low. */
    TB_TEST_EXPECT_HEX(report[1 + 38], ==, 0x04);

    pad.vibration_v2 = false;
    (void)tb_dualsense_driver.rumble(&pad, 2.0f, -1.0f, report, sizeof(report));
    TB_TEST_EXPECT_HEX(report[1], ==, 0x03);
    TB_TEST_EXPECT_HEX(report[3], ==, 0x00);
    TB_TEST_EXPECT_HEX(report[4], ==, 0xff);
    TB_TEST_EXPECT_HEX(report[1 + 38], ==, 0x00);
    (void)tb_dualsense_driver.rumble(&pad, 0.0f, 0.0f, report, sizeof(report));
    TB_TEST_EXPECT_HEX(report[3], ==, 0x00);
    TB_TEST_EXPECT_HEX(report[4], ==, 0x00);
}

/* Over Bluetooth each output report has the next number, from 0 to 15,
 * and a right CRC. */
TB_TEST(platform_gamepad_dualsense, rumble_report_over_bluetooth)
{
    tb_dualsense pad = {.bluetooth = true, .vibration_v2 = true};
    uint8_t report[TB_HID_REPORT_CAPACITY] = {0};

    for (size_t count = 0; count < 17u; count++)
    {
        TB_TEST_CONTEXT("report %zu", count);
        TB_TEST_REQUIRE_UINT(tb_dualsense_driver.rumble(&pad, 0.25f, 0.0f, report, sizeof(report)),
                             ==, TB_DUALSENSE_OUTPUT_BLUETOOTH_SIZE);
        TB_TEST_EXPECT_HEX(report[0], ==, TB_DUALSENSE_OUTPUT_BLUETOOTH);
        TB_TEST_EXPECT_HEX(report[1], ==, (uint8_t)((count % 16u) << 4));
        TB_TEST_EXPECT_HEX(report[2], ==, 0x10);
        TB_TEST_EXPECT_HEX(report[3 + 0], ==, 0x02);
        TB_TEST_EXPECT_HEX(report[3 + 3], ==, 0x40);
        TB_TEST_EXPECT(tb_dualsense_crc_ok(TB_DUALSENSE_SEED_OUTPUT, report,
                                           TB_DUALSENSE_OUTPUT_BLUETOOTH_SIZE));
    }
}

/* A player's lights: the light bar's color and the player lights, and
 * nothing else; players past the fifth light as the first again, with a
 * color of their own. */
TB_TEST(platform_gamepad_dualsense, player_lights)
{
    static const struct
    {
        size_t player;
        uint8_t lights;
        uint8_t color[3];
    } cases[] = {
        {0, 0x04, {0x00, 0x00, 0x40}}, {1, 0x0a, {0x40, 0x00, 0x00}}, {2, 0x15, {0x00, 0x40, 0x00}},
        {3, 0x1b, {0x20, 0x00, 0x20}}, {4, 0x1f, {0x20, 0x10, 0x00}}, {5, 0x04, {0x00, 0x10, 0x10}},
        {7, 0x15, {0x10, 0x10, 0x10}},
    };
    tb_dualsense pad = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        uint8_t report[TB_HID_REPORT_CAPACITY] = {0};
        TB_TEST_CONTEXT("player %zu", cases[index].player);
        TB_TEST_REQUIRE_UINT(
            tb_dualsense_driver.player(&pad, cases[index].player, report, sizeof(report)), ==,
            TB_DUALSENSE_OUTPUT_USB_SIZE);
        TB_TEST_EXPECT_HEX(report[1], ==, 0x00);
        TB_TEST_EXPECT_HEX(report[2], ==, 0x14);
        TB_TEST_EXPECT_HEX(report[1 + 38], ==, 0x00);
        TB_TEST_EXPECT_HEX(report[1 + 43], ==, cases[index].lights);
        TB_TEST_EXPECT_BYTES(report + 1 + 44, cases[index].color, 3);
    }
}

/* ---------------------------------------------------------------------------
 * The drivers.
 * ------------------------------------------------------------------------- */

/* A CRC-32 with a seed is the CRC-32 of the seed and the rest. */
TB_TEST(platform_gamepad_dualsense, crc_has_its_seed_first)
{
    const uint8_t digits[] = {'2', '3', '4', '5', '6', '7', '8', '9'};
    uint8_t short_report[4] = {0};

    TB_TEST_EXPECT_HEX(tb_dualsense_crc('1', digits, sizeof(digits)), ==, 0xcbf43926u);
    TB_TEST_EXPECT(
        !tb_dualsense_crc_ok(TB_DUALSENSE_SEED_INPUT, short_report, sizeof(short_report)));
}

/* Each driver is found by its pad's IDs, and by no others. */
TB_TEST(platform_gamepad_dualsense, drivers_are_found_by_their_ids)
{
    TB_TEST_EXPECT_POINTER(tb_hid_driver_find(TB_DUALSENSE_VENDOR, TB_DUALSENSE_PRODUCT), ==,
                           &tb_dualsense_driver);
    TB_TEST_EXPECT_POINTER(tb_hid_driver_find(TB_DUALSENSE_VENDOR, TB_DUALSENSE_PRODUCT_EDGE), ==,
                           &tb_dualsense_edge_driver);
    /* The DualShock 4, which has reports of its own. */
    TB_TEST_EXPECT_POINTER(tb_hid_driver_find(TB_DUALSENSE_VENDOR, 0x09cc), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_hid_driver_find(0x057e, TB_DUALSENSE_PRODUCT), ==, NULL);
    TB_TEST_EXPECT_STRING(tb_dualsense_driver.name, ==, "DualSense Wireless Controller");
    TB_TEST_EXPECT_STRING(tb_dualsense_edge_driver.name, ==, "DualSense Edge Wireless Controller");
    TB_TEST_EXPECT_HEX(tb_dualsense_driver.capabilities, ==, TB_GAMEPAD_CAPABILITY_RUMBLE);
    TB_TEST_EXPECT_UINT(tb_dualsense_driver.rumble_period, ==, 0u);
}
