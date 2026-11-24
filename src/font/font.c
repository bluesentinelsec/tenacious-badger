/* Opening a TrueType font: the table directory, the tables of whole-font
 * measures, the horizontal metrics, and the glyph locations. */
#include "badger/font.h"

#include "badger/allocator.h"
#include "badger/assert.h"
#include "font_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* sfnt versions: TrueType outlines, Apple's tag for them, CFF, and a collection. */
#define TB_FONT_VERSION_TRUETYPE UINT32_C(0x00010000)
#define TB_FONT_VERSION_APPLE UINT32_C(0x74727565)      /* 'true' */
#define TB_FONT_VERSION_CFF UINT32_C(0x4F54544F)        /* 'OTTO' */
#define TB_FONT_VERSION_COLLECTION UINT32_C(0x74746366) /* 'ttcf' */

#define TB_FONT_DIRECTORY_LENGTH ((size_t)12)
#define TB_FONT_RECORD_LENGTH ((size_t)16)

#define TB_FONT_HEAD_LENGTH ((size_t)54)
#define TB_FONT_HEAD_MAGIC UINT32_C(0x5F0F3CF5)
#define TB_FONT_HHEA_LENGTH ((size_t)36)
#define TB_FONT_MAXP_LENGTH ((size_t)6)
#define TB_FONT_MAXP_VERSION_0_5 UINT32_C(0x00005000)
#define TB_FONT_MAXP_VERSION_1 UINT32_C(0x00010000)

/* OS/2: the shortest table, of version 0 as Apple wrote it; and the length
 * through sCapHeight, which version 2 added. */
#define TB_FONT_OS2_SHORTEST ((size_t)68)
#define TB_FONT_OS2_HEIGHTS_LENGTH ((size_t)90)

#define TB_FONT_MIN_UNITS_PER_EM 16u
#define TB_FONT_MAX_UNITS_PER_EM 16384u

#define TB_FONT_MAC_STYLE_BOLD 0x0001u
#define TB_FONT_MAC_STYLE_ITALIC 0x0002u
#define TB_FONT_SELECTION_ITALIC 0x0001u
#define TB_FONT_SELECTION_OBLIQUE 0x0200u

/* The tables the reader uses, as the directory gives them. */
typedef struct tb_font_tables
{
    tb_font_span head;
    tb_font_span hhea;
    tb_font_span maxp;
    tb_font_span hmtx;
    tb_font_span loca;
    tb_font_span glyf;
    tb_font_span cmap;
    tb_font_span kern;
    tb_font_span gpos;
    tb_font_span os2;
    tb_font_span name;
} tb_font_tables;

tb_font_result tb_font_fail(tb_font_error *error, tb_font_result result, const char *table,
                            const char *reason)
{
    error->result = result;
    error->table = table;
    error->reason = reason;
    return result;
}

/* -------------------------------------------------------------------------
 * The table directory.
 * ------------------------------------------------------------------------- */

static tb_font_result tb_font_check_version(uint32_t version, tb_font_error *error)
{
    if (version == TB_FONT_VERSION_TRUETYPE || version == TB_FONT_VERSION_APPLE)
    {
        return TB_FONT_OK;
    }
    if (version == TB_FONT_VERSION_CFF)
    {
        return tb_font_fail(error, TB_FONT_UNSUPPORTED, "",
                            "the font has PostScript (CFF) outlines, not TrueType");
    }
    if (version == TB_FONT_VERSION_COLLECTION)
    {
        return tb_font_fail(error, TB_FONT_UNSUPPORTED, "",
                            "the file is a font collection, not one font");
    }
    return tb_font_fail(error, TB_FONT_MALFORMED, "", "the bytes are not a TrueType font");
}

/* Where a tag's table goes in `tables`, or NULL for a table the reader does not use. */
static tb_font_span *tb_font_slot(tb_font_tables *tables, uint32_t tag)
{
    static const struct
    {
        uint32_t tag;
        size_t offset;
    } slots[] = {
        {UINT32_C(0x68656164), offsetof(tb_font_tables, head)}, /* head */
        {UINT32_C(0x68686561), offsetof(tb_font_tables, hhea)}, /* hhea */
        {UINT32_C(0x6D617870), offsetof(tb_font_tables, maxp)}, /* maxp */
        {UINT32_C(0x686D7478), offsetof(tb_font_tables, hmtx)}, /* hmtx */
        {UINT32_C(0x6C6F6361), offsetof(tb_font_tables, loca)}, /* loca */
        {UINT32_C(0x676C7966), offsetof(tb_font_tables, glyf)}, /* glyf */
        {UINT32_C(0x636D6170), offsetof(tb_font_tables, cmap)}, /* cmap */
        {UINT32_C(0x6B65726E), offsetof(tb_font_tables, kern)}, /* kern */
        {UINT32_C(0x47504F53), offsetof(tb_font_tables, gpos)}, /* GPOS */
        {UINT32_C(0x4F532F32), offsetof(tb_font_tables, os2)},  /* OS/2 */
        {UINT32_C(0x6E616D65), offsetof(tb_font_tables, name)}, /* name */
    };
    for (size_t index = 0; index < sizeof slots / sizeof slots[0]; index++)
    {
        if (slots[index].tag == tag)
        {
            return (tb_font_span *)(void *)((unsigned char *)tables + slots[index].offset);
        }
    }
    return NULL;
}

static tb_font_result tb_font_read_directory(tb_font_span file, tb_font_tables *tables,
                                             tb_font_error *error)
{
    if (file.length < TB_FONT_DIRECTORY_LENGTH)
    {
        if (file.length >= 4 && tb_font_check_version(tb_font_u32(file.bytes), error) != TB_FONT_OK)
        {
            return error->result;
        }
        return tb_font_fail(error, TB_FONT_TRUNCATED, "", "the table directory is cut off");
    }
    const tb_font_result version = tb_font_check_version(tb_font_u32(file.bytes), error);
    if (version != TB_FONT_OK)
    {
        return version;
    }
    const size_t count = tb_font_u16(file.bytes + 4);
    if (!tb_font_array_fits(TB_FONT_DIRECTORY_LENGTH, count, TB_FONT_RECORD_LENGTH, file.length))
    {
        return tb_font_fail(error, TB_FONT_TRUNCATED, "", "the table directory is cut off");
    }
    for (size_t index = 0; index < count; index++)
    {
        const uint8_t *record =
            file.bytes + TB_FONT_DIRECTORY_LENGTH + index * TB_FONT_RECORD_LENGTH;
        tb_font_span *slot = tb_font_slot(tables, tb_font_u32(record));
        /* The first of two tables with one tag wins. */
        if (slot == NULL || slot->bytes != NULL)
        {
            continue;
        }
        const size_t offset = tb_font_u32(record + 8);
        const size_t length = tb_font_u32(record + 12);
        if (!tb_font_fits(offset, length, file.length))
        {
            return tb_font_fail(error, TB_FONT_TRUNCATED, "",
                                "a table runs past the end of the bytes");
        }
        slot->bytes = file.bytes + offset;
        slot->length = length;
    }
    return TB_FONT_OK;
}

static tb_font_result tb_font_require(tb_font_span table, const char *tag, tb_font_error *error)
{
    if (table.bytes == NULL)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, tag, "a required table is missing");
    }
    return TB_FONT_OK;
}

static tb_font_result tb_font_require_all(const tb_font_tables *tables, tb_font_error *error)
{
    const struct
    {
        tb_font_span table;
        const char *tag;
    } required[] = {
        {tables->head, "head"}, {tables->hhea, "hhea"}, {tables->maxp, "maxp"},
        {tables->hmtx, "hmtx"}, {tables->loca, "loca"}, {tables->glyf, "glyf"},
        {tables->cmap, "cmap"},
    };
    for (size_t index = 0; index < sizeof required / sizeof required[0]; index++)
    {
        const tb_font_result result =
            tb_font_require(required[index].table, required[index].tag, error);
        if (result != TB_FONT_OK)
        {
            return result;
        }
    }
    return TB_FONT_OK;
}

/* -------------------------------------------------------------------------
 * The measures of the whole font.
 * ------------------------------------------------------------------------- */

static tb_font_result tb_font_open_head(tb_font *font, tb_font_span head, tb_font_error *error)
{
    if (head.length < TB_FONT_HEAD_LENGTH)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "head", "the table is too short");
    }
    if (tb_font_u32(head.bytes + 12) != TB_FONT_HEAD_MAGIC)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "head", "the magic number is wrong");
    }
    const uint16_t units_per_em = tb_font_u16(head.bytes + 18);
    if (units_per_em < TB_FONT_MIN_UNITS_PER_EM || units_per_em > TB_FONT_MAX_UNITS_PER_EM)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "head",
                            "units per em are outside 16 to 16384");
    }
    const int16_t location_format = tb_font_s16(head.bytes + 50);
    if (location_format != 0 && location_format != 1)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "head", "the loca format is unknown");
    }
    if (tb_font_s16(head.bytes + 52) != 0)
    {
        return tb_font_fail(error, TB_FONT_UNSUPPORTED, "head", "the glyph data format is unknown");
    }
    font->metrics.units_per_em = units_per_em;
    font->metrics.x_min = tb_font_s16(head.bytes + 36);
    font->metrics.y_min = tb_font_s16(head.bytes + 38);
    font->metrics.x_max = tb_font_s16(head.bytes + 40);
    font->metrics.y_max = tb_font_s16(head.bytes + 42);
    const uint16_t mac_style = tb_font_u16(head.bytes + 44);
    font->style.weight =
        (mac_style & TB_FONT_MAC_STYLE_BOLD) != 0 ? TB_FONT_WEIGHT_BOLD : TB_FONT_WEIGHT_REGULAR;
    font->style.italic = (mac_style & TB_FONT_MAC_STYLE_ITALIC) != 0;
    font->long_offsets = location_format == 1;
    return TB_FONT_OK;
}

static tb_font_result tb_font_open_maxp(tb_font *font, tb_font_span maxp, tb_font_error *error)
{
    if (maxp.length < TB_FONT_MAXP_LENGTH)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "maxp", "the table is too short");
    }
    const uint32_t version = tb_font_u32(maxp.bytes);
    if (version != TB_FONT_MAXP_VERSION_0_5 && version != TB_FONT_MAXP_VERSION_1)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "maxp", "the version is unknown");
    }
    const uint16_t glyph_count = tb_font_u16(maxp.bytes + 4);
    if (glyph_count == 0)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "maxp", "the font has no glyphs");
    }
    font->metrics.glyph_count = glyph_count;
    return TB_FONT_OK;
}

static tb_font_result tb_font_open_hhea(tb_font *font, tb_font_span hhea, tb_font_error *error)
{
    if (hhea.length < TB_FONT_HHEA_LENGTH)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "hhea", "the table is too short");
    }
    font->metrics.ascender = tb_font_s16(hhea.bytes + 4);
    font->metrics.descender = tb_font_s16(hhea.bytes + 6);
    font->metrics.line_gap = tb_font_s16(hhea.bytes + 8);
    const uint16_t count = tb_font_u16(hhea.bytes + 34);
    if (count == 0 || count > font->metrics.glyph_count)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "hhea",
                            "the count of horizontal metrics is outside 1 to the glyph count");
    }
    font->hmetric_count = count;
    return TB_FONT_OK;
}

static tb_font_result tb_font_open_hmtx(tb_font *font, tb_font_span hmtx, tb_font_error *error)
{
    /* A full record for each of the first glyphs, then a bearing for each of the rest. */
    const size_t records = (size_t)font->hmetric_count * 4;
    const size_t bearings = ((size_t)font->metrics.glyph_count - font->hmetric_count) * 2;
    if (hmtx.length < records + bearings)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "hmtx",
                            "the table is too short for every glyph");
    }
    font->hmtx = hmtx;
    return TB_FONT_OK;
}

/* Offset in glyf of where a glyph's data begins; `index` may be the glyph count. */
static size_t tb_font_location(const tb_font *font, size_t index)
{
    if (font->long_offsets)
    {
        return tb_font_span_u32(font->loca, index * 4);
    }
    return (size_t)tb_font_span_u16(font->loca, index * 2) * 2;
}

static tb_font_result tb_font_open_loca(tb_font *font, tb_font_span loca, tb_font_span glyf,
                                        tb_font_error *error)
{
    const size_t entry = font->long_offsets ? 4 : 2;
    if (!tb_font_array_fits(0, (size_t)font->metrics.glyph_count + 1, entry, loca.length))
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "loca",
                            "the table is too short for every glyph");
    }
    font->loca = loca;
    font->glyf = glyf;
    size_t previous = 0;
    for (size_t index = 0; index <= font->metrics.glyph_count; index++)
    {
        const size_t location = tb_font_location(font, index);
        if (location < previous)
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "loca", "glyph offsets go backward");
        }
        if (location > glyf.length)
        {
            return tb_font_fail(error, TB_FONT_MALFORMED, "loca",
                                "a glyph runs past the end of glyf");
        }
        previous = location;
    }
    return TB_FONT_OK;
}

tb_font_span tb_font_glyph_data(const tb_font *font, uint16_t glyph)
{
    TB_ASSERT(glyph < font->metrics.glyph_count);
    const size_t start = tb_font_location(font, glyph);
    const size_t end = tb_font_location(font, (size_t)glyph + 1);
    const tb_font_span data = {font->glyf.bytes + start, end - start};
    return data;
}

static tb_font_result tb_font_open_os2(tb_font *font, tb_font_span os2, tb_font_error *error)
{
    if (os2.bytes == NULL)
    {
        return TB_FONT_OK;
    }
    if (os2.length < TB_FONT_OS2_SHORTEST)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "OS/2", "the table is too short");
    }
    const uint16_t version = tb_font_u16(os2.bytes);
    const uint16_t weight = tb_font_u16(os2.bytes + 4);
    const uint16_t selection = tb_font_u16(os2.bytes + 62);
    /* Some fonts give a weight of 1 to 9, in hundreds. */
    if (weight >= 1 && weight <= 9)
    {
        font->style.weight = (uint16_t)(weight * 100);
    }
    else if (weight != 0)
    {
        font->style.weight = weight;
    }
    font->style.italic = font->style.italic ||
                         (selection & (TB_FONT_SELECTION_ITALIC | TB_FONT_SELECTION_OBLIQUE)) != 0;
    if (version < 2)
    {
        return TB_FONT_OK;
    }
    if (os2.length < TB_FONT_OS2_HEIGHTS_LENGTH)
    {
        return tb_font_fail(error, TB_FONT_MALFORMED, "OS/2",
                            "the table is too short for its version");
    }
    font->metrics.x_height = tb_font_s16(os2.bytes + 86);
    font->metrics.cap_height = tb_font_s16(os2.bytes + 88);
    return TB_FONT_OK;
}

/* -------------------------------------------------------------------------
 * Opening and closing.
 * ------------------------------------------------------------------------- */

static tb_font_result tb_font_open_tables(tb_font *font, tb_font_span file, tb_font_error *error)
{
    tb_font_tables tables = {0};
    tb_font_result result = tb_font_read_directory(file, &tables, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    result = tb_font_require_all(&tables, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    result = tb_font_open_head(font, tables.head, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    result = tb_font_open_maxp(font, tables.maxp, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    result = tb_font_open_hhea(font, tables.hhea, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    result = tb_font_open_hmtx(font, tables.hmtx, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    result = tb_font_open_loca(font, tables.loca, tables.glyf, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    result = tb_font_open_os2(font, tables.os2, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    result = tb_font_open_cmap(font, tables.cmap, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    result = tb_font_open_kern(font, tables.kern, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    result = tb_font_open_gpos(font, tables.gpos, error);
    if (result != TB_FONT_OK)
    {
        return result;
    }
    return tb_font_open_names(font, tables.name, error);
}

tb_font_result tb_font_open(const void *bytes, size_t length, const tb_allocator *allocator,
                            tb_font **font, tb_font_error *error)
{
    TB_ASSERT(bytes != NULL);
    TB_ASSERT(font != NULL);
    *font = NULL;
    tb_font_error local_error = {TB_FONT_OK, "", ""};
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    tb_font *opened = TB_ALLOCATOR_NEW(&resolved, tb_font);
    opened->allocator = resolved;
    const tb_font_span file = {(const uint8_t *)bytes, length};
    const tb_font_result result = tb_font_open_tables(opened, file, &local_error);
    if (result != TB_FONT_OK)
    {
        goto fail;
    }
    *font = opened;
    goto done;
fail:
    tb_font_close(opened);
done:
    if (error != NULL)
    {
        *error = local_error;
    }
    return result;
}

void tb_font_close(tb_font *font)
{
    if (font == NULL)
    {
        return;
    }
    const tb_allocator allocator = font->allocator;
    tb_font_close_gpos(font);
    if (font->names != NULL)
    {
        tb_allocator_free(&allocator, font->names, font->names_size, 1);
    }
    TB_ALLOCATOR_DELETE(&allocator, tb_font, font);
}

/* -------------------------------------------------------------------------
 * Queries.
 * ------------------------------------------------------------------------- */

void tb_font_get_metrics(const tb_font *font, tb_font_metrics *metrics)
{
    TB_ASSERT(font != NULL);
    TB_ASSERT(metrics != NULL);
    *metrics = font->metrics;
}

void tb_font_get_style(const tb_font *font, tb_font_style *style)
{
    TB_ASSERT(font != NULL);
    TB_ASSERT(style != NULL);
    *style = font->style;
}

void tb_font_get_glyph_metrics(const tb_font *font, uint16_t glyph, tb_font_glyph_metrics *metrics)
{
    TB_ASSERT(font != NULL);
    TB_ASSERT(metrics != NULL);
    TB_ASSERT(glyph < font->metrics.glyph_count);
    const size_t last = (size_t)font->hmetric_count - 1;
    if (glyph <= last)
    {
        metrics->advance_width = tb_font_span_u16(font->hmtx, (size_t)glyph * 4);
        metrics->left_side_bearing = tb_font_span_s16(font->hmtx, (size_t)glyph * 4 + 2);
        return;
    }
    /* Glyphs past the last full record share its advance. */
    metrics->advance_width = tb_font_span_u16(font->hmtx, last * 4);
    metrics->left_side_bearing =
        tb_font_span_s16(font->hmtx, (last + 1) * 4 + ((size_t)glyph - last - 1) * 2);
}

const char *tb_font_result_name(tb_font_result result)
{
    switch (result)
    {
    case TB_FONT_OK:
        return "ok";
    case TB_FONT_MALFORMED:
        return "malformed";
    case TB_FONT_TRUNCATED:
        return "truncated";
    case TB_FONT_UNSUPPORTED:
        return "unsupported";
    case TB_FONT_TOO_LARGE:
        return "too large";
    }
    return "unknown result";
}
