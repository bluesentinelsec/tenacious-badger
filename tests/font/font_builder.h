/* Small TrueType fonts made in memory, table by table, for the cases of the
 * font reader that the fonts of tests/data/fonts do not reach: formats they
 * do not use, and damage at a chosen place. */
#ifndef TB_TEST_FONT_BUILDER_H
#define TB_TEST_FONT_BUILDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TEST_TABLE_CAPACITY ((size_t)8192)
#define TEST_FONT_MAX_TABLES ((size_t)16)

/* One table: its tag and its bytes. */
typedef struct test_table
{
    char tag[5];
    uint8_t bytes[TEST_TABLE_CAPACITY];
    size_t length;
} test_table;

/* A font's tables, written into a file in the order they were added. */
typedef struct test_font
{
    test_table tables[TEST_FONT_MAX_TABLES];
    size_t count;
} test_font;

/* A point of a simple glyph. */
typedef struct test_point
{
    int16_t x;
    int16_t y;
    bool on_curve;
} test_point;

/* The bytes of one glyph in glyf; none for a blank glyph. */
typedef struct test_glyph
{
    uint8_t bytes[1024];
    size_t length;
} test_glyph;

/* A valid font of three glyphs: 0, blank; 1, a square from (0, 0) to (100,
 * 100) that 'A' maps to; 2, a diamond of off-curve points that 'B' maps to.
 * Its family is "Test" and its style "Regular", it weighs 400, its cap
 * height is 700 and its x height 500. Release it with test_font_destroy(). */
test_font *test_font_create(void);
void test_font_destroy(test_font *font);

/* The table with a tag, added empty when the font has none. */
test_table *test_font_table(test_font *font, const char *tag);
void test_font_remove(test_font *font, const char *tag);

/* Append to a table, big-endian. */
void test_table_u8(test_table *table, uint8_t value);
void test_table_u16(test_table *table, uint16_t value);
void test_table_u32(test_table *table, uint32_t value);
/* Overwrite at an offset within the table. */
void test_table_set16(test_table *table, size_t offset, uint16_t value);
void test_table_set32(test_table *table, size_t offset, uint32_t value);

/* Write the glyphs into glyf and loca (short offsets), and make maxp, hhea,
 * and hmtx agree: glyph g advances 500 + g with a bearing of g. */
void test_font_set_glyphs(test_font *font, const test_glyph *glyphs, size_t count);

/* A simple glyph of `contours` contours, each ending at its entry of `ends`. */
void test_simple_glyph(test_glyph *glyph, const test_point *points, size_t point_count,
                       const uint16_t *ends, size_t contours);

/* Append to a glyph's bytes, big-endian, as for a compound glyph's components. */
void test_glyph_u8(test_glyph *glyph, uint8_t value);
void test_glyph_u16(test_glyph *glyph, uint16_t value);
/* Begin a compound glyph: its header with a box of zero. */
void test_compound_header(test_glyph *glyph);

/* A cmap table of one Windows Unicode subtable of format 4 that maps each
 * of `count` code points to its glyph, one segment each, and ends with the
 * required segment for 0xFFFF. */
void test_cmap_format4(test_table *cmap, const uint16_t *code_points, const uint16_t *glyphs,
                       size_t count);

/* A cmap of a format 4 subtable for (3, 1), whose one real segment maps
 * 'a' to 1 and 'c' to 2 through its glyph array, and a format 12 subtable
 * for (3, 10): U+1F600 to 2, U+20000 and U+20001 to 1 and 2, and U+30000
 * to glyph 50, which the font does not have. */
void test_cmap_two_subtables(test_table *cmap);

/* A kern table of `skipped` vertical subtables, then the one that kerns
 * glyph 1 then 2 by -40, and 2 then 1 by +25. */
void test_kern_table(test_table *kern, size_t skipped);

/* Where test_gpos_table() put its parts, as offsets in GPOS. */
typedef struct test_gpos_layout
{
    size_t list;       /* The lookup list. */
    size_t lookups[5]; /* Each lookup. */
    size_t extension;  /* Lookup 0's extension. */
    size_t pairs;      /* Its format 1 PairPos. */
    size_t classes;    /* Lookup 1's first subtable, of format 2. */
    size_t second;     /* Lookup 1's second subtable. */
    size_t marks;      /* Lookup 4's PairPos. */
} test_gpos_layout;

/* GPOS with a 'mark' feature of lookup 4 and a 'kern' feature of lookups
 * 1, 0, 2, 3, and 1 again, which together kern 1 then 2 by -35, 1 then 0
 * by +77, and 0 then anything by +500; see font_builder.c. The 'kern'
 * feature's tag is at offset 18. */
void test_gpos_table(test_table *gpos, test_gpos_layout *layout);

/* Write the font into `bytes` of `capacity`; returns its length. */
size_t test_font_write(const test_font *font, uint8_t *bytes, size_t capacity);

#endif
