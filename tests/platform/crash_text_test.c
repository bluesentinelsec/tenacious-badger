/* A crash report's first line is built without the C library, by code the
 * adapters share: text, addresses, and integers, cut off where the buffer
 * ends and always terminated, since a crash leaves no second chance to get
 * it right. It is plain C, so it is checked here on every platform. */
#include "crash_text.h"

#include "tb_test.h"

#include <stdint.h>

TB_TEST(platform_crash_text, text_addresses_and_integers_are_written)
{
    char buffer[128] = {0};
    tb_crash_text line = {buffer, sizeof(buffer), 0};

    tb_crash_text_add(&line, "crash: signal ");
    tb_crash_text_add_integer(&line, 11);
    tb_crash_text_add(&line, " at ");
    tb_crash_text_add_address(&line, (uintptr_t)0xBADC0FFEu);
    TB_TEST_EXPECT_STRING(buffer, ==, "crash: signal 11 at 0x00000000badc0ffe");
    TB_TEST_EXPECT_UINT(line.length, ==, 38);
}

TB_TEST(platform_crash_text, integers_of_every_sign_and_size_are_written)
{
    static const struct
    {
        long long value;
        const char *text;
    } cases[] = {
        {0, "0"},
        {7, "7"},
        {-7, "-7"},
        {1234567890, "1234567890"},
        {INT64_MAX, "9223372036854775807"},
        {INT64_MIN, "-9223372036854775808"},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        char buffer[32] = {0};
        tb_crash_text line = {buffer, sizeof(buffer), 0};

        tb_crash_text_add_integer(&line, cases[index].value);
        TB_TEST_EXPECT_STRING(buffer, ==, cases[index].text);
    }
}

/* A line that does not fit is cut, terminated, and never written past. */
TB_TEST(platform_crash_text, a_full_buffer_cuts_the_line_and_stays_terminated)
{
    char buffer[12] = {0};
    char guard = 'G';
    tb_crash_text line = {buffer, sizeof(buffer) - 1, 0};

    buffer[sizeof(buffer) - 1] = guard;
    tb_crash_text_add(&line, "crash: ");
    tb_crash_text_add_address(&line, (uintptr_t)0x1234u);
    tb_crash_text_add_integer(&line, -42);
    TB_TEST_EXPECT_STRING(buffer, ==, "crash: 0x0");
    TB_TEST_EXPECT_UINT(line.length, ==, sizeof(buffer) - 2);
    TB_TEST_EXPECT_UINT((unsigned char)buffer[sizeof(buffer) - 1], ==, (unsigned char)guard);
}
