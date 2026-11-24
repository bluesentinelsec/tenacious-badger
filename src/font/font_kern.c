/* Kerning: the pair adjustments of GPOS's kern feature, or the older kern
 * table's pairs where GPOS has none. */
#include "badger/font.h"

#include "badger/allocator.h"
#include "badger/assert.h"
#include "font_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The kern table. */
#define TB_FONT_KERN_HEADER ((size_t)4)
#define TB_FONT_KERN_SUBTABLE_HEADER ((size_t)6)
#define TB_FONT_KERN_PAIRS_HEADER ((size_t)8)
#define TB_FONT_KERN_PAIR ((size_t)6)
#define TB_FONT_KERN_HORIZONTAL 0x0001u
#define TB_FONT_KERN_MINIMUM 0x0002u
#define TB_FONT_KERN_CROSS_STREAM 0x0004u

/* GPOS. */
#define TB_FONT_GPOS_HEADER ((size_t)10)
#define TB_FONT_FEATURE_RECORD ((size_t)6)
#define TB_FONT_FEATURE_KERN UINT32_C(0x6B65726E) /* 'kern' */
#define TB_FONT_LOOKUP_HEADER ((size_t)6)
#define TB_FONT_LOOKUP_PAIR 2u
#define TB_FONT_LOOKUP_EXTENSION 9u
#define TB_FONT_EXTENSION_LENGTH ((size_t)8)
#define TB_FONT_PAIR1_HEADER ((size_t)10)
#define TB_FONT_PAIR2_HEADER ((size_t)16)
#define TB_FONT_RANGE_RECORD ((size_t)6)
#define TB_FONT_VALUE_FORMAT_KNOWN 0x00FFu
#define TB_FONT_VALUE_X_ADVANCE 0x0004u
/* Any glyph's coverage index, for a coverage table whose indices are not used. */
#define TB_FONT_ANY_INDEX ((size_t)0x10000)

/* -------------------------------------------------------------------------
 * The kern table.
 * ------------------------------------------------------------------------- */

static tb_font_result tb_font_check_kern_pairs(tb_font *font, tb_font_span pairs, size_t count,
                                               tb_font_error *error)
{
    if (!tb_font_array_fits(0, count, TB_FONT_KERN_PAIR, pairs.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "kern", "the pairs are cut off");
    }
    for (size_t index = 1; index < count; index++)
    {
        const uint32_t previous = tb_font_u32(pairs.bytes + (index - 1) * TB_FONT_KERN_PAIR);
        if (tb_font_u32(pairs.bytes + index * TB_FONT_KERN_PAIR) <= previous)
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "kern", "the pairs are out of order");
        }
    }
    font->kern_pairs = pairs;
    font->kern_pair_count = count;
    return TB_FONT_OK;
}

tb_font_result tb_font_open_kern(tb_font *font, tb_font_span kern, tb_font_error *error)
{
    if (kern.bytes == NULL)
    {
        return TB_FONT_OK;
    }
    if (kern.length < TB_FONT_KERN_HEADER)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "kern", "the table is too short");
    }
    /* Apple's version 1 table, or a later one, holds no pairs this reader knows. */
    if (tb_font_u16(kern.bytes) != 0)
    {
        return TB_FONT_OK;
    }
    const size_t count = tb_font_u16(kern.bytes + 2);
    size_t offset = TB_FONT_KERN_HEADER;
    for (size_t index = 0; index < count; index++)
    {
        if (!tb_font_fits(offset, TB_FONT_KERN_SUBTABLE_HEADER, kern.length))
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "kern", "a subtable is cut off");
        }
        const size_t length = tb_font_u16(kern.bytes + offset + 2);
        const uint16_t coverage = tb_font_u16(kern.bytes + offset + 4);
        const unsigned format = coverage >> 8;
        const bool wanted = format == 0 && (coverage & TB_FONT_KERN_HORIZONTAL) != 0 &&
                            (coverage & (TB_FONT_KERN_MINIMUM | TB_FONT_KERN_CROSS_STREAM)) == 0;
        if (wanted)
        {
            const size_t header = offset + TB_FONT_KERN_SUBTABLE_HEADER;
            if (!tb_font_fits(header, TB_FONT_KERN_PAIRS_HEADER, kern.length))
            {
                return tb_font_fail(error, TB_FONT_MALFORMED, "kern", "a subtable is cut off");
            }
            /* The pairs are found by their count, not the subtable's 16-bit
             * length, which a large subtable overflows. */
            const size_t pairs = tb_font_u16(kern.bytes + header);
            return tb_font_check_kern_pairs(
                font, tb_font_span_from(kern, header + TB_FONT_KERN_PAIRS_HEADER), pairs, error);
        }
        if (length < TB_FONT_KERN_SUBTABLE_HEADER)
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "kern",
                                "a subtable's length is too short");
        }
        offset += length;
    }
    return TB_FONT_OK;
}

static int32_t tb_font_kern_table(const tb_font *font, uint16_t left, uint16_t right)
{
    const uint32_t key = ((uint32_t)left << 16) | right;
    size_t low = 0;
    size_t high = font->kern_pair_count;
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        const uint32_t found = tb_font_span_u32(font->kern_pairs, middle * TB_FONT_KERN_PAIR);
        if (found < key)
        {
            low = middle + 1;
        }
        else if (found > key)
        {
            high = middle;
        }
        else
        {
            return tb_font_span_s16(font->kern_pairs, middle * TB_FONT_KERN_PAIR + 4);
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Checking GPOS's coverage and class definition tables.
 * ------------------------------------------------------------------------- */

/* Check a coverage table whose every index must be less than `limit`. */
static tb_font_result tb_font_check_coverage(tb_font_span gpos, size_t offset, size_t limit,
                                             tb_font_error *error)
{
    if (!tb_font_fits(offset, 4, gpos.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a coverage table is cut off");
    }
    const uint16_t format = tb_font_u16(gpos.bytes + offset);
    const size_t count = tb_font_u16(gpos.bytes + offset + 2);
    const size_t records = offset + 4;
    if (format == 1)
    {
        if (!tb_font_array_fits(records, count, 2, gpos.length))
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a coverage table is cut off");
        }
        if (count > limit)
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS",
                                "a coverage index is past its subtable's records");
        }
        for (size_t index = 1; index < count; index++)
        {
            if (tb_font_u16(gpos.bytes + records + index * 2) <=
                tb_font_u16(gpos.bytes + records + (index - 1) * 2))
            {
                return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS",
                                    "a coverage table is out of order");
            }
        }
        return TB_FONT_OK;
    }
    if (format != 2)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a coverage format is unknown");
    }
    if (!tb_font_array_fits(records, count, TB_FONT_RANGE_RECORD, gpos.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a coverage table is cut off");
    }
    for (size_t index = 0; index < count; index++)
    {
        const uint8_t *range = gpos.bytes + records + index * TB_FONT_RANGE_RECORD;
        const uint16_t start = tb_font_u16(range);
        const uint16_t end = tb_font_u16(range + 2);
        const bool after_previous = index == 0 || start > tb_font_u16(range - 4);
        if (start > end || !after_previous)
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS",
                                "a coverage table is out of order");
        }
        if ((size_t)tb_font_u16(range + 4) + (end - start) >= limit)
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS",
                                "a coverage index is past its subtable's records");
        }
    }
    return TB_FONT_OK;
}

/* Check a class definition table whose every class must be less than `classes`. */
static tb_font_result tb_font_check_classes(tb_font_span gpos, size_t offset, size_t classes,
                                            tb_font_error *error)
{
    if (!tb_font_fits(offset, 6, gpos.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a class table is cut off");
    }
    const uint16_t format = tb_font_u16(gpos.bytes + offset);
    if (format == 1)
    {
        const size_t count = tb_font_u16(gpos.bytes + offset + 4);
        if (!tb_font_array_fits(offset + 6, count, 2, gpos.length))
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a class table is cut off");
        }
        for (size_t index = 0; index < count; index++)
        {
            if (tb_font_u16(gpos.bytes + offset + 6 + index * 2) >= classes)
            {
                return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a class is out of range");
            }
        }
        return TB_FONT_OK;
    }
    if (format != 2)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a class format is unknown");
    }
    const size_t count = tb_font_u16(gpos.bytes + offset + 2);
    const size_t records = offset + 4;
    if (!tb_font_array_fits(records, count, TB_FONT_RANGE_RECORD, gpos.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a class table is cut off");
    }
    for (size_t index = 0; index < count; index++)
    {
        const uint8_t *range = gpos.bytes + records + index * TB_FONT_RANGE_RECORD;
        const uint16_t start = tb_font_u16(range);
        const uint16_t end = tb_font_u16(range + 2);
        const bool after_previous = index == 0 || start > tb_font_u16(range - 4);
        if (start > end || !after_previous)
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a class table is out of order");
        }
        if (tb_font_u16(range + 4) >= classes)
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a class is out of range");
        }
    }
    return TB_FONT_OK;
}

/* Bytes of a value record of a format. */
static size_t tb_font_value_size(uint16_t format)
{
    size_t size = 0;
    for (uint16_t bits = format; bits != 0; bits = (uint16_t)(bits & (bits - 1u)))
    {
        size += 2;
    }
    return size;
}

/* -------------------------------------------------------------------------
 * Checking pair adjustment subtables.
 * ------------------------------------------------------------------------- */

static tb_font_result tb_font_check_pair_sets(tb_font_span gpos, size_t subtable,
                                              size_t record_size, tb_font_error *error)
{
    const size_t count = tb_font_u16(gpos.bytes + subtable + 8);
    if (!tb_font_array_fits(subtable + TB_FONT_PAIR1_HEADER, count, 2, gpos.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "the pair sets are cut off");
    }
    for (size_t index = 0; index < count; index++)
    {
        const size_t set =
            subtable + tb_font_u16(gpos.bytes + subtable + TB_FONT_PAIR1_HEADER + index * 2);
        if (!tb_font_fits(set, 2, gpos.length))
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a pair set is cut off");
        }
        const size_t pairs = tb_font_u16(gpos.bytes + set);
        if (!tb_font_array_fits(set + 2, pairs, record_size, gpos.length))
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a pair set is cut off");
        }
        for (size_t pair = 1; pair < pairs; pair++)
        {
            const uint8_t *record = gpos.bytes + set + 2 + pair * record_size;
            if (tb_font_u16(record) <= tb_font_u16(record - record_size))
            {
                return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a pair set is out of order");
            }
        }
    }
    return tb_font_check_coverage(gpos, subtable + tb_font_u16(gpos.bytes + subtable + 2), count,
                                  error);
}

static tb_font_result tb_font_check_class_pairs(tb_font_span gpos, size_t subtable,
                                                size_t record_size, tb_font_error *error)
{
    const size_t first_classes = tb_font_u16(gpos.bytes + subtable + 12);
    const size_t second_classes = tb_font_u16(gpos.bytes + subtable + 14);
    if (first_classes == 0 || second_classes == 0)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a pair subtable has no classes");
    }
    /* Both counts are 16-bit, so their product fits 32 bits. */
    if (!tb_font_array_fits(subtable + TB_FONT_PAIR2_HEADER, first_classes * second_classes,
                            record_size, gpos.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "the class pairs are cut off");
    }
    tb_font_result result = tb_font_check_classes(
        gpos, subtable + tb_font_u16(gpos.bytes + subtable + 8), first_classes, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    result = tb_font_check_classes(gpos, subtable + tb_font_u16(gpos.bytes + subtable + 10),
                                   second_classes, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    return tb_font_check_coverage(gpos, subtable + tb_font_u16(gpos.bytes + subtable + 2),
                                  TB_FONT_ANY_INDEX, error);
}

static tb_font_result tb_font_check_pair_subtable(tb_font_span gpos, size_t subtable,
                                                  tb_font_error *error)
{
    if (!tb_font_fits(subtable, TB_FONT_PAIR1_HEADER, gpos.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a pair subtable is cut off");
    }
    const uint16_t format = tb_font_u16(gpos.bytes + subtable);
    const uint16_t first = tb_font_u16(gpos.bytes + subtable + 4);
    const uint16_t second = tb_font_u16(gpos.bytes + subtable + 6);
    if ((first & ~TB_FONT_VALUE_FORMAT_KNOWN) != 0 || (second & ~TB_FONT_VALUE_FORMAT_KNOWN) != 0)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a value format is unknown");
    }
    const size_t values = tb_font_value_size(first) + tb_font_value_size(second);
    if (format == 1)
    {
        return tb_font_check_pair_sets(gpos, subtable, values + 2, error);
    }
    if (format != 2)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a pair format is unknown");
    }
    if (!tb_font_fits(subtable, TB_FONT_PAIR2_HEADER, gpos.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a pair subtable is cut off");
    }
    return tb_font_check_class_pairs(gpos, subtable, values, error);
}

/* -------------------------------------------------------------------------
 * The lookups of the kern feature.
 * ------------------------------------------------------------------------- */

/* Mark each lookup of a kern feature in `used`, of `lookup_count`. */
static tb_font_result tb_font_mark_kern_lookups(tb_font_span gpos, bool *used, size_t lookup_count,
                                                tb_font_error *error)
{
    const size_t list = tb_font_u16(gpos.bytes + 6);
    if (!tb_font_fits(list, 2, gpos.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "the feature list is cut off");
    }
    const size_t count = tb_font_u16(gpos.bytes + list);
    if (!tb_font_array_fits(list + 2, count, TB_FONT_FEATURE_RECORD, gpos.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "the feature list is cut off");
    }
    for (size_t index = 0; index < count; index++)
    {
        const uint8_t *record = gpos.bytes + list + 2 + index * TB_FONT_FEATURE_RECORD;
        if (tb_font_u32(record) != TB_FONT_FEATURE_KERN)
        {
            continue;
        }
        const size_t feature = list + tb_font_u16(record + 4);
        if (!tb_font_fits(feature, 4, gpos.length))
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a feature is cut off");
        }
        const size_t lookups = tb_font_u16(gpos.bytes + feature + 2);
        if (!tb_font_array_fits(feature + 4, lookups, 2, gpos.length))
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a feature is cut off");
        }
        for (size_t lookup = 0; lookup < lookups; lookup++)
        {
            const size_t which = tb_font_u16(gpos.bytes + feature + 4 + lookup * 2);
            if (which >= lookup_count)
            {
                return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS",
                                    "a feature names a lookup that does not exist");
            }
            used[which] = true;
        }
    }
    return TB_FONT_OK;
}

/* Where a lookup subtable's pair adjustments are, through an extension if
 * it is one; 0 when it is not a pair adjustment. */
static tb_font_result tb_font_pair_subtable(tb_font_span gpos, uint16_t type, size_t subtable,
                                            size_t *pairs, tb_font_error *error)
{
    *pairs = 0;
    if (type == TB_FONT_LOOKUP_PAIR)
    {
        *pairs = subtable;
        return TB_FONT_OK;
    }
    if (!tb_font_fits(subtable, TB_FONT_EXTENSION_LENGTH, gpos.length) ||
        tb_font_u16(gpos.bytes + subtable) != 1)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "an extension is malformed");
    }
    if (tb_font_u16(gpos.bytes + subtable + 2) != TB_FONT_LOOKUP_PAIR)
    {
        return TB_FONT_OK;
    }
    const size_t target = (size_t)tb_font_u32(gpos.bytes + subtable + 4);
    if (target > gpos.length - subtable)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "an extension is malformed");
    }
    *pairs = subtable + target;
    return TB_FONT_OK;
}

/* Walk the pair subtables of the used lookups, checking them. With `font`'s
 * arrays allocated, record them there; without, only count them. */
static tb_font_result tb_font_walk_lookups(tb_font *font, tb_font_span gpos, const bool *used,
                                           size_t lookup_count, tb_font_error *error)
{
    const size_t list = tb_font_u16(gpos.bytes + 8);
    const bool record = font->pair_subtables != NULL;
    font->pair_lookup_count = 0;
    font->pair_subtable_count = 0;
    for (size_t index = 0; index < lookup_count; index++)
    {
        if (!used[index])
        {
            continue;
        }
        const size_t lookup = list + tb_font_u16(gpos.bytes + list + 2 + index * 2);
        if (!tb_font_fits(lookup, TB_FONT_LOOKUP_HEADER, gpos.length))
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a lookup is cut off");
        }
        const uint16_t type = tb_font_u16(gpos.bytes + lookup);
        const size_t count = tb_font_u16(gpos.bytes + lookup + 4);
        if (type != TB_FONT_LOOKUP_PAIR && type != TB_FONT_LOOKUP_EXTENSION)
        {
            continue;
        }
        if (!tb_font_array_fits(lookup + TB_FONT_LOOKUP_HEADER, count, 2, gpos.length))
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "a lookup is cut off");
        }
        const size_t first = font->pair_subtable_count;
        for (size_t subtable = 0; subtable < count; subtable++)
        {
            size_t pairs = 0;
            const size_t at =
                lookup + tb_font_u16(gpos.bytes + lookup + TB_FONT_LOOKUP_HEADER + subtable * 2);
            tb_font_result result = tb_font_pair_subtable(gpos, type, at, &pairs, error);
            if (result == TB_FONT_OK && pairs != 0 && !record)
            {
                result = tb_font_check_pair_subtable(gpos, pairs, error);
            }
            if (result != TB_FONT_OK)
            {
                return result;
            }
            if (pairs == 0)
            {
                continue;
            }
            if (record)
            {
                font->pair_subtables[font->pair_subtable_count] = pairs;
            }
            font->pair_subtable_count++;
        }
        if (font->pair_subtable_count == first)
        {
            continue;
        }
        if (record)
        {
            font->pair_lookups[font->pair_lookup_count].first = first;
            font->pair_lookups[font->pair_lookup_count].count = font->pair_subtable_count - first;
        }
        font->pair_lookup_count++;
    }
    return TB_FONT_OK;
}

static tb_font_result tb_font_find_pair_lookups(tb_font *font, tb_font_span gpos, bool *used,
                                                size_t lookup_count, tb_font_error *error)
{
    tb_font_result result = tb_font_mark_kern_lookups(gpos, used, lookup_count, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    result = tb_font_walk_lookups(font, gpos, used, lookup_count, error);
    if (result != TB_FONT_OK || font->pair_subtable_count == 0)
    {
        return result;
    }
    const size_t lookups = font->pair_lookup_count;
    const size_t subtables = font->pair_subtable_count;
    font->pair_lookups = TB_ALLOCATOR_NEW_ARRAY(&font->allocator, tb_font_pair_lookup, lookups);
    font->pair_subtables = TB_ALLOCATOR_NEW_ARRAY(&font->allocator, size_t, subtables);
    result = tb_font_walk_lookups(font, gpos, used, lookup_count, error);
    TB_ASSERT(result == TB_FONT_OK);
    TB_ASSERT(font->pair_lookup_count == lookups && font->pair_subtable_count == subtables);
    font->gpos = gpos;
    return result;
}

tb_font_result tb_font_open_gpos(tb_font *font, tb_font_span gpos, tb_font_error *error)
{
    if (gpos.bytes == NULL)
    {
        return TB_FONT_OK;
    }
    if (gpos.length < TB_FONT_GPOS_HEADER)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "the table is too short");
    }
    /* A later major version may lay the table out otherwise. */
    if (tb_font_u16(gpos.bytes) != 1)
    {
        return TB_FONT_OK;
    }
    const size_t list = tb_font_u16(gpos.bytes + 8);
    if (!tb_font_fits(list, 2, gpos.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "the lookup list is cut off");
    }
    const size_t lookup_count = tb_font_u16(gpos.bytes + list);
    if (!tb_font_array_fits(list + 2, lookup_count, 2, gpos.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "GPOS", "the lookup list is cut off");
    }
    if (lookup_count == 0)
    {
        return TB_FONT_OK;
    }
    bool *used = TB_ALLOCATOR_NEW_ARRAY(&font->allocator, bool, lookup_count);
    const tb_font_result result = tb_font_find_pair_lookups(font, gpos, used, lookup_count, error);
    TB_ALLOCATOR_DELETE_ARRAY(&font->allocator, bool, used, lookup_count);
    return result;
}

void tb_font_close_gpos(tb_font *font)
{
    if (font->pair_lookups != NULL)
    {
        TB_ALLOCATOR_DELETE_ARRAY(&font->allocator, tb_font_pair_lookup, font->pair_lookups,
                                  font->pair_lookup_count);
    }
    if (font->pair_subtables != NULL)
    {
        TB_ALLOCATOR_DELETE_ARRAY(&font->allocator, size_t, font->pair_subtables,
                                  font->pair_subtable_count);
    }
}

/* -------------------------------------------------------------------------
 * Finding a pair's kerning.
 * ------------------------------------------------------------------------- */

/* The glyph's coverage index, or false when the table does not cover it. */
static bool tb_font_covered(tb_font_span gpos, size_t coverage, uint16_t glyph, size_t *index)
{
    const uint16_t format = tb_font_span_u16(gpos, coverage);
    const size_t count = tb_font_span_u16(gpos, coverage + 2);
    const size_t records = coverage + 4;
    const size_t size = format == 1 ? 2 : TB_FONT_RANGE_RECORD;
    size_t low = 0;
    size_t high = count;
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        const size_t record = records + middle * size;
        const uint16_t start = tb_font_span_u16(gpos, record);
        const uint16_t end = format == 1 ? start : tb_font_span_u16(gpos, record + 2);
        if (glyph < start)
        {
            high = middle;
        }
        else if (glyph > end)
        {
            low = middle + 1;
        }
        else
        {
            *index =
                format == 1 ? middle : (size_t)tb_font_span_u16(gpos, record + 4) + (glyph - start);
            return true;
        }
    }
    return false;
}

static size_t tb_font_class_of(tb_font_span gpos, size_t table, uint16_t glyph)
{
    const uint16_t format = tb_font_span_u16(gpos, table);
    if (format == 1)
    {
        const uint16_t start = tb_font_span_u16(gpos, table + 2);
        const size_t count = tb_font_span_u16(gpos, table + 4);
        if (glyph < start || (size_t)(glyph - start) >= count)
        {
            return 0;
        }
        return tb_font_span_u16(gpos, table + 6 + ((size_t)glyph - start) * 2);
    }
    size_t low = 0;
    size_t high = tb_font_span_u16(gpos, table + 2);
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        const size_t record = table + 4 + middle * TB_FONT_RANGE_RECORD;
        if (glyph < tb_font_span_u16(gpos, record))
        {
            high = middle;
        }
        else if (glyph > tb_font_span_u16(gpos, record + 2))
        {
            low = middle + 1;
        }
        else
        {
            return tb_font_span_u16(gpos, record + 4);
        }
    }
    return 0;
}

/* The first glyph's advance in a value record at `record`. */
static int32_t tb_font_x_advance(tb_font_span gpos, uint16_t format, size_t record)
{
    if ((format & TB_FONT_VALUE_X_ADVANCE) == 0)
    {
        return 0;
    }
    return tb_font_span_s16(gpos, record + tb_font_value_size((uint16_t)(format & 0x0003u)));
}

/* A pair subtable's kerning of the pair; false when the subtable does not apply to it. */
static bool tb_font_pair_value(tb_font_span gpos, size_t subtable, uint16_t left, uint16_t right,
                               int32_t *value)
{
    size_t index = 0;
    if (!tb_font_covered(gpos, subtable + tb_font_span_u16(gpos, subtable + 2), left, &index))
    {
        return false;
    }
    const uint16_t first = tb_font_span_u16(gpos, subtable + 4);
    const size_t values =
        tb_font_value_size(first) + tb_font_value_size(tb_font_span_u16(gpos, subtable + 6));
    if (tb_font_span_u16(gpos, subtable) == 2)
    {
        const size_t first_class =
            tb_font_class_of(gpos, subtable + tb_font_span_u16(gpos, subtable + 8), left);
        const size_t second_class =
            tb_font_class_of(gpos, subtable + tb_font_span_u16(gpos, subtable + 10), right);
        const size_t second_classes = tb_font_span_u16(gpos, subtable + 14);
        const size_t record = subtable + TB_FONT_PAIR2_HEADER +
                              (first_class * second_classes + second_class) * values;
        *value = tb_font_x_advance(gpos, first, record);
        return true;
    }
    const size_t set =
        subtable + tb_font_span_u16(gpos, subtable + TB_FONT_PAIR1_HEADER + index * 2);
    const size_t size = values + 2;
    size_t low = 0;
    size_t high = tb_font_span_u16(gpos, set);
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        const size_t record = set + 2 + middle * size;
        const uint16_t second = tb_font_span_u16(gpos, record);
        if (second < right)
        {
            low = middle + 1;
        }
        else if (second > right)
        {
            high = middle;
        }
        else
        {
            *value = tb_font_x_advance(gpos, first, record + 2);
            return true;
        }
    }
    return false;
}

int32_t tb_font_get_kerning(const tb_font *font, uint16_t left, uint16_t right)
{
    TB_ASSERT(font != NULL);
    if (font->pair_lookup_count == 0)
    {
        return tb_font_kern_table(font, left, right);
    }
    /* At most 65535 lookups of a 16-bit value each: the sum fits. */
    int32_t total = 0;
    for (size_t lookup = 0; lookup < font->pair_lookup_count; lookup++)
    {
        const tb_font_pair_lookup *pairs = &font->pair_lookups[lookup];
        /* A lookup applies its first subtable that applies to the pair. */
        for (size_t index = 0; index < pairs->count; index++)
        {
            int32_t value = 0;
            if (tb_font_pair_value(font->gpos, font->pair_subtables[pairs->first + index], left,
                                   right, &value))
            {
                total += value;
                break;
            }
        }
    }
    return total;
}
