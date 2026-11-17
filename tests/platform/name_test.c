/* The adapters fit a thread's name into buffers of fixed size, the engine's
 * and each system's. A name that is too long must be cut between characters:
 * half of a UTF-8 character is not text, and a debugger would show it as
 * garbage. The copying is shared by the adapters and is plain C, so it is
 * checked here on every platform. */
#include "name.h"

#include "tb_test.h"

#include <stddef.h>

/* A byte that a correct copy never writes. */
#define UNTOUCHED ((char)0x7E)

#define BUFFER_SIZE 16

typedef struct name_case
{
    const char *name;
    size_t capacity;
    const char *kept;
} name_case;

/* "é" is two bytes, "€" three, and the badger four. */
static const name_case name_cases[] = {
    {"worker", 16, "worker"},
    {"worker", 7, "worker"},
    {"worker", 6, "worke"},
    {"worker", 2, "w"},
    {"worker", 1, ""},
    {"", 16, ""},
    {"", 1, ""},
    /* Two bytes, cut after the first of them. */
    {"caf\xC3\xA9", 6, "caf\xC3\xA9"},
    {"caf\xC3\xA9", 5, "caf"},
    {"caf\xC3\xA9s", 5, "caf"},
    {"\xC3\xA9", 2, ""},
    /* Three bytes, cut after the first and after the second. */
    {"a\xE2\x82\xACz", 5, "a\xE2\x82\xAC"},
    {"a\xE2\x82\xACz", 4, "a"},
    {"a\xE2\x82\xACz", 3, "a"},
    {"a\xE2\x82\xACz", 2, "a"},
    /* Four bytes, cut at each place inside. */
    {"\xF0\x9F\xA6\xA1!", 6, "\xF0\x9F\xA6\xA1!"},
    {"\xF0\x9F\xA6\xA1!", 5, "\xF0\x9F\xA6\xA1"},
    {"\xF0\x9F\xA6\xA1!", 4, ""},
    {"\xF0\x9F\xA6\xA1!", 3, ""},
    {"\xF0\x9F\xA6\xA1!", 2, ""},
    /* A cut that falls between two characters keeps the first whole. */
    {"\xC3\xA9\xC3\xA9", 3, "\xC3\xA9"},
    {"\xC3\xA9\xC3\xA9", 4, "\xC3\xA9"},
    {"\xC3\xA9\xC3\xA9", 5, "\xC3\xA9\xC3\xA9"},
};

static size_t length_of(const char *text)
{
    size_t length = 0;

    while (text[length] != '\0')
    {
        length++;
    }
    return length;
}

TB_TEST(platform_name, a_name_is_cut_between_characters)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(name_cases); index++)
    {
        const name_case *const row = &name_cases[index];
        char buffer[BUFFER_SIZE] = {0};
        size_t length = 0;

        TB_TEST_CONTEXT("row %zu: capacity %zu", index, row->capacity);
        TB_TEST_REQUIRE(row->capacity <= BUFFER_SIZE);
        for (size_t at = 0; at < BUFFER_SIZE; at++)
        {
            buffer[at] = UNTOUCHED;
        }
        length = tb_platform_copy_name(buffer, row->capacity, row->name);
        TB_TEST_EXPECT_UINT(length, ==, length_of(row->kept));
        TB_TEST_EXPECT_BYTES(buffer, row->kept, length + 1);
        /* Nothing is written past the terminator. */
        for (size_t at = length + 1; at < BUFFER_SIZE; at++)
        {
            if (buffer[at] != UNTOUCHED)
            {
                TB_TEST_FAIL("byte %zu was written", at);
            }
        }
    }
}

TB_TEST(platform_name, no_name_is_the_empty_name)
{
    char buffer[BUFFER_SIZE] = {UNTOUCHED, UNTOUCHED};

    TB_TEST_EXPECT_UINT(tb_platform_copy_name(buffer, sizeof(buffer), NULL), ==, 0);
    TB_TEST_EXPECT_UINT((unsigned char)buffer[0], ==, 0);
    TB_TEST_EXPECT_UINT((unsigned char)buffer[1], ==, (unsigned char)UNTOUCHED);
}
