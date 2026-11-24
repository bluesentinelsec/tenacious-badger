/* The name table: the font's family and style, in UTF-16 or Mac Roman,
 * decoded to UTF-8. */
#include "badger/font.h"

#include "badger/allocator.h"
#include "badger/utf8.h"
#include "font_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TB_FONT_NAME_HEADER ((size_t)6)
#define TB_FONT_NAME_RECORD ((size_t)12)

#define TB_FONT_NAME_FAMILY 1u
#define TB_FONT_NAME_SUBFAMILY 2u
#define TB_FONT_NAME_TYPOGRAPHIC_FAMILY 16u
#define TB_FONT_NAME_TYPOGRAPHIC_SUBFAMILY 17u

#define TB_FONT_PLATFORM_UNICODE 0u
#define TB_FONT_PLATFORM_MACINTOSH 1u
#define TB_FONT_PLATFORM_WINDOWS 3u
#define TB_FONT_WINDOWS_BMP 1u
#define TB_FONT_WINDOWS_FULL 10u
#define TB_FONT_MACINTOSH_ROMAN 0u
#define TB_FONT_MACINTOSH_ENGLISH 0u
#define TB_FONT_WINDOWS_ENGLISH_US 0x0409u

#define TB_FONT_REPLACEMENT UINT32_C(0xFFFD)

/* What a Mac Roman byte from 0x80 up is in Unicode. */
static const uint16_t tb_font_mac_roman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3,
    0x00E5, 0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3,
    0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA,
    0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D,
    0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE,
    0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

/* One string of the table, chosen for a name. */
typedef struct tb_font_name_string
{
    tb_font_span bytes;
    bool utf16; /* UTF-16BE; otherwise Mac Roman. */
} tb_font_name_string;

/* How much a record's encoding is preferred; 0 for one the reader cannot decode. */
static int tb_font_name_preference(const uint8_t *record)
{
    const uint16_t platform = tb_font_u16(record);
    const uint16_t encoding = tb_font_u16(record + 2);
    const uint16_t language = tb_font_u16(record + 4);
    if (platform == TB_FONT_PLATFORM_WINDOWS &&
        (encoding == TB_FONT_WINDOWS_BMP || encoding == TB_FONT_WINDOWS_FULL))
    {
        return language == TB_FONT_WINDOWS_ENGLISH_US ? 4 : 3;
    }
    if (platform == TB_FONT_PLATFORM_UNICODE)
    {
        return 2;
    }
    if (platform == TB_FONT_PLATFORM_MACINTOSH && encoding == TB_FONT_MACINTOSH_ROMAN &&
        language == TB_FONT_MACINTOSH_ENGLISH)
    {
        return 1;
    }
    return 0;
}

/* Find the best string for a name ID; an empty one when the table has none. */
static tb_font_result tb_font_find_name(tb_font_span name, uint16_t id, tb_font_name_string *string,
                                        tb_font_error *error)
{
    const size_t count = tb_font_u16(name.bytes + 2);
    const size_t storage = tb_font_u16(name.bytes + 4);
    int best = 0;
    for (size_t index = 0; index < count; index++)
    {
        const uint8_t *record = name.bytes + TB_FONT_NAME_HEADER + index * TB_FONT_NAME_RECORD;
        const int preference = tb_font_name_preference(record);
        if (tb_font_u16(record + 6) != id || preference <= best)
        {
            continue;
        }
        const size_t length = tb_font_u16(record + 8);
        const size_t offset = storage + tb_font_u16(record + 10);
        if (!tb_font_fits(offset, length, name.length))
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "name",
                                "a string runs past the end of the table");
        }
        best = preference;
        string->bytes.bytes = name.bytes + offset;
        string->bytes.length = length;
        string->utf16 = tb_font_u16(record) != TB_FONT_PLATFORM_MACINTOSH;
    }
    return TB_FONT_OK;
}

/* The string's next character, from `*offset` on, which it advances. */
static uint32_t tb_font_next_character(const tb_font_name_string *string, size_t *offset)
{
    const uint8_t *bytes = string->bytes.bytes;
    if (!string->utf16)
    {
        const uint8_t byte = bytes[*offset];
        *offset += 1;
        return byte < 0x80 ? byte : tb_font_mac_roman[byte - 0x80];
    }
    const uint32_t unit = tb_font_u16(bytes + *offset);
    *offset += 2;
    if (unit < 0xD800 || unit > 0xDFFF)
    {
        return unit;
    }
    const bool paired = unit < 0xDC00 && *offset + 2 <= string->bytes.length;
    const uint32_t low = paired ? tb_font_u16(bytes + *offset) : 0;
    if (low < 0xDC00 || low > 0xDFFF)
    {
        return TB_FONT_REPLACEMENT;
    }
    *offset += 2;
    return 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
}

/* Decode a string to UTF-8 into `output`, or measure it when `output` is NULL.
 * Returns the bytes. A last odd byte of UTF-16 is dropped. */
static size_t tb_font_decode_name(const tb_font_name_string *string, char *output)
{
    const size_t unit = string->utf16 ? 2 : 1;
    const size_t end = string->bytes.length - string->bytes.length % unit;
    size_t written = 0;
    size_t offset = 0;
    while (offset < end)
    {
        const uint32_t character = tb_font_next_character(string, &offset);
        const size_t size = tb_utf8_encoded_size(character);
        if (output != NULL)
        {
            (void)tb_utf8_encode(character, output + written, size);
        }
        written += size;
    }
    return written;
}

static tb_font_result tb_font_find_names(tb_font_span name, tb_font_name_string *family,
                                         tb_font_name_string *style, tb_font_error *error)
{
    if (name.length < TB_FONT_NAME_HEADER)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "name", "the table is too short");
    }
    const size_t count = tb_font_u16(name.bytes + 2);
    if (!tb_font_array_fits(TB_FONT_NAME_HEADER, count, TB_FONT_NAME_RECORD, name.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "name", "the name records are cut off");
    }
    const uint16_t ids[4] = {TB_FONT_NAME_TYPOGRAPHIC_FAMILY, TB_FONT_NAME_FAMILY,
                             TB_FONT_NAME_TYPOGRAPHIC_SUBFAMILY, TB_FONT_NAME_SUBFAMILY};
    tb_font_name_string *strings[4] = {family, family, style, style};
    for (size_t index = 0; index < 4; index++)
    {
        /* The typographic name, where there is one, is kept. */
        if (strings[index]->bytes.bytes != NULL)
        {
            continue;
        }
        const tb_font_result result = tb_font_find_name(name, ids[index], strings[index], error);
        if (result != TB_FONT_OK)
        {
            return result;
        }
    }
    return TB_FONT_OK;
}

tb_font_result tb_font_open_names(tb_font *font, tb_font_span name, tb_font_error *error)
{
    tb_font_name_string family = {{NULL, 0}, false};
    tb_font_name_string style = {{NULL, 0}, false};
    if (name.bytes != NULL)
    {
        const tb_font_result result = tb_font_find_names(name, &family, &style, error);
        if (result != TB_FONT_OK)
        {
            return result;
        }
    }
    const size_t family_length = tb_font_decode_name(&family, NULL);
    const size_t style_length = tb_font_decode_name(&style, NULL);
    /* A string of at most 65535 bytes decodes to at most three times as
     * many: no sum can overflow. */
    font->names_size = family_length + style_length + 2;
    font->names = tb_allocator_alloc(&font->allocator, font->names_size, 1);
    (void)tb_font_decode_name(&family, font->names);
    font->names[family_length] = '\0';
    (void)tb_font_decode_name(&style, font->names + family_length + 1);
    font->names[family_length + 1 + style_length] = '\0';
    font->style.family = font->names;
    font->style.family_length = family_length;
    font->style.style = font->names + family_length + 1;
    font->style.style_length = style_length;
    return TB_FONT_OK;
}
