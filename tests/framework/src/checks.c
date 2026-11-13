#include "internal.h"

#include <inttypes.h>
#include <string.h>

void tb_test_check_true(const tb_test_site *site, bool passed, const char *condition_text)
{
    if (!tb_test_failure_begin(site, passed))
    {
        return;
    }
    printf("%s(%s) failed\n", site->macro, condition_text);
    tb_test_failure_end(site);
}

void tb_test_check_int(const tb_test_site *site, bool passed, intmax_t left, intmax_t right,
                       const char *left_text, const char *comparison_text, const char *right_text)
{
    if (!tb_test_failure_begin(site, passed))
    {
        return;
    }
    printf("%s(%s, %s, %s) failed with %" PRIdMAX " %s %" PRIdMAX "\n", site->macro, left_text,
           comparison_text, right_text, left, comparison_text, right);
    tb_test_failure_end(site);
}

void tb_test_check_uint(const tb_test_site *site, bool passed, uintmax_t left, uintmax_t right,
                        const char *left_text, const char *comparison_text, const char *right_text)
{
    if (!tb_test_failure_begin(site, passed))
    {
        return;
    }
    printf("%s(%s, %s, %s) failed with %" PRIuMAX " %s %" PRIuMAX "\n", site->macro, left_text,
           comparison_text, right_text, left, comparison_text, right);
    tb_test_failure_end(site);
}

void tb_test_check_hex(const tb_test_site *site, bool passed, uintmax_t left, uintmax_t right,
                       const char *left_text, const char *comparison_text, const char *right_text)
{
    if (!tb_test_failure_begin(site, passed))
    {
        return;
    }
    printf("%s(%s, %s, %s) failed with 0x%" PRIXMAX " %s 0x%" PRIXMAX "\n", site->macro, left_text,
           comparison_text, right_text, left, comparison_text, right);
    tb_test_failure_end(site);
}

void tb_test_check_float(const tb_test_site *site, bool passed, double left, double right,
                         const char *left_text, const char *comparison_text, const char *right_text)
{
    if (!tb_test_failure_begin(site, passed))
    {
        return;
    }
    /* 17 significant digits distinguish any two doubles. */
    printf("%s(%s, %s, %s) failed with %.17g %s %.17g\n", site->macro, left_text, comparison_text,
           right_text, left, comparison_text, right);
    tb_test_failure_end(site);
}

/* Infinities are near only themselves: their difference is not a number. */
static bool tb_test_floats_are_near(double actual, double expected, double tolerance)
{
    double difference = {0};

    if (actual == expected)
    {
        return tolerance >= 0.0;
    }
    difference = actual - expected;
    if (difference < 0.0)
    {
        difference = -difference;
    }
    /* A NaN operand or tolerance makes this comparison false. */
    return difference <= tolerance;
}

void tb_test_check_float_near(const tb_test_site *site, double actual, double expected,
                              double tolerance, const char *actual_text, const char *expected_text,
                              const char *tolerance_text)
{
    const bool passed = tb_test_floats_are_near(actual, expected, tolerance);

    if (!tb_test_failure_begin(site, passed))
    {
        return;
    }
    printf("%s(%s, %s, %s) failed: %.17g is not within %.17g of %.17g\n", site->macro, actual_text,
           expected_text, tolerance_text, actual, tolerance, expected);
    tb_test_failure_end(site);
}

void tb_test_check_pointer(const tb_test_site *site, bool passed, const void *left,
                           const void *right, const char *left_text, const char *comparison_text,
                           const char *right_text)
{
    if (!tb_test_failure_begin(site, passed))
    {
        return;
    }
    printf("%s(%s, %s, %s) failed with %p %s %p\n", site->macro, left_text, comparison_text,
           right_text, left, comparison_text, right);
    tb_test_failure_end(site);
}

int tb_test_compare_strings(const char *left, const char *right)
{
    int order = {0};

    if (left == NULL && right == NULL)
    {
        return 0;
    }
    if (left == NULL)
    {
        return -1;
    }
    if (right == NULL)
    {
        return 1;
    }
    order = strcmp(left, right);
    if (order < 0)
    {
        return -1;
    }
    if (order > 0)
    {
        return 1;
    }
    return 0;
}

void tb_test_check_string(const tb_test_site *site, bool passed, const char *left,
                          const char *right, const char *left_text, const char *comparison_text,
                          const char *right_text)
{
    if (!tb_test_failure_begin(site, passed))
    {
        return;
    }
    printf("%s(%s, %s, %s) failed with ", site->macro, left_text, comparison_text, right_text);
    tb_test_report_string(left);
    printf(" %s ", comparison_text);
    tb_test_report_string(right);
    fputc('\n', stdout);
    if (left != NULL && right != NULL && strcmp(left, right) != 0)
    {
        printf("    first difference at index %zu\n",
               tb_test_report_string_difference(left, right));
    }
    tb_test_failure_end(site);
}

/* Offset of the first differing byte, or `size` when the ranges are equal. */
static size_t tb_test_find_byte_difference(const unsigned char *actual,
                                           const unsigned char *expected, size_t size)
{
    size_t offset = {0};

    while (offset < size && actual[offset] == expected[offset])
    {
        offset++;
    }
    return offset;
}

void tb_test_check_bytes(const tb_test_site *site, const void *actual, const void *expected,
                         size_t size, const char *actual_text, const char *expected_text,
                         const char *size_text)
{
    const unsigned char *const actual_bytes = actual;
    const unsigned char *const expected_bytes = expected;
    const bool has_both_ranges = actual != NULL && expected != NULL;
    size_t difference = {0};

    if (size > 0 && has_both_ranges)
    {
        difference = tb_test_find_byte_difference(actual_bytes, expected_bytes, size);
    }
    if (!tb_test_failure_begin(site, size == 0 || (has_both_ranges && difference == size)))
    {
        return;
    }
    printf("%s(%s, %s, %s) failed: ", site->macro, actual_text, expected_text, size_text);
    if (!has_both_ranges)
    {
        printf("a range of %zu bytes is NULL (actual %p, expected %p)\n", size, actual, expected);
        tb_test_failure_end(site);
        return;
    }
    printf("first difference at offset %zu of %zu\n", difference, size);
    fputs("    actual:   ", stdout);
    tb_test_report_bytes(actual_bytes, size, difference);
    fputs("\n    expected: ", stdout);
    tb_test_report_bytes(expected_bytes, size, difference);
    fputc('\n', stdout);
    tb_test_failure_end(site);
}
