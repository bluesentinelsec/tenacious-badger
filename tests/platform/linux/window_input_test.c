/* What Linux windowing reads from the mouse that a test cannot have the
 * system send: Wayland's buttons, by their evdev codes, which a headless
 * compositor without a seat never sends; and XInput2's raw motion, whose
 * valuators come packed by a mask. */
#include "platform/linux/window_linux.h"

#include "tb_test.h"

TB_TEST(platform_linux_window, evdev_buttons_are_mouse_buttons)
{
    static const struct
    {
        uint32_t code;
        tb_mouse_button button;
    } rows[] = {
        {0x110, TB_MOUSE_BUTTON_LEFT},   {0x111, TB_MOUSE_BUTTON_RIGHT},
        {0x112, TB_MOUSE_BUTTON_MIDDLE}, {0x113, TB_MOUSE_BUTTON_X1},
        {0x114, TB_MOUSE_BUTTON_X2},     {0x115, TB_MOUSE_BUTTON_NONE},
        {30, TB_MOUSE_BUTTON_NONE},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("code 0x%x", (unsigned)rows[index].code);
        TB_TEST_EXPECT_UINT(tb_linux_button(rows[index].code), ==, rows[index].button);
    }
}

/* Keys come by their evdev codes on X11 and Wayland alike: the keys that
 * edit text among them. */
TB_TEST(platform_linux_window, evdev_codes_are_keys)
{
    static const struct
    {
        uint32_t code;
        tb_key key;
    } rows[] = {
        {30, TB_KEY_A},          {14, TB_KEY_BACKSPACE}, {102, TB_KEY_HOME},
        {107, TB_KEY_END},       {111, TB_KEY_DELETE},   {104, TB_KEY_PAGE_UP},
        {109, TB_KEY_PAGE_DOWN}, {110, TB_KEY_INSERT},   {0x2FF, TB_KEY_UNKNOWN},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("code %u", (unsigned)rows[index].code);
        TB_TEST_EXPECT_UINT(tb_linux_key(rows[index].code), ==, rows[index].key);
    }
}

TB_TEST(platform_linux_window, raw_motion_reads_the_valuators_the_mask_names)
{
    /* x and y, then a wheel's valuator, which is not motion. */
    const unsigned char both[1] = {0x07};
    const double both_values[3] = {4.5, -2.0, 120.0};
    /* y alone: its value is the first. */
    const unsigned char only_y[1] = {0x02};
    const double only_y_values[1] = {3.0};
    double delta_x = 9.0;
    double delta_y = 9.0;

    tb_x11_raw_motion(both, 1, both_values, &delta_x, &delta_y);
    TB_TEST_EXPECT_FLOAT(delta_x, ==, 4.5);
    TB_TEST_EXPECT_FLOAT(delta_y, ==, -2.0);
    tb_x11_raw_motion(only_y, 1, only_y_values, &delta_x, &delta_y);
    TB_TEST_EXPECT_FLOAT(delta_x, ==, 0.0);
    TB_TEST_EXPECT_FLOAT(delta_y, ==, 3.0);
    /* An event without valuators moved nothing. */
    tb_x11_raw_motion(NULL, 0, NULL, &delta_x, &delta_y);
    TB_TEST_EXPECT_FLOAT(delta_x, ==, 0.0);
    TB_TEST_EXPECT_FLOAT(delta_y, ==, 0.0);
}

TB_TEST(platform_linux_window, wayland_key_repeat_follows_the_seat_clock)
{
    tb_wl_repeat repeat = {0};
    const uint64_t ms = UINT64_C(1000000);
    tb_wl_repeat_configure(&repeat, 20, 200, 0);
    tb_wl_repeat_key(&repeat, 30, true, true, 10 * ms);
    TB_TEST_EXPECT_UINT(tb_wl_repeat_wait(&repeat, 20 * ms, UINT64_MAX), ==, 190 * ms);
    TB_TEST_EXPECT_UINT(tb_wl_repeat_wait(&repeat, 20 * ms, ms), ==, ms);
    TB_TEST_EXPECT(!tb_wl_repeat_due(&repeat, 209 * ms));
    TB_TEST_EXPECT(tb_wl_repeat_due(&repeat, 210 * ms));
    tb_wl_repeat_key(&repeat, 42, true, false, 211 * ms);
    tb_wl_repeat_key(&repeat, 42, false, false, 212 * ms);
    TB_TEST_EXPECT_UINT(repeat.key, ==, 30);
    TB_TEST_EXPECT(tb_wl_repeat_due(&repeat, 371 * ms));
    TB_TEST_EXPECT(!tb_wl_repeat_due(&repeat, 371 * ms));
    TB_TEST_EXPECT_UINT(tb_wl_repeat_wait(&repeat, 371 * ms, UINT64_MAX), ==, 39 * ms);
    tb_wl_repeat_key(&repeat, 14, true, true, 372 * ms);
    tb_wl_repeat_key(&repeat, 30, false, false, 373 * ms);
    TB_TEST_EXPECT(tb_wl_repeat_due(&repeat, 572 * ms));
    tb_wl_repeat_key(&repeat, 14, false, false, 573 * ms);
    TB_TEST_EXPECT_UINT(tb_wl_repeat_wait(&repeat, 600 * ms, UINT64_MAX), ==, UINT64_MAX);
    tb_wl_repeat_key(&repeat, 30, true, true, 600 * ms);
    tb_wl_repeat_configure(&repeat, 0, 0, 601 * ms);
    tb_wl_repeat_key(&repeat, 30, true, true, 602 * ms);
    TB_TEST_EXPECT(!tb_wl_repeat_due(&repeat, 1000 * ms));
}
