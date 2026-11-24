/* The character map: from a Unicode code point to a glyph, through a cmap
 * subtable of format 4 (segments of the Basic Multilingual Plane) or format
 * 12 (groups of any code points). */
#include "badger/font.h"

#include "badger/assert.h"
#include "font_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TB_FONT_CMAP_HEADER ((size_t)4)
#define TB_FONT_CMAP_RECORD ((size_t)8)

#define TB_FONT_PLATFORM_UNICODE 0u
#define TB_FONT_PLATFORM_WINDOWS 3u
#define TB_FONT_WINDOWS_BMP 1u
#define TB_FONT_WINDOWS_FULL 10u

/* Format 4: the header, then four arrays of one entry for each segment
 * (with two bytes of padding after the first). */
#define TB_FONT_FORMAT4_HEADER ((size_t)14)
#define TB_FONT_FORMAT4_ENDS ((size_t)14)

/* Format 12: the header, then a group of three 32-bit numbers each. */
#define TB_FONT_FORMAT12_HEADER ((size_t)16)
#define TB_FONT_FORMAT12_GROUP ((size_t)12)

#define TB_FONT_BMP_LAST UINT32_C(0xFFFF)

/* Where the arrays of a format 4 subtable of `count` segments begin. */
static size_t tb_font_format4_starts(size_t count)
{
    return TB_FONT_FORMAT4_ENDS + count * 2 + 2;
}

static size_t tb_font_format4_deltas(size_t count)
{
    return tb_font_format4_starts(count) + count * 2;
}

static size_t tb_font_format4_ranges(size_t count)
{
    return tb_font_format4_deltas(count) + count * 2;
}

static tb_font_result tb_font_check_format4(tb_font *font, tb_font_span subtable,
                                            tb_font_error *error)
{
    if (subtable.length < TB_FONT_FORMAT4_HEADER)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "cmap", "the format 4 header is cut off");
    }
    const uint16_t doubled = tb_font_u16(subtable.bytes + 6);
    if (doubled == 0 || doubled % 2 != 0)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "cmap", "the segment count is not valid");
    }
    const size_t count = doubled / 2u;
    const size_t ranges = tb_font_format4_ranges(count);
    if (!tb_font_fits(ranges, count * 2, subtable.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "cmap", "the segments are cut off");
    }
    uint32_t previous_end = 0;
    for (size_t index = 0; index < count; index++)
    {
        const uint16_t end = tb_font_u16(subtable.bytes + TB_FONT_FORMAT4_ENDS + index * 2);
        const uint16_t start =
            tb_font_u16(subtable.bytes + tb_font_format4_starts(count) + index * 2);
        const uint16_t range = tb_font_u16(subtable.bytes + ranges + index * 2);
        if (start > end || (index > 0 && end <= previous_end))
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "cmap", "the segments are out of order");
        }
        previous_end = end;
        /* A segment that maps through the glyph array must not leave the table. */
        const size_t last = ranges + index * 2 + range + ((size_t)end - start) * 2;
        if (range != 0 && !tb_font_fits(last, 2, subtable.length))
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "cmap",
                                "a segment maps past the end of the table");
        }
    }
    font->cmap_count = (uint32_t)count;
    return TB_FONT_OK;
}

static tb_font_result tb_font_check_format12(tb_font *font, tb_font_span subtable,
                                             tb_font_error *error)
{
    if (subtable.length < TB_FONT_FORMAT12_HEADER)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "cmap", "the format 12 header is cut off");
    }
    const uint32_t count = tb_font_u32(subtable.bytes + 12);
    if (!tb_font_array_fits(TB_FONT_FORMAT12_HEADER, count, TB_FONT_FORMAT12_GROUP,
                            subtable.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "cmap", "the groups are cut off");
    }
    for (size_t index = 0; index < count; index++)
    {
        const uint8_t *group =
            subtable.bytes + TB_FONT_FORMAT12_HEADER + index * TB_FONT_FORMAT12_GROUP;
        const uint32_t start = tb_font_u32(group);
        const uint32_t end = tb_font_u32(group + 4);
        const bool after_previous = index == 0 || start > tb_font_u32(group - 8);
        if (start > end || !after_previous)
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "cmap", "the groups are out of order");
        }
    }
    font->cmap_count = count;
    return TB_FONT_OK;
}

/* How much a subtable is preferred: 2 for Unicode in format 12, 1 for
 * format 4, 0 for one the reader cannot use. */
static int tb_font_cmap_preference(uint16_t platform, uint16_t encoding, uint16_t format)
{
    const bool unicode = platform == TB_FONT_PLATFORM_UNICODE ||
                         (platform == TB_FONT_PLATFORM_WINDOWS &&
                          (encoding == TB_FONT_WINDOWS_BMP || encoding == TB_FONT_WINDOWS_FULL));
    if (!unicode)
    {
        return 0;
    }
    if (format == 12)
    {
        return 2;
    }
    return format == 4 ? 1 : 0;
}

tb_font_result tb_font_open_cmap(tb_font *font, tb_font_span cmap, tb_font_error *error)
{
    if (cmap.length < TB_FONT_CMAP_HEADER)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "cmap", "the table is too short");
    }
    const size_t count = tb_font_u16(cmap.bytes + 2);
    if (!tb_font_array_fits(TB_FONT_CMAP_HEADER, count, TB_FONT_CMAP_RECORD, cmap.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "cmap", "the encoding records are cut off");
    }
    int best = 0;
    size_t best_offset = 0;
    for (size_t index = 0; index < count; index++)
    {
        const uint8_t *record = cmap.bytes + TB_FONT_CMAP_HEADER + index * TB_FONT_CMAP_RECORD;
        const size_t offset = tb_font_u32(record + 4);
        if (!tb_font_fits(offset, 2, cmap.length))
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "cmap",
                                "a subtable is past the end of the table");
        }
        const int preference = tb_font_cmap_preference(tb_font_u16(record), tb_font_u16(record + 2),
                                                       tb_font_u16(cmap.bytes + offset));
        if (preference > best)
        {
            best = preference;
            best_offset = offset;
        }
    }
    if (best == 0)
    {
        return tb_font_fail(error, TB_FONT_UNSUPPORTED, "cmap",
                            "no Unicode subtable of format 4 or 12");
    }
    font->cmap = tb_font_span_from(cmap, best_offset);
    font->cmap_format = best == 2 ? 12 : 4;
    if (font->cmap_format == 12)
    {
        return tb_font_check_format12(font, font->cmap, error);
    }
    return tb_font_check_format4(font, font->cmap, error);
}

static uint64_t tb_font_find_format4(const tb_font *font, uint32_t code_point)
{
    if (code_point > TB_FONT_BMP_LAST)
    {
        return 0;
    }
    const size_t count = font->cmap_count;
    /* The first segment that ends at or after the code point. */
    size_t low = 0;
    size_t high = count;
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        if (tb_font_span_u16(font->cmap, TB_FONT_FORMAT4_ENDS + middle * 2) < code_point)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    if (low == count)
    {
        return 0;
    }
    const uint16_t start = tb_font_span_u16(font->cmap, tb_font_format4_starts(count) + low * 2);
    if (start > code_point)
    {
        return 0;
    }
    const uint16_t delta = tb_font_span_u16(font->cmap, tb_font_format4_deltas(count) + low * 2);
    const size_t range_at = tb_font_format4_ranges(count) + low * 2;
    const uint16_t range = tb_font_span_u16(font->cmap, range_at);
    if (range == 0)
    {
        return (code_point + delta) & TB_FONT_BMP_LAST;
    }
    const uint16_t glyph =
        tb_font_span_u16(font->cmap, range_at + range + ((size_t)code_point - start) * 2);
    if (glyph == 0)
    {
        return 0;
    }
    return ((uint32_t)glyph + delta) & TB_FONT_BMP_LAST;
}

static uint64_t tb_font_find_format12(const tb_font *font, uint32_t code_point)
{
    size_t low = 0;
    size_t high = font->cmap_count;
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        const size_t group = TB_FONT_FORMAT12_HEADER + middle * TB_FONT_FORMAT12_GROUP;
        const uint32_t start = tb_font_span_u32(font->cmap, group);
        const uint32_t end = tb_font_span_u32(font->cmap, group + 4);
        if (code_point < start)
        {
            high = middle;
        }
        else if (code_point > end)
        {
            low = middle + 1;
        }
        else
        {
            return (uint64_t)tb_font_span_u32(font->cmap, group + 8) + (code_point - start);
        }
    }
    return 0;
}

uint16_t tb_font_find_glyph(const tb_font *font, uint32_t code_point)
{
    TB_ASSERT(font != NULL);
    const uint64_t glyph = font->cmap_format == 12 ? tb_font_find_format12(font, code_point)
                                                   : tb_font_find_format4(font, code_point);
    /* A map to a glyph the font does not have maps to nothing. */
    if (glyph >= font->metrics.glyph_count)
    {
        return 0;
    }
    return (uint16_t)glyph;
}
