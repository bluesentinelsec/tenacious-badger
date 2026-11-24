/* The TrueType reader and damaged fonts: each check the reader makes,
 * provoked on a font made in memory, must fail with its reason; and the
 * fonts of the font pack, cut short and corrupted at thousands of places,
 * must open cleanly or fail with a reason, and every query of what opens
 * must stay within the bytes. Each damaged font is in memory of exactly its
 * length, so that the sanitizer build reports any read past its end. */
#include "badger/arena.h"
#include "badger/font.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "font_builder.h"
#include "tb_test.h"
#include "test_data.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern const tb_test_file tb_font_files[];
extern const size_t tb_font_files_count;

#define FONT_BUFFER ((size_t)65536)

/* Write a font made in memory into memory of exactly its length, and open it. */
static tb_font_result open_exact(const test_font *made, tb_font **font, tb_font_error *error,
                                 uint8_t **bytes)
{
    uint8_t *buffer = tb_malloc(FONT_BUFFER);
    const size_t length = test_font_write(made, buffer, FONT_BUFFER);
    *bytes = tb_malloc(length);
    memcpy(*bytes, buffer, length);
    tb_free(buffer);
    return tb_font_open(*bytes, length, NULL, font, error);
}

/* -------------------------------------------------------------------------
 * Each check of opening.
 * ------------------------------------------------------------------------- */

typedef struct damage_case
{
    const char *name;
    void (*damage)(test_font *font);
    tb_font_result result;
    const char *table;
    const char *reason;
} damage_case;

static test_table *table(test_font *font, const char *tag)
{
    return test_font_table(font, tag);
}

/* The directory and the required tables. */
static void undamaged(test_font *font)
{
    (void)font;
}
static void missing_head(test_font *font)
{
    test_font_remove(font, "head");
}
static void missing_hhea(test_font *font)
{
    test_font_remove(font, "hhea");
}
static void missing_maxp(test_font *font)
{
    test_font_remove(font, "maxp");
}
static void missing_hmtx(test_font *font)
{
    test_font_remove(font, "hmtx");
}
static void missing_loca(test_font *font)
{
    test_font_remove(font, "loca");
}
static void missing_glyf(test_font *font)
{
    test_font_remove(font, "glyf");
}
static void missing_cmap(test_font *font)
{
    test_font_remove(font, "cmap");
}
static void short_head(test_font *font)
{
    table(font, "head")->length = 53;
}
static void head_magic(test_font *font)
{
    test_table_set32(table(font, "head"), 12, 0x5F0F3CF6);
}
static void small_em(test_font *font)
{
    test_table_set16(table(font, "head"), 18, 15);
}
static void large_em(test_font *font)
{
    test_table_set16(table(font, "head"), 18, 16385);
}
static void loca_format(test_font *font)
{
    test_table_set16(table(font, "head"), 50, 2);
}
static void glyph_format(test_font *font)
{
    test_table_set16(table(font, "head"), 52, 1);
}
static void short_maxp(test_font *font)
{
    table(font, "maxp")->length = 5;
}
static void maxp_version(test_font *font)
{
    test_table_set32(table(font, "maxp"), 0, 0x00020000);
}
static void no_glyphs(test_font *font)
{
    test_table_set16(table(font, "maxp"), 4, 0);
}
static void short_hhea(test_font *font)
{
    table(font, "hhea")->length = 35;
}
static void no_metrics(test_font *font)
{
    test_table_set16(table(font, "hhea"), 34, 0);
}
static void too_many_metrics(test_font *font)
{
    test_table_set16(table(font, "hhea"), 34, 4);
}
static void short_hmtx(test_font *font)
{
    table(font, "hmtx")->length = 10;
}
static void short_loca(test_font *font)
{
    table(font, "loca")->length = 6;
}
static void loca_backward(test_font *font)
{
    test_table_set16(table(font, "loca"), 6, 1);
}
static void loca_past_glyf(test_font *font)
{
    test_table_set16(table(font, "loca"), 6, 0xFFFF);
}
static void long_loca(test_font *font)
{
    /* 32-bit offsets, the last past the end of glyf. */
    test_table *loca = table(font, "loca");
    test_table *glyf = table(font, "glyf");
    loca->length = 0;
    test_table_u32(loca, 0);
    test_table_u32(loca, 0);
    test_table_u32(loca, 0);
    test_table_u32(loca, (uint32_t)glyf->length + 1);
    test_table_set16(table(font, "head"), 50, 1);
}
static void short_os2(test_font *font)
{
    table(font, "OS/2")->length = 67;
}
static void short_os2_version_2(test_font *font)
{
    table(font, "OS/2")->length = 89;
}

/* cmap, of format 4 at 12 with segments for 'A', 'B', and 0xFFFF. */
static void short_cmap(test_font *font)
{
    table(font, "cmap")->length = 3;
}
static void cmap_records(test_font *font)
{
    test_table_set16(table(font, "cmap"), 2, 100);
}
static void cmap_subtable_past(test_font *font)
{
    test_table_set32(table(font, "cmap"), 8, 1000);
}
static void cmap_not_unicode(test_font *font)
{
    test_table_set16(table(font, "cmap"), 4, 1);
    test_table_set16(table(font, "cmap"), 6, 0);
}
static void format4_header(test_font *font)
{
    table(font, "cmap")->length = 22;
}
static void format4_odd_segments(test_font *font)
{
    test_table_set16(table(font, "cmap"), 18, 5);
}
static void format4_no_segments(test_font *font)
{
    test_table_set16(table(font, "cmap"), 18, 0);
}
static void format4_segments_cut(test_font *font)
{
    test_table_set16(table(font, "cmap"), 18, 200);
}
static void format4_start_after_end(test_font *font)
{
    test_table_set16(table(font, "cmap"), 34, 'C');
}
static void format4_ends_out_of_order(test_font *font)
{
    test_table_set16(table(font, "cmap"), 28, 'A');
}
static void format4_range_past(test_font *font)
{
    test_table_set16(table(font, "cmap"), 46, 1000);
}
static void format12_header(test_font *font)
{
    test_cmap_two_subtables(table(font, "cmap"));
    table(font, "cmap")->length = 78;
}
static void format12_groups_cut(test_font *font)
{
    test_cmap_two_subtables(table(font, "cmap"));
    test_table_set32(table(font, "cmap"), 80, 1000);
}
static void format12_overlap(test_font *font)
{
    test_cmap_two_subtables(table(font, "cmap"));
    test_table_set32(table(font, "cmap"), 96, 0x1F600);
}
static void format12_start_after_end(test_font *font)
{
    test_cmap_two_subtables(table(font, "cmap"));
    test_table_set32(table(font, "cmap"), 88, 0x1F5FF);
}

/* name: two records, then their strings. */
static void short_name(test_font *font)
{
    table(font, "name")->length = 5;
}
static void name_records(test_font *font)
{
    test_table_set16(table(font, "name"), 2, 100);
}
static void name_string_past(test_font *font)
{
    test_table_set16(table(font, "name"), 4, 0xFFFF);
}

/* kern: one skipped vertical subtable at 4, then the one of pairs at 18. */
static test_table *kern(test_font *font)
{
    test_table *kern_table = table(font, "kern");
    test_kern_table(kern_table, 1);
    return kern_table;
}
static void short_kern(test_font *font)
{
    kern(font)->length = 3;
}
static void kern_subtable_cut(test_font *font)
{
    test_table *kern_table = kern(font);
    test_table_set16(kern_table, 2, 3);
    test_table_set16(kern_table, 22, 0x0000);
}
static void kern_pairs_header(test_font *font)
{
    kern(font)->length = 4 + 14 + 6 + 4;
}
static void kern_pairs_cut(test_font *font)
{
    test_table_set16(kern(font), 24, 100);
}
static void kern_pairs_out_of_order(test_font *font)
{
    test_table_set16(kern(font), 32, 3);
}
static void kern_subtable_length(test_font *font)
{
    test_table_set16(kern(font), 6, 2);
}

/* GPOS. */
static test_gpos_layout gpos(test_font *font)
{
    test_gpos_layout layout = {0};
    test_gpos_table(table(font, "GPOS"), &layout);
    return layout;
}
static void set_gpos(test_font *font, size_t offset, uint16_t value)
{
    test_table_set16(table(font, "GPOS"), offset, value);
}
static void short_gpos(test_font *font)
{
    (void)gpos(font);
    table(font, "GPOS")->length = 9;
}
static void gpos_version_2(test_font *font)
{
    (void)gpos(font);
    set_gpos(font, 0, 2);
}
static void lookup_list_past(test_font *font)
{
    (void)gpos(font);
    set_gpos(font, 8, 0xFFFF);
}
static void lookup_list_cut(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.list, 1000);
}
static void no_lookups(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.list, 0);
}
static void feature_list_past(test_font *font)
{
    (void)gpos(font);
    set_gpos(font, 6, 0xFFFF);
}
static void feature_list_cut(test_font *font)
{
    (void)gpos(font);
    set_gpos(font, 10, 1000);
}
static void feature_past(test_font *font)
{
    (void)gpos(font);
    set_gpos(font, 22, 0xFFF0);
}
static void feature_lookups_cut(test_font *font)
{
    (void)gpos(font);
    set_gpos(font, 32, 1000);
}
static void feature_lookup_missing(test_font *font)
{
    (void)gpos(font);
    set_gpos(font, 34, 9);
}
static void lookup_past(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.list + 4, 0xFFF0);
}
static void lookup_subtables_cut(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.lookups[1] + 4, 1000);
}
static void extension_format(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.extension, 2);
}
static void extension_cut(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.lookups[0] + 6,
             (uint16_t)(table(font, "GPOS")->length - layout.lookups[0] - 4));
}
static void extension_past(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    test_table_set32(table(font, "GPOS"), layout.extension + 4, 0xFFFFFF);
}
static void pair_cut(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.lookups[1] + 6,
             (uint16_t)(table(font, "GPOS")->length - layout.lookups[1] - 4));
}
static void value_format(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.classes + 4, 0x0104);
}
static void pair_format(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.classes, 3);
}
static void pair2_header_cut(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    table(font, "GPOS")->length = layout.second + 12;
}
static void pair_sets_cut(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.pairs + 8, 1000);
}
static void pair_set_past(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.pairs + 10, 0xFFF0);
}
static void pair_set_cut(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.pairs + 18, 1000);
}
static void pair_set_out_of_order(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.pairs + 26, 0);
}
static void coverage_past_records(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.pairs + 14, 2);
}
static void coverage_past(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.pairs + 2, 0xFFF0);
}
static void coverage_format(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.pairs + 12, 3);
}
static void coverage_glyphs_cut(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.pairs + 14, 0xFFFF);
}
static void coverage_glyphs_out_of_order(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.second + 22, 1);
    set_gpos(font, layout.second + 24, 0);
}
static void coverage_ranges_cut(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.classes + 26, 1000);
}
static void coverage_range_backward(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.classes + 28, 3);
}
static void coverage_range_past_records(test_font *font)
{
    /* Format 2 coverage in a format 1 PairPos: glyphs 1 to 2 from index 0, of one pair set. */
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.pairs + 12, 2);
}
static void classes_past(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.classes + 8, 0xFFF0);
}
static void classes_format(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.classes + 34, 3);
}
static void class_array_cut(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.classes + 38, 1000);
}
static void class_array_range(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.classes + 40, 5);
}
static void class_ranges_cut(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.classes + 46, 1000);
}
static void class_range_backward(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.classes + 48, 3);
}
static void class_range_class(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.classes + 52, 2);
}
static void no_classes(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.classes + 12, 0);
}
static void class_pairs_cut(test_font *font)
{
    const test_gpos_layout layout = gpos(font);
    set_gpos(font, layout.classes + 12, 1000);
}

static const damage_case damage_cases[] = {
    {"missing head", missing_head, TB_FONT_MALFORMED, "head", "a required table is missing"},
    {"missing hhea", missing_hhea, TB_FONT_MALFORMED, "hhea", "a required table is missing"},
    {"missing maxp", missing_maxp, TB_FONT_MALFORMED, "maxp", "a required table is missing"},
    {"missing hmtx", missing_hmtx, TB_FONT_MALFORMED, "hmtx", "a required table is missing"},
    {"missing loca", missing_loca, TB_FONT_MALFORMED, "loca", "a required table is missing"},
    {"missing glyf", missing_glyf, TB_FONT_MALFORMED, "glyf", "a required table is missing"},
    {"missing cmap", missing_cmap, TB_FONT_MALFORMED, "cmap", "a required table is missing"},
    {"undamaged", undamaged, TB_FONT_OK, "", ""},
    {"short head", short_head, TB_FONT_MALFORMED, "head", "the table is too short"},
    {"head magic", head_magic, TB_FONT_MALFORMED, "head", "the magic number is wrong"},
    {"small em", small_em, TB_FONT_MALFORMED, "head", "units per em are outside 16 to 16384"},
    {"large em", large_em, TB_FONT_MALFORMED, "head", "units per em are outside 16 to 16384"},
    {"loca format", loca_format, TB_FONT_MALFORMED, "head", "the loca format is unknown"},
    {"glyph format", glyph_format, TB_FONT_UNSUPPORTED, "head", "the glyph data format is unknown"},
    {"short maxp", short_maxp, TB_FONT_MALFORMED, "maxp", "the table is too short"},
    {"maxp version", maxp_version, TB_FONT_MALFORMED, "maxp", "the version is unknown"},
    {"no glyphs", no_glyphs, TB_FONT_MALFORMED, "maxp", "the font has no glyphs"},
    {"short hhea", short_hhea, TB_FONT_MALFORMED, "hhea", "the table is too short"},
    {"no metrics", no_metrics, TB_FONT_MALFORMED, "hhea",
     "the count of horizontal metrics is outside 1 to the glyph count"},
    {"too many metrics", too_many_metrics, TB_FONT_MALFORMED, "hhea",
     "the count of horizontal metrics is outside 1 to the glyph count"},
    {"short hmtx", short_hmtx, TB_FONT_MALFORMED, "hmtx", "the table is too short for every glyph"},
    {"short loca", short_loca, TB_FONT_MALFORMED, "loca", "the table is too short for every glyph"},
    {"loca backward", loca_backward, TB_FONT_MALFORMED, "loca", "glyph offsets go backward"},
    {"loca past glyf", loca_past_glyf, TB_FONT_MALFORMED, "loca",
     "a glyph runs past the end of glyf"},
    {"long loca", long_loca, TB_FONT_MALFORMED, "loca", "a glyph runs past the end of glyf"},
    {"short OS/2", short_os2, TB_FONT_MALFORMED, "OS/2", "the table is too short"},
    {"short OS/2 version 2", short_os2_version_2, TB_FONT_MALFORMED, "OS/2",
     "the table is too short for its version"},
    {"short cmap", short_cmap, TB_FONT_MALFORMED, "cmap", "the table is too short"},
    {"cmap records", cmap_records, TB_FONT_MALFORMED, "cmap", "the encoding records are cut off"},
    {"cmap subtable past", cmap_subtable_past, TB_FONT_MALFORMED, "cmap",
     "a subtable is past the end of the table"},
    {"cmap not Unicode", cmap_not_unicode, TB_FONT_UNSUPPORTED, "cmap",
     "no Unicode subtable of format 4 or 12"},
    {"format 4 header", format4_header, TB_FONT_MALFORMED, "cmap",
     "the format 4 header is cut off"},
    {"format 4 odd segments", format4_odd_segments, TB_FONT_MALFORMED, "cmap",
     "the segment count is not valid"},
    {"format 4 no segments", format4_no_segments, TB_FONT_MALFORMED, "cmap",
     "the segment count is not valid"},
    {"format 4 segments cut", format4_segments_cut, TB_FONT_MALFORMED, "cmap",
     "the segments are cut off"},
    {"format 4 start after end", format4_start_after_end, TB_FONT_MALFORMED, "cmap",
     "the segments are out of order"},
    {"format 4 ends out of order", format4_ends_out_of_order, TB_FONT_MALFORMED, "cmap",
     "the segments are out of order"},
    {"format 4 range past", format4_range_past, TB_FONT_MALFORMED, "cmap",
     "a segment maps past the end of the table"},
    {"format 12 header", format12_header, TB_FONT_MALFORMED, "cmap",
     "the format 12 header is cut off"},
    {"format 12 groups cut", format12_groups_cut, TB_FONT_MALFORMED, "cmap",
     "the groups are cut off"},
    {"format 12 overlap", format12_overlap, TB_FONT_MALFORMED, "cmap",
     "the groups are out of order"},
    {"format 12 start after end", format12_start_after_end, TB_FONT_MALFORMED, "cmap",
     "the groups are out of order"},
    {"short name", short_name, TB_FONT_MALFORMED, "name", "the table is too short"},
    {"name records", name_records, TB_FONT_MALFORMED, "name", "the name records are cut off"},
    {"name string past", name_string_past, TB_FONT_MALFORMED, "name",
     "a string runs past the end of the table"},
    {"short kern", short_kern, TB_FONT_MALFORMED, "kern", "the table is too short"},
    {"kern subtable cut", kern_subtable_cut, TB_FONT_MALFORMED, "kern", "a subtable is cut off"},
    {"kern pairs header", kern_pairs_header, TB_FONT_MALFORMED, "kern", "a subtable is cut off"},
    {"kern pairs cut", kern_pairs_cut, TB_FONT_MALFORMED, "kern", "the pairs are cut off"},
    {"kern pairs out of order", kern_pairs_out_of_order, TB_FONT_MALFORMED, "kern",
     "the pairs are out of order"},
    {"kern subtable length", kern_subtable_length, TB_FONT_MALFORMED, "kern",
     "a subtable's length is too short"},
    {"short GPOS", short_gpos, TB_FONT_MALFORMED, "GPOS", "the table is too short"},
    {"GPOS version 2", gpos_version_2, TB_FONT_OK, "", ""},
    {"lookup list past", lookup_list_past, TB_FONT_MALFORMED, "GPOS", "the lookup list is cut off"},
    {"lookup list cut", lookup_list_cut, TB_FONT_MALFORMED, "GPOS", "the lookup list is cut off"},
    {"no lookups", no_lookups, TB_FONT_OK, "", ""},
    {"feature list past", feature_list_past, TB_FONT_MALFORMED, "GPOS",
     "the feature list is cut off"},
    {"feature list cut", feature_list_cut, TB_FONT_MALFORMED, "GPOS",
     "the feature list is cut off"},
    {"feature past", feature_past, TB_FONT_MALFORMED, "GPOS", "a feature is cut off"},
    {"feature lookups cut", feature_lookups_cut, TB_FONT_MALFORMED, "GPOS", "a feature is cut off"},
    {"feature lookup missing", feature_lookup_missing, TB_FONT_MALFORMED, "GPOS",
     "a feature names a lookup that does not exist"},
    {"lookup past", lookup_past, TB_FONT_MALFORMED, "GPOS", "a lookup is cut off"},
    {"lookup subtables cut", lookup_subtables_cut, TB_FONT_MALFORMED, "GPOS",
     "a lookup is cut off"},
    {"extension format", extension_format, TB_FONT_MALFORMED, "GPOS", "an extension is malformed"},
    {"extension cut", extension_cut, TB_FONT_MALFORMED, "GPOS", "an extension is malformed"},
    {"extension past", extension_past, TB_FONT_MALFORMED, "GPOS", "an extension is malformed"},
    {"pair cut", pair_cut, TB_FONT_MALFORMED, "GPOS", "a pair subtable is cut off"},
    {"value format", value_format, TB_FONT_MALFORMED, "GPOS", "a value format is unknown"},
    {"pair format", pair_format, TB_FONT_MALFORMED, "GPOS", "a pair format is unknown"},
    {"pair 2 header cut", pair2_header_cut, TB_FONT_MALFORMED, "GPOS",
     "a pair subtable is cut off"},
    {"pair sets cut", pair_sets_cut, TB_FONT_MALFORMED, "GPOS", "the pair sets are cut off"},
    {"pair set past", pair_set_past, TB_FONT_MALFORMED, "GPOS", "a pair set is cut off"},
    {"pair set cut", pair_set_cut, TB_FONT_MALFORMED, "GPOS", "a pair set is cut off"},
    {"pair set out of order", pair_set_out_of_order, TB_FONT_MALFORMED, "GPOS",
     "a pair set is out of order"},
    {"coverage past records", coverage_past_records, TB_FONT_MALFORMED, "GPOS",
     "a coverage index is past its subtable's records"},
    {"coverage past", coverage_past, TB_FONT_MALFORMED, "GPOS", "a coverage table is cut off"},
    {"coverage format", coverage_format, TB_FONT_MALFORMED, "GPOS", "a coverage format is unknown"},
    {"coverage glyphs cut", coverage_glyphs_cut, TB_FONT_MALFORMED, "GPOS",
     "a coverage table is cut off"},
    {"coverage glyphs out of order", coverage_glyphs_out_of_order, TB_FONT_MALFORMED, "GPOS",
     "a coverage table is out of order"},
    {"coverage ranges cut", coverage_ranges_cut, TB_FONT_MALFORMED, "GPOS",
     "a coverage table is cut off"},
    {"coverage range backward", coverage_range_backward, TB_FONT_MALFORMED, "GPOS",
     "a coverage table is out of order"},
    {"coverage range past records", coverage_range_past_records, TB_FONT_MALFORMED, "GPOS",
     "a coverage index is past its subtable's records"},
    {"classes past", classes_past, TB_FONT_MALFORMED, "GPOS", "a class table is cut off"},
    {"classes format", classes_format, TB_FONT_MALFORMED, "GPOS", "a class format is unknown"},
    {"class array cut", class_array_cut, TB_FONT_MALFORMED, "GPOS", "a class table is cut off"},
    {"class array range", class_array_range, TB_FONT_MALFORMED, "GPOS", "a class is out of range"},
    {"class ranges cut", class_ranges_cut, TB_FONT_MALFORMED, "GPOS", "a class table is cut off"},
    {"class range backward", class_range_backward, TB_FONT_MALFORMED, "GPOS",
     "a class table is out of order"},
    {"class range class", class_range_class, TB_FONT_MALFORMED, "GPOS", "a class is out of range"},
    {"no classes", no_classes, TB_FONT_MALFORMED, "GPOS", "a pair subtable has no classes"},
    {"class pairs cut", class_pairs_cut, TB_FONT_MALFORMED, "GPOS", "the class pairs are cut off"},
};

TB_TEST(font_damage, every_check_of_opening_fails_with_its_reason)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(damage_cases); index++)
    {
        const damage_case *each = &damage_cases[index];
        TB_TEST_CONTEXT("%s", each->name);
        test_font *made = test_font_create();
        each->damage(made);
        tb_font *font = NULL;
        tb_font_error error = {0};
        uint8_t *bytes = NULL;
        const tb_font_result result = open_exact(made, &font, &error, &bytes);
        TB_TEST_EXPECT_UINT(result, ==, each->result);
        TB_TEST_EXPECT_UINT(error.result, ==, each->result);
        TB_TEST_EXPECT_STRING(error.table, ==, each->table);
        TB_TEST_EXPECT_STRING(error.reason, ==, each->reason);
        TB_TEST_EXPECT((font != NULL) == (result == TB_FONT_OK));
        tb_font_close(font);
        tb_free(bytes);
        test_font_destroy(made);
    }
}

/* Open bytes of a length; the bytes are copied to memory of exactly that length. */
static tb_font_result open_bytes(const uint8_t *source, size_t length, tb_font_error *error)
{
    uint8_t *bytes = tb_malloc(length == 0 ? 1 : length);
    if (length != 0)
    {
        memcpy(bytes, source, length);
    }
    tb_font *font = NULL;
    const tb_font_result result = tb_font_open(bytes, length, NULL, &font, error);
    tb_font_close(font);
    tb_free(bytes);
    return result;
}

TB_TEST(font_damage, the_directory_is_checked)
{
    test_font *made = test_font_create();
    uint8_t *bytes = tb_malloc(FONT_BUFFER);
    const size_t length = test_font_write(made, bytes, FONT_BUFFER);
    tb_font_error error = {0};

    TB_TEST_EXPECT_UINT(open_bytes(bytes, 0, &error), ==, TB_FONT_TRUNCATED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the table directory is cut off");
    TB_TEST_EXPECT_UINT(open_bytes(bytes, 11, &error), ==, TB_FONT_TRUNCATED);
    TB_TEST_EXPECT_UINT(open_bytes(bytes, 40, &error), ==, TB_FONT_TRUNCATED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the table directory is cut off");
    TB_TEST_EXPECT_UINT(open_bytes(bytes, length - 8, &error), ==, TB_FONT_TRUNCATED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "a table runs past the end of the bytes");

    /* Apple's tag for TrueType outlines is TrueType too. */
    memcpy(bytes, "true", 4);
    TB_TEST_EXPECT_UINT(open_bytes(bytes, length, &error), ==, TB_FONT_OK);
    memcpy(bytes, "OTTO", 4);
    TB_TEST_EXPECT_UINT(open_bytes(bytes, length, &error), ==, TB_FONT_UNSUPPORTED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the font has PostScript (CFF) outlines, not TrueType");
    TB_TEST_EXPECT_UINT(open_bytes(bytes, 8, &error), ==, TB_FONT_UNSUPPORTED);
    memcpy(bytes, "ttcf", 4);
    TB_TEST_EXPECT_UINT(open_bytes(bytes, length, &error), ==, TB_FONT_UNSUPPORTED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the file is a font collection, not one font");
    memcpy(bytes, "\x89PNG", 4);
    TB_TEST_EXPECT_UINT(open_bytes(bytes, length, &error), ==, TB_FONT_MALFORMED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the bytes are not a TrueType font");
    TB_TEST_EXPECT_STRING(error.table, ==, "");
    bytes[0] = 0;
    bytes[1] = 1;
    bytes[2] = 0;
    bytes[3] = 0;

    /* Too many tables for the bytes. */
    bytes[4] = 0x10;
    TB_TEST_EXPECT_UINT(open_bytes(bytes, length, &error), ==, TB_FONT_TRUNCATED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the table directory is cut off");
    bytes[4] = 0;

    /* Two tables of one tag: the first is read. The name table becomes a
     * second head, so the font has no names. */
    for (size_t record = 0; record < made->count; record++)
    {
        uint8_t *tag = bytes + 12 + record * 16;
        if (memcmp(tag, "name", 4) == 0)
        {
            memcpy(tag, "head", 4);
        }
    }
    tb_font *font = NULL;
    TB_TEST_REQUIRE_UINT(tb_font_open(bytes, length, NULL, &font, &error), ==, TB_FONT_OK);
    tb_font_style style = {0};
    tb_font_get_style(font, &style);
    TB_TEST_EXPECT_STRING(style.family, ==, "");
    tb_font_close(font);
    tb_free(bytes);
    test_font_destroy(made);
}

/* -------------------------------------------------------------------------
 * Each check of reading an outline.
 * ------------------------------------------------------------------------- */

enum
{
    ON = 0x01,
    X_SHORT = 0x02,
    REPEAT = 0x08,
    X_SAME = 0x10,
    Y_SAME = 0x20,
    WORDS = 0x0001,
    XY = 0x0002,
    MORE = 0x0020
};

/* A simple glyph's header and contour ends, before its instructions. */
static void simple_header(test_glyph *glyph, const uint16_t *ends, size_t contours)
{
    glyph->length = 0;
    test_glyph_u16(glyph, (uint16_t)contours);
    for (size_t field = 0; field < 4; field++)
    {
        test_glyph_u16(glyph, 0);
    }
    for (size_t contour = 0; contour < contours; contour++)
    {
        test_glyph_u16(glyph, ends[contour]);
    }
}

typedef struct outline_case
{
    const char *name;
    void (*make)(test_glyph *glyphs);
    tb_font_result result;
    const char *reason;
} outline_case;

static const uint16_t four_points[] = {3};

static void glyph_header_cut(test_glyph *glyphs)
{
    test_glyph_u16(&glyphs[1], 1);
    test_glyph_u16(&glyphs[1], 0);
}
static void contour_ends_cut(test_glyph *glyphs)
{
    simple_header(&glyphs[1], four_points, 1);
    glyphs[1].bytes[1] = 100;
}
static void contour_ends_out_of_order(test_glyph *glyphs)
{
    const uint16_t ends[] = {3, 3};
    simple_header(&glyphs[1], ends, 2);
    test_glyph_u16(&glyphs[1], 0);
}
static void instruction_length_cut(test_glyph *glyphs)
{
    simple_header(&glyphs[1], four_points, 1);
}
static void instructions_cut(test_glyph *glyphs)
{
    simple_header(&glyphs[1], four_points, 1);
    test_glyph_u16(&glyphs[1], 100);
}
static void flags_cut(test_glyph *glyphs)
{
    simple_header(&glyphs[1], four_points, 1);
    test_glyph_u16(&glyphs[1], 0);
    test_glyph_u8(&glyphs[1], ON | X_SAME | Y_SAME);
}
static void repeat_cut(test_glyph *glyphs)
{
    simple_header(&glyphs[1], four_points, 1);
    test_glyph_u16(&glyphs[1], 0);
    test_glyph_u8(&glyphs[1], ON | X_SAME | Y_SAME);
    test_glyph_u8(&glyphs[1], ON | REPEAT);
}
static void repeat_past(test_glyph *glyphs)
{
    simple_header(&glyphs[1], four_points, 1);
    test_glyph_u16(&glyphs[1], 0);
    test_glyph_u8(&glyphs[1], ON | REPEAT | X_SAME | Y_SAME);
    test_glyph_u8(&glyphs[1], 4);
}
static void short_coordinate_cut(test_glyph *glyphs)
{
    simple_header(&glyphs[1], four_points, 1);
    test_glyph_u16(&glyphs[1], 0);
    test_glyph_u8(&glyphs[1], ON | REPEAT | X_SHORT);
    test_glyph_u8(&glyphs[1], 3);
    test_glyph_u8(&glyphs[1], 5);
}
static void long_coordinate_cut(test_glyph *glyphs)
{
    /* Each x the same as the last; each y a change of two bytes. */
    simple_header(&glyphs[1], four_points, 1);
    test_glyph_u16(&glyphs[1], 0);
    test_glyph_u8(&glyphs[1], ON | REPEAT | X_SAME);
    test_glyph_u8(&glyphs[1], 3);
    test_glyph_u16(&glyphs[1], 5);
}
/* Glyph 2: 65536 points, every one at the origin. */
static void most_points(test_glyph *glyph)
{
    const uint16_t ends[] = {0xFFFF};
    simple_header(glyph, ends, 1);
    test_glyph_u16(glyph, 0);
    for (size_t run = 0; run < 256; run++)
    {
        test_glyph_u8(glyph, ON | REPEAT | X_SAME | Y_SAME);
        test_glyph_u8(glyph, 255);
    }
}
static void too_many_points(test_glyph *glyphs)
{
    most_points(&glyphs[2]);
    test_compound_header(&glyphs[1]);
    for (size_t copy = 0; copy < 17; copy++)
    {
        test_glyph_u16(&glyphs[1], copy < 16 ? XY | MORE : XY);
        test_glyph_u16(&glyphs[1], 2);
        test_glyph_u16(&glyphs[1], 0);
    }
}
/* A compound of `count` components, all glyph `component`. */
static void many_components(test_glyph *glyph, size_t count, uint16_t component)
{
    test_compound_header(glyph);
    for (size_t copy = 0; copy < count; copy++)
    {
        test_glyph_u16(glyph, copy + 1 < count ? XY | MORE : XY);
        test_glyph_u16(glyph, component);
        test_glyph_u16(glyph, 0);
    }
}
static void too_many_components(test_glyph *glyphs)
{
    many_components(&glyphs[1], 110, 2);
    many_components(&glyphs[2], 110, 3);
    many_components(&glyphs[3], 110, 0);
}
static void compound_cycle(test_glyph *glyphs)
{
    many_components(&glyphs[1], 1, 2);
    many_components(&glyphs[2], 1, 1);
}
static void component_cut(test_glyph *glyphs)
{
    test_compound_header(&glyphs[1]);
    test_glyph_u16(&glyphs[1], XY);
}
static void component_arguments_cut(test_glyph *glyphs)
{
    test_compound_header(&glyphs[1]);
    test_glyph_u16(&glyphs[1], XY | WORDS);
    test_glyph_u16(&glyphs[1], 0);
    test_glyph_u16(&glyphs[1], 0);
}
static void component_glyph_missing(test_glyph *glyphs)
{
    many_components(&glyphs[1], 1, 50);
}
static void square(test_glyph *glyph)
{
    const test_point points[] = {{0, 0, true}, {0, 100, true}, {100, 100, true}, {100, 0, true}};
    test_simple_glyph(glyph, points, 4, four_points, 1);
}
static void match_no_compound_point(test_glyph *glyphs)
{
    square(&glyphs[2]);
    test_compound_header(&glyphs[1]);
    test_glyph_u16(&glyphs[1], 0);
    test_glyph_u16(&glyphs[1], 2);
    test_glyph_u16(&glyphs[1], 0);
}
static void match_no_component_point(test_glyph *glyphs)
{
    square(&glyphs[2]);
    test_compound_header(&glyphs[1]);
    test_glyph_u16(&glyphs[1], XY | MORE);
    test_glyph_u16(&glyphs[1], 2);
    test_glyph_u16(&glyphs[1], 0);
    test_glyph_u16(&glyphs[1], 0);
    test_glyph_u16(&glyphs[1], 2);
    test_glyph_u8(&glyphs[1], 0);
    test_glyph_u8(&glyphs[1], 4);
}
static void damaged_component(test_glyph *glyphs)
{
    many_components(&glyphs[1], 1, 2);
    glyph_header_cut(glyphs + 1);
}

static const outline_case outline_cases[] = {
    {"glyph header cut", glyph_header_cut, TB_FONT_MALFORMED, "the glyph header is cut off"},
    {"contour ends cut", contour_ends_cut, TB_FONT_MALFORMED, "the contour ends are cut off"},
    {"contour ends out of order", contour_ends_out_of_order, TB_FONT_MALFORMED,
     "contour ends are out of order"},
    {"instruction length cut", instruction_length_cut, TB_FONT_MALFORMED,
     "the instructions are cut off"},
    {"instructions cut", instructions_cut, TB_FONT_MALFORMED, "the instructions are cut off"},
    {"flags cut", flags_cut, TB_FONT_MALFORMED, "the point flags are cut off"},
    {"repeat cut", repeat_cut, TB_FONT_MALFORMED, "the point flags are cut off"},
    {"repeat past", repeat_past, TB_FONT_MALFORMED, "the point flags repeat past the last point"},
    {"short coordinate cut", short_coordinate_cut, TB_FONT_MALFORMED,
     "the coordinates are cut off"},
    {"long coordinate cut", long_coordinate_cut, TB_FONT_MALFORMED, "the coordinates are cut off"},
    {"too many points", too_many_points, TB_FONT_TOO_LARGE, "the outline has too many points"},
    {"too many components", too_many_components, TB_FONT_TOO_LARGE,
     "the outline has too many components"},
    {"compound cycle", compound_cycle, TB_FONT_MALFORMED,
     "compound glyphs nest too deeply, or in a cycle"},
    {"component cut", component_cut, TB_FONT_MALFORMED, "a component is cut off"},
    {"component arguments cut", component_arguments_cut, TB_FONT_MALFORMED,
     "a component is cut off"},
    {"component glyph missing", component_glyph_missing, TB_FONT_MALFORMED,
     "a component is a glyph the font does not have"},
    {"match no compound point", match_no_compound_point, TB_FONT_MALFORMED,
     "a component matches a point that does not exist"},
    {"match no component point", match_no_component_point, TB_FONT_MALFORMED,
     "a component matches a point that does not exist"},
    {"damaged component", damaged_component, TB_FONT_MALFORMED, "the glyph header is cut off"},
};

TB_TEST(font_damage, every_check_of_an_outline_fails_with_its_reason)
{
    tb_arena *arena = tb_arena_create(0);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(outline_cases); index++)
    {
        const outline_case *each = &outline_cases[index];
        TB_TEST_CONTEXT("%s", each->name);
        test_font *made = test_font_create();
        test_glyph *glyphs = TB_NEW_ARRAY(test_glyph, 4);
        each->make(glyphs);
        test_font_set_glyphs(made, glyphs, 4);
        tb_free(glyphs);
        tb_font *font = NULL;
        uint8_t *bytes = NULL;
        TB_TEST_REQUIRE_UINT(open_exact(made, &font, NULL, &bytes), ==, TB_FONT_OK);
        /* A failure leaves the arena as it was. */
        const size_t remaining = tb_arena_remaining(arena);
        tb_font_outline outline = {0};
        tb_font_error error = {0};
        TB_TEST_EXPECT_UINT(tb_font_get_outline(font, 1, arena, &outline, &error), ==,
                            each->result);
        TB_TEST_EXPECT_UINT(error.result, ==, each->result);
        TB_TEST_EXPECT_STRING(error.table, ==, "glyf");
        TB_TEST_EXPECT_STRING(error.reason, ==, each->reason);
        TB_TEST_EXPECT_UINT(outline.contour_count, ==, 0);
        TB_TEST_EXPECT_POINTER(outline.contours, ==, NULL);
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, remaining);
        tb_font_close(font);
        tb_free(bytes);
        test_font_destroy(made);
    }
    tb_arena_destroy(arena);
}

/* -------------------------------------------------------------------------
 * The fonts of the font pack, cut short and corrupted.
 * ------------------------------------------------------------------------- */

/* Read everything a font holds, checking only that what it says is whole:
 * the sanitizer build checks that no read leaves the bytes. `stride`
 * thins the glyphs of a large font. */
static void read_everything(const tb_font *font, tb_arena *arena, uint16_t stride)
{
    tb_font_metrics metrics = {0};
    tb_font_get_metrics(font, &metrics);
    tb_font_style style = {0};
    tb_font_get_style(font, &style);
    TB_TEST_EXPECT_UINT(tb_str_length(style.family), ==, style.family_length);
    TB_TEST_EXPECT_UINT(tb_str_length(style.style), ==, style.style_length);
    for (uint32_t code_point = 0; code_point < 0x300; code_point++)
    {
        TB_TEST_EXPECT_UINT(tb_font_find_glyph(font, code_point), <, metrics.glyph_count);
    }
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(font, 0x1F600), <, metrics.glyph_count);
    for (uint16_t left = 0; left < metrics.glyph_count && left < 120; left += 3)
    {
        for (uint16_t right = 0; right < metrics.glyph_count && right < 120; right += 5)
        {
            (void)tb_font_get_kerning(font, left, right);
        }
    }
    for (uint32_t glyph = 0; glyph < metrics.glyph_count; glyph += stride)
    {
        tb_font_glyph_metrics advance = {0};
        tb_font_get_glyph_metrics(font, (uint16_t)glyph, &advance);
        tb_font_outline outline = {0};
        tb_font_error error = {0};
        const tb_font_result result =
            tb_font_get_outline(font, (uint16_t)glyph, arena, &outline, &error);
        if (result != TB_FONT_OK)
        {
            TB_TEST_EXPECT_UINT(tb_str_length(error.reason), >, 0);
            continue;
        }
        for (size_t contour = 0; contour < outline.contour_count; contour++)
        {
            TB_TEST_EXPECT_UINT(outline.contours[contour].segment_count, >, 0);
        }
        tb_arena_reset(arena);
    }
    tb_arena_reset(arena);
}

/* Open the bytes, in memory of exactly their length; read all of a font that opens. */
static void open_damaged(const uint8_t *source, size_t length, tb_arena *arena, uint16_t stride)
{
    uint8_t *bytes = tb_malloc(length == 0 ? 1 : length);
    if (length != 0)
    {
        memcpy(bytes, source, length);
    }
    tb_font *font = NULL;
    tb_font_error error = {0};
    const tb_font_result result = tb_font_open(bytes, length, NULL, &font, &error);
    if (result == TB_FONT_OK)
    {
        read_everything(font, arena, stride);
    }
    else
    {
        TB_TEST_EXPECT_POINTER(font, ==, NULL);
        TB_TEST_EXPECT_UINT(tb_str_length(error.reason), >, 0);
    }
    tb_font_close(font);
    tb_free(bytes);
}

/* Every glyph of a small font; one in seven of a large one. */
static uint16_t stride_of(const tb_test_file *file)
{
    return file->length > 100000 ? 7 : 1;
}

TB_TEST(font_damage, fonts_cut_short_fail_cleanly)
{
    tb_arena *arena = tb_arena_create(0);
    for (size_t index = 0; index < tb_font_files_count; index++)
    {
        const tb_test_file *file = &tb_font_files[index];
        TB_TEST_CONTEXT("%s", file->name);
        /* Every length through the directory, then lengths across the rest. */
        for (size_t length = 0; length < file->length; length += length < 512 ? 1 : 1 + length / 40)
        {
            open_damaged(file->bytes, length, arena, stride_of(file));
        }
    }
    tb_arena_destroy(arena);
}

/* One table of a font, chosen at random: where it begins and how long it is. */
static void random_table(const tb_test_file *file, size_t *offset, size_t *length)
{
    const size_t count = ((size_t)file->bytes[4] << 8) | file->bytes[5];
    const uint8_t *record = file->bytes + 12 + tb_test_random_below(count) * 16;
    *offset = ((size_t)record[8] << 24) | ((size_t)record[9] << 16) | ((size_t)record[10] << 8) |
              record[11];
    *length = ((size_t)record[12] << 24) | ((size_t)record[13] << 16) | ((size_t)record[14] << 8) |
              record[15];
}

/* A byte of damage: all ones, all zeros, or anything, a third of the time each. */
static uint8_t corrupt_byte(void)
{
    switch (tb_test_random_below(3))
    {
    case 0:
        return 0xFF;
    case 1:
        return 0x00;
    default:
        return (uint8_t)tb_test_random_below(256);
    }
}

TB_TEST(font_damage, fonts_corrupted_anywhere_open_or_fail_cleanly)
{
    tb_arena *arena = tb_arena_create(0);
    for (size_t index = 0; index < tb_font_files_count; index++)
    {
        const tb_test_file *file = &tb_font_files[index];
        uint8_t *copy = tb_malloc(file->length);
        memcpy(copy, file->bytes, file->length);
        const size_t rounds = file->length > 100000 ? 400 : 1000;
        for (size_t round = 0; round < rounds; round++)
        {
            /* A table at random, then a place in it: the small tables that
             * everything depends on are as likely to be hit as glyf. A
             * place in the directory, now and then. */
            size_t start = 0;
            size_t length = 12 + 16 * (((size_t)file->bytes[4] << 8) | file->bytes[5]);
            if (round % 8 != 0)
            {
                random_table(file, &start, &length);
            }
            const size_t place = start + (size_t)tb_test_random_below(length == 0 ? 1 : length);
            const size_t width = 1 + (size_t)tb_test_random_below(4);
            TB_TEST_CONTEXT("%s: %zu bytes at %zu", file->name, width, place);
            for (size_t byte = place; byte < place + width && byte < file->length; byte++)
            {
                copy[byte] = corrupt_byte();
            }
            open_damaged(copy, file->length, arena, stride_of(file));
            for (size_t byte = place; byte < place + width && byte < file->length; byte++)
            {
                copy[byte] = file->bytes[byte];
            }
        }
        tb_free(copy);
    }
    tb_arena_destroy(arena);
}
