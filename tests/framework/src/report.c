#include "internal.h"

/* Longest prefix of a string that a failure report prints before eliding. */
#define TB_TEST_REPORTED_STRING_LIMIT ((size_t)200)

/* Bytes shown on each side of the first difference in a byte-range report. */
#define TB_TEST_REPORTED_BYTES_RADIUS ((size_t)8)

static void tb_test_report_character(unsigned char character)
{
    switch (character)
    {
    case '\n':
        fputs("\\n", stdout);
        return;
    case '\r':
        fputs("\\r", stdout);
        return;
    case '\t':
        fputs("\\t", stdout);
        return;
    case '"':
        fputs("\\\"", stdout);
        return;
    case '\\':
        fputs("\\\\", stdout);
        return;
    default:
        break;
    }
    /* Printable ASCII is shown as written; everything else as a hex escape so
     * invisible and non-ASCII bytes cannot hide a difference. */
    if (character >= 0x20 && character < 0x7F)
    {
        fputc(character, stdout);
        return;
    }
    printf("\\x%02X", (unsigned int)character);
}

void tb_test_report_string(const char *text)
{
    size_t index = {0};

    if (text == NULL)
    {
        fputs("(null)", stdout);
        return;
    }
    fputc('"', stdout);
    while (text[index] != '\0' && index < TB_TEST_REPORTED_STRING_LIMIT)
    {
        tb_test_report_character((unsigned char)text[index]);
        index++;
    }
    fputc('"', stdout);
    if (text[index] != '\0')
    {
        fputs("...", stdout);
    }
}

size_t tb_test_report_string_difference(const char *left, const char *right)
{
    size_t index = {0};

    while (left[index] != '\0' && left[index] == right[index])
    {
        index++;
    }
    return index;
}

void tb_test_report_bytes(const unsigned char *bytes, size_t size, size_t focus)
{
    size_t first = {0};
    size_t last = {0};

    if (focus > TB_TEST_REPORTED_BYTES_RADIUS)
    {
        first = focus - TB_TEST_REPORTED_BYTES_RADIUS;
    }
    last = size - 1;
    if (last - focus > TB_TEST_REPORTED_BYTES_RADIUS)
    {
        last = focus + TB_TEST_REPORTED_BYTES_RADIUS;
    }

    if (first > 0)
    {
        fputs(".. ", stdout);
    }
    for (size_t index = first; index <= last; index++)
    {
        if (index == focus)
        {
            printf("[%02X] ", (unsigned int)bytes[index]);
            continue;
        }
        printf("%02X ", (unsigned int)bytes[index]);
    }
    if (last < size - 1)
    {
        fputs("..", stdout);
    }
}
