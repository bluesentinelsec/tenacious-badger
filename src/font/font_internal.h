/* What the parts of the TrueType reader share: the open font, bytes read
 * big-endian with their bounds checked, and the checks each table makes when
 * the font is opened. */
#ifndef TB_FONT_INTERNAL_H
#define TB_FONT_INTERNAL_H

#include "badger/allocator.h"
#include "badger/assert.h"
#include "badger/font.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A range of the font's bytes, such as one table. */
typedef struct tb_font_span
{
    const uint8_t *bytes;
    size_t length;
} tb_font_span;

/* The subtables of one GPOS lookup that kern pairs, as offsets in GPOS. */
typedef struct tb_font_pair_lookup
{
    size_t first; /* Index of its first subtable in tb_font.pair_subtables. */
    size_t count;
} tb_font_pair_lookup;

struct tb_font
{
    tb_allocator allocator;
    tb_font_metrics metrics;
    tb_font_style style;
    char *names; /* family, NUL, style, NUL; style.family and style.style point here. */
    size_t names_size;

    tb_font_span hmtx;
    uint16_t hmetric_count; /* Glyphs with an advance of their own in hmtx. */

    tb_font_span loca;
    tb_font_span glyf;
    bool long_offsets; /* loca holds 32-bit offsets, not 16-bit halves. */

    tb_font_span cmap;    /* From the chosen subtable to the end of the cmap table. */
    uint16_t cmap_format; /* 4 or 12. */
    uint32_t cmap_count;  /* Segments of format 4, groups of format 12. */

    tb_font_span kern_pairs; /* The pairs of the kern table's chosen subtable. */
    size_t kern_pair_count;

    tb_font_span gpos;
    tb_font_pair_lookup *pair_lookups; /* The pair lookups of the kern feature, in order. */
    size_t pair_lookup_count;
    size_t *pair_subtables; /* Offsets in gpos of PairPos subtables, format 1 or 2. */
    size_t pair_subtable_count;
};

/* -------------------------------------------------------------------------
 * Bytes.
 * ------------------------------------------------------------------------- */

static inline uint16_t tb_font_u16(const uint8_t *bytes)
{
    return (uint16_t)(((unsigned)bytes[0] << 8) | bytes[1]);
}

static inline int16_t tb_font_s16(const uint8_t *bytes)
{
    const int32_t value = tb_font_u16(bytes);
    return (int16_t)(value >= 0x8000 ? value - 0x10000 : value);
}

static inline uint32_t tb_font_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

/* Whether `size` bytes at `offset` lie within `length`. */
static inline bool tb_font_fits(size_t offset, size_t size, size_t length)
{
    return offset <= length && size <= length - offset;
}

/* Whether `count` records of `size` bytes at `offset` lie within `length`. */
static inline bool tb_font_array_fits(size_t offset, size_t count, size_t size, size_t length)
{
    return offset <= length && (size == 0 || count <= (length - offset) / size);
}

/* Reads of a span whose bounds an earlier check proved: a contract of the
 * reader's own, asserted. */
static inline uint16_t tb_font_span_u16(tb_font_span span, size_t offset)
{
    TB_ASSERT(tb_font_fits(offset, 2, span.length));
    return tb_font_u16(span.bytes + offset);
}

static inline int16_t tb_font_span_s16(tb_font_span span, size_t offset)
{
    TB_ASSERT(tb_font_fits(offset, 2, span.length));
    return tb_font_s16(span.bytes + offset);
}

static inline uint32_t tb_font_span_u32(tb_font_span span, size_t offset)
{
    TB_ASSERT(tb_font_fits(offset, 4, span.length));
    return tb_font_u32(span.bytes + offset);
}

/* The part of a span from `offset` on; `offset` must be within it. */
static inline tb_font_span tb_font_span_from(tb_font_span span, size_t offset)
{
    TB_ASSERT(offset <= span.length);
    const tb_font_span rest = {span.bytes + offset, span.length - offset};
    return rest;
}

/* Record a failure in `error`, and return its result. */
tb_font_result tb_font_fail(tb_font_error *error, tb_font_result result, const char *table,
                            const char *reason);

/* -------------------------------------------------------------------------
 * The tables, checked as the font opens. Each returns OK or a failure that
 * it has recorded in `error`. A missing optional table is an empty span.
 * ------------------------------------------------------------------------- */

tb_font_result tb_font_open_cmap(tb_font *font, tb_font_span cmap, tb_font_error *error);

/* The family and style, into memory from the font's allocator. */
tb_font_result tb_font_open_names(tb_font *font, tb_font_span name, tb_font_error *error);

tb_font_result tb_font_open_kern(tb_font *font, tb_font_span kern, tb_font_error *error);

tb_font_result tb_font_open_gpos(tb_font *font, tb_font_span gpos, tb_font_error *error);

/* Release what tb_font_open_gpos() allocated. */
void tb_font_close_gpos(tb_font *font);

/* The byte range of a glyph in glyf, which loca was checked to give. */
tb_font_span tb_font_glyph_data(const tb_font *font, uint16_t glyph);

#endif
