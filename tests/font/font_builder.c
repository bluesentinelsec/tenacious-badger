/* Small TrueType fonts made in memory; see font_builder.h. */
#include "font_builder.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <string.h>

/* A cmap of a format 4 subtable for (3, 1), whose one real segment maps
 * 'a' to 'c' through the glyph array, and a format 12 subtable for (3, 10). */
void test_cmap_two_subtables(test_table *cmap)
{
    cmap->length = 0;
    test_table_u16(cmap, 0);
    test_table_u16(cmap, 3);
    test_table_u16(cmap, 3); /* An encoding this reader does not use. */
    test_table_u16(cmap, 0);
    test_table_u32(cmap, 28);
    test_table_u16(cmap, 3);
    test_table_u16(cmap, 1);
    test_table_u32(cmap, 28);
    test_table_u16(cmap, 3);
    test_table_u16(cmap, 10);
    test_table_u32(cmap, 28 + 40);
    /* Format 4 at 28: 2 segments, 40 bytes with its glyph array. */
    test_table_u16(cmap, 4);
    test_table_u16(cmap, 40);
    test_table_u16(cmap, 0);
    test_table_u16(cmap, 4);
    test_table_u16(cmap, 0);
    test_table_u16(cmap, 0);
    test_table_u16(cmap, 0);
    test_table_u16(cmap, 'c'); /* ends */
    test_table_u16(cmap, 0xFFFF);
    test_table_u16(cmap, 0);
    test_table_u16(cmap, 'a'); /* starts */
    test_table_u16(cmap, 0xFFFF);
    test_table_u16(cmap, 0); /* deltas */
    test_table_u16(cmap, 1);
    test_table_u16(cmap, 4); /* range offsets: from here to the glyph array */
    test_table_u16(cmap, 0);
    test_table_u16(cmap, 1); /* 'a' */
    test_table_u16(cmap, 0); /* 'b': none */
    test_table_u16(cmap, 2); /* 'c' */
    test_table_u16(cmap, 0);
    /* Format 12 at 68: U+1F600 to glyph 2, and U+20000 to U+20001 to 1 and 2;
     * U+30000 to glyph 50, which the font does not have. */
    test_table_u16(cmap, 12);
    test_table_u16(cmap, 0);
    test_table_u32(cmap, 16 + 3 * 12);
    test_table_u32(cmap, 0);
    test_table_u32(cmap, 3);
    test_table_u32(cmap, 0x1F600);
    test_table_u32(cmap, 0x1F600);
    test_table_u32(cmap, 2);
    test_table_u32(cmap, 0x20000);
    test_table_u32(cmap, 0x20001);
    test_table_u32(cmap, 1);
    test_table_u32(cmap, 0x30000);
    test_table_u32(cmap, 0x30000);
    test_table_u32(cmap, 50);
}

/* A kern table of `subtables` subtables before the one of pairs: each a
 * vertical format 0 subtable that is skipped. */
void test_kern_table(test_table *kern, size_t skipped)
{
    kern->length = 0;
    test_table_u16(kern, 0);
    test_table_u16(kern, (uint16_t)(skipped + 1));
    for (size_t index = 0; index < skipped; index++)
    {
        test_table_u16(kern, 0);
        test_table_u16(kern, 14);
        test_table_u16(kern, 0x0000); /* format 0, not horizontal */
        test_table_u16(kern, 0);
        test_table_u16(kern, 0);
        test_table_u16(kern, 0);
        test_table_u16(kern, 0);
    }
    test_table_u16(kern, 0);
    test_table_u16(kern, 14 + 2 * 6);
    test_table_u16(kern, 0x0001);
    test_table_u16(kern, 2);
    test_table_u16(kern, 0);
    test_table_u16(kern, 0);
    test_table_u16(kern, 0);
    test_table_u16(kern, 1);
    test_table_u16(kern, 2);
    test_table_u16(kern, (uint16_t)-40);
    test_table_u16(kern, 2);
    test_table_u16(kern, 1);
    test_table_u16(kern, 25);
}

/* GPOS with a 'mark' feature of lookup 4 and a 'kern' feature of lookups
 * 1, 0, 2, 3, and 1 again:
 * 0. an extension to a format 1 PairPos: 1 then 0 by +77, 1 then 2 by -30;
 * 1. a format 2 PairPos: glyph 1 then glyph 2 by -5; and a second subtable,
 *    reached only for glyph 0, which kerns it by +500 with anything;
 * 2. a single adjustment (type 1), which is not kerning;
 * 3. an extension to a single adjustment;
 * 4. a PairPos that only 'mark' uses: 2 then 1 by +1000. */
void test_gpos_table(test_table *g, test_gpos_layout *layout)
{
    g->length = 0;
    test_table_u32(g, 0x00010000);
    test_table_u16(g, 0);  /* script list: not read */
    test_table_u16(g, 10); /* feature list */
    test_table_u16(g, 0);  /* lookup list, at 8 */
    test_table_u16(g, 2);
    test_table_u32(g, 0x6D61726B); /* 'mark', its offset at 16 */
    test_table_u16(g, 0);
    test_table_u32(g, 0x6B65726E); /* 'kern', its tag at 18 and offset at 22 */
    test_table_u16(g, 0);
    test_table_set16(g, 16, (uint16_t)(g->length - 10));
    test_table_u16(g, 0);
    test_table_u16(g, 1);
    test_table_u16(g, 4);
    test_table_set16(g, 22, (uint16_t)(g->length - 10));
    test_table_u16(g, 0);
    test_table_u16(g, 5);
    test_table_u16(g, 1);
    test_table_u16(g, 0);
    test_table_u16(g, 2);
    test_table_u16(g, 3);
    test_table_u16(g, 1);

    const size_t list = g->length;
    test_table_set16(g, 8, (uint16_t)list);
    test_table_u16(g, 5);
    for (size_t index = 0; index < 5; index++)
    {
        test_table_u16(g, 0);
    }
    size_t *lookups = layout->lookups;
    layout->list = list;
    const uint16_t types[5] = {9, 2, 1, 9, 2};
    const uint16_t subtables[5] = {1, 2, 1, 1, 1};
    for (size_t index = 0; index < 5; index++)
    {
        lookups[index] = g->length;
        test_table_set16(g, list + 2 + index * 2, (uint16_t)(g->length - list));
        test_table_u16(g, types[index]);
        test_table_u16(g, 0);
        test_table_u16(g, subtables[index]);
        for (size_t subtable = 0; subtable < subtables[index]; subtable++)
        {
            test_table_u16(g, 0); /* Lookup 2's stays 0: it is never read. */
        }
    }

    /* Lookup 0: an extension to a format 1 PairPos. */
    const size_t extension = g->length;
    layout->extension = extension;
    test_table_set16(g, lookups[0] + 6, (uint16_t)(extension - lookups[0]));
    test_table_u16(g, 1);
    test_table_u16(g, 2);
    test_table_u32(g, 8);
    const size_t pairs = g->length;
    layout->pairs = pairs;
    test_table_u16(g, 1);
    test_table_u16(g, 12);     /* coverage */
    test_table_u16(g, 0x0005); /* XPlacement, then XAdvance */
    test_table_u16(g, 0);
    test_table_u16(g, 1);
    test_table_u16(g, 18); /* the pair set */
    test_table_u16(g, 1);  /* coverage: glyph 1 */
    test_table_u16(g, 1);
    test_table_u16(g, 1);
    TB_ASSERT(g->length == pairs + 18);
    test_table_u16(g, 2);
    test_table_u16(g, 0);
    test_table_u16(g, 99);
    test_table_u16(g, 77);
    test_table_u16(g, 2);
    test_table_u16(g, 99);
    test_table_u16(g, (uint16_t)-30);

    /* Lookup 1, subtable 0: classes. */
    const size_t classes = g->length;
    layout->classes = classes;
    test_table_set16(g, lookups[1] + 6, (uint16_t)(classes - lookups[1]));
    test_table_u16(g, 2);
    test_table_u16(g, 24);     /* coverage */
    test_table_u16(g, 0x0004); /* XAdvance */
    test_table_u16(g, 0);
    test_table_u16(g, 34); /* first classes */
    test_table_u16(g, 44); /* second classes */
    test_table_u16(g, 2);
    test_table_u16(g, 2);
    test_table_u16(g, 0); /* class 0 then 0, 1 */
    test_table_u16(g, 0);
    test_table_u16(g, 0); /* class 1 then 0, 1 */
    test_table_u16(g, (uint16_t)-5);
    TB_ASSERT(g->length == classes + 24);
    test_table_u16(g, 2); /* coverage: glyphs 1 and 2 */
    test_table_u16(g, 1);
    test_table_u16(g, 1);
    test_table_u16(g, 2);
    test_table_u16(g, 0);
    TB_ASSERT(g->length == classes + 34);
    test_table_u16(g, 1); /* first classes: glyph 1 is 1, glyph 2 is 0 */
    test_table_u16(g, 1);
    test_table_u16(g, 2);
    test_table_u16(g, 1);
    test_table_u16(g, 0);
    TB_ASSERT(g->length == classes + 44);
    test_table_u16(g, 2); /* second classes: glyph 2 is 1 */
    test_table_u16(g, 1);
    test_table_u16(g, 2);
    test_table_u16(g, 2);
    test_table_u16(g, 1);

    /* Lookup 1, subtable 1: covers glyphs 0 and 1, but glyph 1 never gets here. */
    const size_t second = g->length;
    layout->second = second;
    test_table_set16(g, lookups[1] + 8, (uint16_t)(second - lookups[1]));
    test_table_u16(g, 2);
    test_table_u16(g, 18);
    test_table_u16(g, 0x0004);
    test_table_u16(g, 0);
    test_table_u16(g, 26);
    test_table_u16(g, 30);
    test_table_u16(g, 1);
    test_table_u16(g, 1);
    test_table_u16(g, 500);
    TB_ASSERT(g->length == second + 18);
    test_table_u16(g, 1); /* coverage: glyphs 0 and 1 */
    test_table_u16(g, 2);
    test_table_u16(g, 0);
    test_table_u16(g, 1);
    test_table_u16(g, 2); /* first classes: no ranges */
    test_table_u16(g, 0);
    test_table_u16(g, 1); /* second classes: no glyphs */
    test_table_u16(g, 0);
    test_table_u16(g, 0);

    /* Lookup 3: an extension to a single adjustment, not followed. */
    test_table_set16(g, lookups[3] + 6, (uint16_t)(g->length - lookups[3]));
    test_table_u16(g, 1);
    test_table_u16(g, 1);
    test_table_u32(g, 0xFFFF);

    /* Lookup 4. */
    const size_t marks = g->length;
    layout->marks = marks;
    test_table_set16(g, lookups[4] + 6, (uint16_t)(marks - lookups[4]));
    test_table_u16(g, 1);
    test_table_u16(g, 12);
    test_table_u16(g, 0x0004);
    test_table_u16(g, 0);
    test_table_u16(g, 1);
    test_table_u16(g, 18);
    test_table_u16(g, 1);
    test_table_u16(g, 1);
    test_table_u16(g, 2);
    TB_ASSERT(g->length == marks + 18);
    test_table_u16(g, 1);
    test_table_u16(g, 1);
    test_table_u16(g, 1000);
}

void test_table_u8(test_table *table, uint8_t value)
{
    TB_ASSERT(table->length < TEST_TABLE_CAPACITY);
    table->bytes[table->length] = value;
    table->length++;
}

void test_table_u16(test_table *table, uint16_t value)
{
    test_table_u8(table, (uint8_t)(value >> 8));
    test_table_u8(table, (uint8_t)(value & 0xFFu));
}

void test_table_u32(test_table *table, uint32_t value)
{
    test_table_u16(table, (uint16_t)(value >> 16));
    test_table_u16(table, (uint16_t)(value & 0xFFFFu));
}

void test_table_set16(test_table *table, size_t offset, uint16_t value)
{
    TB_ASSERT(offset + 2 <= table->length);
    table->bytes[offset] = (uint8_t)(value >> 8);
    table->bytes[offset + 1] = (uint8_t)(value & 0xFFu);
}

void test_table_set32(test_table *table, size_t offset, uint32_t value)
{
    test_table_set16(table, offset, (uint16_t)(value >> 16));
    test_table_set16(table, offset + 2, (uint16_t)(value & 0xFFFFu));
}

void test_glyph_u8(test_glyph *glyph, uint8_t value)
{
    TB_ASSERT(glyph->length < sizeof glyph->bytes);
    glyph->bytes[glyph->length] = value;
    glyph->length++;
}

void test_glyph_u16(test_glyph *glyph, uint16_t value)
{
    test_glyph_u8(glyph, (uint8_t)(value >> 8));
    test_glyph_u8(glyph, (uint8_t)(value & 0xFFu));
}

test_table *test_font_table(test_font *font, const char *tag)
{
    for (size_t index = 0; index < font->count; index++)
    {
        if (tb_str_equal(font->tables[index].tag, tag))
        {
            return &font->tables[index];
        }
    }
    TB_ASSERT(font->count < TEST_FONT_MAX_TABLES);
    TB_ASSERT(tb_str_length(tag) == 4);
    test_table *table = &font->tables[font->count];
    font->count++;
    memcpy(table->tag, tag, 5);
    table->length = 0;
    return table;
}

void test_font_remove(test_font *font, const char *tag)
{
    for (size_t index = 0; index < font->count; index++)
    {
        if (tb_str_equal(font->tables[index].tag, tag))
        {
            font->count--;
            for (size_t rest = index; rest < font->count; rest++)
            {
                font->tables[rest] = font->tables[rest + 1];
            }
            return;
        }
    }
}

void test_simple_glyph(test_glyph *glyph, const test_point *points, size_t point_count,
                       const uint16_t *ends, size_t contours)
{
    int16_t x_min = INT16_MAX;
    int16_t y_min = INT16_MAX;
    int16_t x_max = INT16_MIN;
    int16_t y_max = INT16_MIN;
    for (size_t index = 0; index < point_count; index++)
    {
        x_min = points[index].x < x_min ? points[index].x : x_min;
        y_min = points[index].y < y_min ? points[index].y : y_min;
        x_max = points[index].x > x_max ? points[index].x : x_max;
        y_max = points[index].y > y_max ? points[index].y : y_max;
    }
    glyph->length = 0;
    test_glyph_u16(glyph, (uint16_t)contours);
    test_glyph_u16(glyph, (uint16_t)x_min);
    test_glyph_u16(glyph, (uint16_t)y_min);
    test_glyph_u16(glyph, (uint16_t)x_max);
    test_glyph_u16(glyph, (uint16_t)y_max);
    for (size_t contour = 0; contour < contours; contour++)
    {
        test_glyph_u16(glyph, ends[contour]);
    }
    test_glyph_u16(glyph, 0); /* No instructions. */
    for (size_t index = 0; index < point_count; index++)
    {
        /* Every coordinate as a 16-bit change from the last. */
        test_glyph_u8(glyph, points[index].on_curve ? 0x01 : 0x00);
    }
    int16_t previous = 0;
    for (size_t index = 0; index < point_count; index++)
    {
        test_glyph_u16(glyph, (uint16_t)(points[index].x - previous));
        previous = points[index].x;
    }
    previous = 0;
    for (size_t index = 0; index < point_count; index++)
    {
        test_glyph_u16(glyph, (uint16_t)(points[index].y - previous));
        previous = points[index].y;
    }
}

void test_compound_header(test_glyph *glyph)
{
    glyph->length = 0;
    test_glyph_u16(glyph, 0xFFFF); /* -1 contours: a compound. */
    for (size_t field = 0; field < 4; field++)
    {
        test_glyph_u16(glyph, 0);
    }
}

void test_font_set_glyphs(test_font *font, const test_glyph *glyphs, size_t count)
{
    test_table *glyf = test_font_table(font, "glyf");
    test_table *loca = test_font_table(font, "loca");
    test_table *hmtx = test_font_table(font, "hmtx");
    glyf->length = 0;
    loca->length = 0;
    hmtx->length = 0;
    for (size_t index = 0; index < count; index++)
    {
        test_table_u16(loca, (uint16_t)(glyf->length / 2));
        for (size_t byte = 0; byte < glyphs[index].length; byte++)
        {
            test_table_u8(glyf, glyphs[index].bytes[byte]);
        }
        if (glyf->length % 2 != 0)
        {
            test_table_u8(glyf, 0);
        }
        test_table_u16(hmtx, (uint16_t)(500 + index));
        test_table_u16(hmtx, (uint16_t)index);
    }
    test_table_u16(loca, (uint16_t)(glyf->length / 2));
    test_table_set16(test_font_table(font, "maxp"), 4, (uint16_t)count);
    test_table_set16(test_font_table(font, "hhea"), 34, (uint16_t)count);
}

void test_cmap_format4(test_table *cmap, const uint16_t *code_points, const uint16_t *glyphs,
                       size_t count)
{
    const size_t segments = count + 1;
    cmap->length = 0;
    test_table_u16(cmap, 0); /* version */
    test_table_u16(cmap, 1); /* one encoding record */
    test_table_u16(cmap, 3); /* Windows */
    test_table_u16(cmap, 1); /* Unicode BMP */
    test_table_u32(cmap, 12);
    test_table_u16(cmap, 4); /* format */
    test_table_u16(cmap, (uint16_t)(16 + segments * 8));
    test_table_u16(cmap, 0); /* language */
    test_table_u16(cmap, (uint16_t)(segments * 2));
    test_table_u16(cmap, 0); /* searchRange, entrySelector, rangeShift: unused */
    test_table_u16(cmap, 0);
    test_table_u16(cmap, 0);
    for (size_t index = 0; index < count; index++)
    {
        test_table_u16(cmap, code_points[index]);
    }
    test_table_u16(cmap, 0xFFFF);
    test_table_u16(cmap, 0); /* reservedPad */
    for (size_t index = 0; index < count; index++)
    {
        test_table_u16(cmap, code_points[index]);
    }
    test_table_u16(cmap, 0xFFFF);
    for (size_t index = 0; index < count; index++)
    {
        test_table_u16(cmap, (uint16_t)(glyphs[index] - code_points[index]));
    }
    test_table_u16(cmap, 1);
    for (size_t index = 0; index < segments; index++)
    {
        test_table_u16(cmap, 0);
    }
}

/* A name record's string in UTF-16BE, of ASCII text. */
static void test_name_record(test_table *name, uint16_t id, const char *text, size_t *storage,
                             test_table *strings)
{
    const size_t length = tb_str_length(text);
    test_table_u16(name, 3);
    test_table_u16(name, 1);
    test_table_u16(name, 0x0409);
    test_table_u16(name, id);
    test_table_u16(name, (uint16_t)(length * 2));
    test_table_u16(name, (uint16_t)*storage);
    for (size_t index = 0; index < length; index++)
    {
        test_table_u16(strings, (uint8_t)text[index]);
    }
    *storage += length * 2;
}

static void test_font_names(test_font *font)
{
    test_table *name = test_font_table(font, "name");
    test_table *strings = TB_NEW(test_table);
    size_t storage = 0;
    test_table_u16(name, 0);
    test_table_u16(name, 2);
    test_table_u16(name, 6 + 2 * 12);
    test_name_record(name, 1, "Test", &storage, strings);
    test_name_record(name, 2, "Regular", &storage, strings);
    for (size_t index = 0; index < strings->length; index++)
    {
        test_table_u8(name, strings->bytes[index]);
    }
    tb_free(strings);
}

static void test_font_os2(test_font *font)
{
    test_table *os2 = test_font_table(font, "OS/2");
    for (size_t index = 0; index < 96; index++)
    {
        test_table_u8(os2, 0);
    }
    test_table_set16(os2, 0, 4);       /* version */
    test_table_set16(os2, 4, 400);     /* usWeightClass */
    test_table_set16(os2, 62, 0x0040); /* fsSelection: regular */
    test_table_set16(os2, 86, 500);    /* sxHeight */
    test_table_set16(os2, 88, 700);    /* sCapHeight */
}

test_font *test_font_create(void)
{
    test_font *font = TB_NEW(test_font);
    test_table *head = test_font_table(font, "head");
    for (size_t index = 0; index < 54; index++)
    {
        test_table_u8(head, 0);
    }
    test_table_set32(head, 0, 0x00010000);
    test_table_set32(head, 12, 0x5F0F3CF5);
    test_table_set16(head, 18, 1000);
    test_table_set16(head, 40, 100);
    test_table_set16(head, 42, 100);
    test_table *hhea = test_font_table(font, "hhea");
    for (size_t index = 0; index < 36; index++)
    {
        test_table_u8(hhea, 0);
    }
    test_table_set32(hhea, 0, 0x00010000);
    test_table_set16(hhea, 4, 800);
    test_table_set16(hhea, 6, (uint16_t)-200);
    test_table_set16(hhea, 8, 90);
    test_table *maxp = test_font_table(font, "maxp");
    test_table_u32(maxp, 0x00005000);
    test_table_u16(maxp, 0);

    test_glyph glyphs[3] = {0};
    const test_point square[] = {{0, 0, true}, {0, 100, true}, {100, 100, true}, {100, 0, true}};
    const uint16_t square_ends[] = {3};
    test_simple_glyph(&glyphs[1], square, 4, square_ends, 1);
    const test_point diamond[] = {
        {50, 0, false}, {0, 50, false}, {50, 100, false}, {100, 50, false}};
    test_simple_glyph(&glyphs[2], diamond, 4, square_ends, 1);
    test_font_set_glyphs(font, glyphs, 3);

    const uint16_t code_points[] = {'A', 'B'};
    const uint16_t indices[] = {1, 2};
    test_cmap_format4(test_font_table(font, "cmap"), code_points, indices, 2);
    test_font_names(font);
    test_font_os2(font);
    return font;
}

void test_font_destroy(test_font *font)
{
    tb_free(font);
}

size_t test_font_write(const test_font *font, uint8_t *bytes, size_t capacity)
{
    size_t length = 12 + font->count * 16;
    TB_ASSERT(length <= capacity);
    memset(bytes, 0, length);
    bytes[0] = 0;
    bytes[1] = 1;
    bytes[4] = (uint8_t)(font->count >> 8);
    bytes[5] = (uint8_t)(font->count & 0xFFu);
    for (size_t index = 0; index < font->count; index++)
    {
        const test_table *table = &font->tables[index];
        uint8_t *record = bytes + 12 + index * 16;
        memcpy(record, table->tag, 4);
        const uint32_t fields[3] = {0, (uint32_t)length, (uint32_t)table->length};
        for (size_t field = 0; field < 3; field++)
        {
            record[4 + field * 4] = (uint8_t)(fields[field] >> 24);
            record[5 + field * 4] = (uint8_t)((fields[field] >> 16) & 0xFFu);
            record[6 + field * 4] = (uint8_t)((fields[field] >> 8) & 0xFFu);
            record[7 + field * 4] = (uint8_t)(fields[field] & 0xFFu);
        }
        TB_ASSERT(table->length <= capacity - length);
        memcpy(bytes + length, table->bytes, table->length);
        length += table->length;
        /* Tables begin on four-byte boundaries. */
        while (length % 4 != 0)
        {
            TB_ASSERT(length < capacity);
            bytes[length] = 0;
            length++;
        }
    }
    return length;
}
