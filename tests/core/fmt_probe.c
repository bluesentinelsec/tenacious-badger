/* Formats to the standard streams, which a test case cannot read back.
 * ExpectFormattedOutput.cmake runs this case as a child process and checks
 * what arrived on each stream; it is not registered with CTest directly. */
#include "badger/fmt.h"

#include "tb_test.h"

#include <stdarg.h>

static size_t print_with_list(const char *format, ...)
{
    va_list arguments = {0};
    size_t length = {0};

    va_start(arguments, format);
    length = tb_fmt_vprint(format, arguments);
    length += tb_fmt_vprint_error(format, arguments);
    va_end(arguments);
    return length;
}

TB_TEST(fmt_probe, prints_to_standard_output_and_standard_error)
{
    size_t length = {0};

    length = tb_fmt_print("out: %s #%03d [%#x]\n", "badger", 7, 255u);
    TB_TEST_EXPECT_UINT(length, ==, 24);
    length = tb_fmt_print_error("err: %s #%03d [%#x]\n", "badger", 8, 4096u);
    TB_TEST_EXPECT_UINT(length, ==, 26);
    /* The list forms write the same text to each stream. */
    length = print_with_list("both: %d%%\n", 100);
    TB_TEST_EXPECT_UINT(length, ==, 22);
    /* Output far longer than any internal buffer arrives whole. */
    length = tb_fmt_print("[%300s]\n", "end");
    TB_TEST_EXPECT_UINT(length, ==, 303);
    /* Both streams took everything, and it is delivered before the case ends. */
    TB_TEST_EXPECT(tb_fmt_flush());
}
