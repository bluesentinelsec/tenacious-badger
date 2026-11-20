#include "crash_text.h"

/* Digits of a decimal value of 64 bits, its sign included. */
#define TB_CRASH_TEXT_DECIMAL_DIGITS 21

static void tb_crash_text_add_character(tb_crash_text *line, char character)
{
    if (line->length + 1 >= line->capacity)
    {
        return;
    }
    line->text[line->length] = character;
    line->length++;
    line->text[line->length] = '\0';
}

void tb_crash_text_add(tb_crash_text *line, const char *text)
{
    while (*text != '\0')
    {
        tb_crash_text_add_character(line, *text);
        text++;
    }
}

void tb_crash_text_add_address(tb_crash_text *line, uintptr_t value)
{
    static const char digits[] = "0123456789abcdef";
    const unsigned width = 2u * (unsigned)sizeof(uint64_t);

    tb_crash_text_add(line, "0x");
    for (unsigned place = width; place > 0; place--)
    {
        const unsigned shift = 4u * (place - 1u);
        tb_crash_text_add_character(line, digits[((uint64_t)value >> shift) & 0xFu]);
    }
}

void tb_crash_text_add_integer(tb_crash_text *line, long long value)
{
    char digits[TB_CRASH_TEXT_DECIMAL_DIGITS] = {0};
    size_t count = 0;
    /* The magnitude, taken without negating the most negative value. */
    unsigned long long magnitude =
        value < 0 ? 0ull - (unsigned long long)value : (unsigned long long)value;

    do
    {
        digits[count] = (char)('0' + (int)(magnitude % 10u));
        count++;
        magnitude /= 10u;
    } while (magnitude > 0 && count < sizeof(digits));
    if (value < 0)
    {
        tb_crash_text_add_character(line, '-');
    }
    while (count > 0)
    {
        count--;
        tb_crash_text_add_character(line, digits[count]);
    }
}
