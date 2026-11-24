/* The glyph atlas: glyphs drawn once and found again, packed without
 * overlapping, the rectangles that changed reported, and the atlas cleared
 * and packed anew when it is full. */
#include "badger/arena.h"
#include "badger/font.h"
#include "badger/font_raster.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "font_builder.h"
#include "tb_test.h"
#include "test_data.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern const tb_test_file tb_font_files[];
extern const size_t tb_font_files_count;

#define FONT_BUFFER ((size_t)65536)

static tb_font *open_font(const char *name)
{
    const tb_test_file *found = NULL;
    for (size_t index = 0; index < tb_font_files_count; index++)
    {
        if (tb_str_equal(tb_font_files[index].name, name))
        {
            found = &tb_font_files[index];
        }
    }
    TB_TEST_REQUIRE_POINTER(found, !=, NULL);
    tb_font *font = NULL;
    TB_TEST_REQUIRE_UINT(tb_font_open(found->bytes, found->length, NULL, &font, NULL), ==,
                         TB_FONT_OK);
    return font;
}

static tb_font_raster_options smooth(float size, float subpixel_x)
{
    const tb_font_raster_options options = {size, subpixel_x, TB_FONT_RASTER_SMOOTH};
    return options;
}

static bool overlap(tb_font_atlas_rect a, tb_font_atlas_rect b)
{
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
           b.y < a.y + a.height;
}

/* A rectangle grown by the padding on every side. */
static tb_font_atlas_rect padded(tb_font_atlas_rect rect, uint32_t padding)
{
    const tb_font_atlas_rect grown = {rect.x - padding, rect.y - padding, rect.width + 2 * padding,
                                      rect.height + 2 * padding};
    return grown;
}

TB_TEST(font_atlas, a_new_atlas_is_empty_and_wholly_changed)
{
    tb_font_atlas *atlas = tb_font_atlas_create(NULL, NULL);
    tb_font_atlas_image image = {0};
    tb_font_atlas_get_image(atlas, &image);
    TB_TEST_EXPECT_UINT(image.width, ==, TB_FONT_ATLAS_DEFAULT_SIDE);
    TB_TEST_EXPECT_UINT(image.height, ==, TB_FONT_ATLAS_DEFAULT_SIDE);
    TB_TEST_EXPECT_UINT(image.stride, ==, TB_FONT_ATLAS_DEFAULT_SIDE);
    TB_TEST_EXPECT_UINT(image.pixels[0], ==, 0);
    TB_TEST_EXPECT_UINT(image.pixels[image.stride * image.height - 1], ==, 0);
    TB_TEST_EXPECT_UINT(tb_font_atlas_generation(atlas), ==, 0);
    TB_TEST_EXPECT_UINT(tb_font_atlas_glyph_count(atlas), ==, 0);
    size_t count = 0;
    const tb_font_atlas_rect *dirty = tb_font_atlas_get_dirty(atlas, &count);
    TB_TEST_REQUIRE_UINT(count, ==, 1);
    TB_TEST_EXPECT_UINT(dirty[0].x, ==, 0);
    TB_TEST_EXPECT_UINT(dirty[0].y, ==, 0);
    TB_TEST_EXPECT_UINT(dirty[0].width, ==, image.width);
    TB_TEST_EXPECT_UINT(dirty[0].height, ==, image.height);
    tb_font_atlas_clear_dirty(atlas);
    TB_TEST_EXPECT_POINTER(tb_font_atlas_get_dirty(atlas, &count), ==, NULL);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    tb_font_atlas_destroy(atlas);
    tb_font_atlas_destroy(NULL);
}

TB_TEST(font_atlas, draws_a_glyph_once_and_finds_it_after)
{
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    const tb_font_atlas_options options = {256, 128, 2, 1};
    tb_font_atlas *atlas = tb_font_atlas_create(&options, NULL);
    tb_font_atlas_clear_dirty(atlas);
    const uint16_t glyph = tb_font_find_glyph(roboto, 'g');
    const tb_font_raster_options size = smooth(30.0f, 0.0f);
    tb_font_atlas_glyph first = {0};
    tb_font_error error = {0};
    TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, roboto, glyph, &size, &first, &error), ==,
                         TB_FONT_OK);
    TB_TEST_EXPECT_STRING(error.reason, ==, "");
    TB_TEST_EXPECT_UINT(tb_font_atlas_glyph_count(atlas), ==, 1);

    /* It is the glyph as tb_font_rasterize_glyph() draws it, padded by 2
     * empty pixels. */
    tb_arena *arena = tb_arena_create(0);
    tb_font_glyph_image drawn = {0};
    TB_TEST_REQUIRE_UINT(tb_font_rasterize_glyph(roboto, glyph, &size, arena, &drawn, NULL), ==,
                         TB_FONT_OK);
    TB_TEST_EXPECT_UINT(first.rect.width, ==, drawn.width);
    TB_TEST_EXPECT_UINT(first.rect.height, ==, drawn.height);
    TB_TEST_EXPECT_INT(first.left, ==, drawn.left);
    TB_TEST_EXPECT_INT(first.top, ==, drawn.top);
    TB_TEST_EXPECT_FLOAT(first.advance, ==, drawn.advance);
    TB_TEST_EXPECT_UINT(first.rect.x, >=, 2);
    TB_TEST_EXPECT_UINT(first.rect.y, >=, 2);
    tb_font_atlas_image image = {0};
    tb_font_atlas_get_image(atlas, &image);
    for (uint32_t row = 0; row < drawn.height; row++)
    {
        TB_TEST_CONTEXT("row %u", (unsigned)row);
        TB_TEST_EXPECT_BYTES(image.pixels + (first.rect.y + row) * image.stride + first.rect.x,
                             drawn.pixels + (size_t)row * drawn.width, drawn.width);
    }
    const tb_font_atlas_rect around = padded(first.rect, 2);
    for (uint32_t y = around.y; y < around.y + around.height; y++)
    {
        for (uint32_t x = around.x; x < around.x + around.width; x++)
        {
            const bool inside = x >= first.rect.x && x < first.rect.x + first.rect.width &&
                                y >= first.rect.y && y < first.rect.y + first.rect.height;
            if (!inside)
            {
                TB_TEST_EXPECT_UINT(image.pixels[(size_t)y * image.stride + x], ==, 0);
            }
        }
    }

    /* What changed is the glyph's rectangle. */
    size_t count = 0;
    const tb_font_atlas_rect *dirty = tb_font_atlas_get_dirty(atlas, &count);
    TB_TEST_REQUIRE_UINT(count, ==, 1);
    TB_TEST_EXPECT_UINT(dirty[0].x, ==, first.rect.x);
    TB_TEST_EXPECT_UINT(dirty[0].y, ==, first.rect.y);
    TB_TEST_EXPECT_UINT(dirty[0].width, ==, first.rect.width);
    TB_TEST_EXPECT_UINT(dirty[0].height, ==, first.rect.height);
    tb_font_atlas_clear_dirty(atlas);

    /* Asked for again, it is found: nothing is drawn or changed. */
    tb_font_atlas_glyph again = {0};
    TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, roboto, glyph, &size, &again, NULL), ==,
                         TB_FONT_OK);
    TB_TEST_EXPECT_UINT(again.rect.x, ==, first.rect.x);
    TB_TEST_EXPECT_UINT(again.rect.y, ==, first.rect.y);
    TB_TEST_EXPECT_UINT(tb_font_atlas_glyph_count(atlas), ==, 1);
    TB_TEST_EXPECT_POINTER(tb_font_atlas_get_dirty(atlas, &count), ==, NULL);

    /* Another size, or the aliased mode, is another glyph. */
    const tb_font_raster_options other_size = smooth(31.0f, 0.0f);
    const tb_font_raster_options aliased = {30.0f, 0.0f, TB_FONT_RASTER_ALIASED};
    tb_font_atlas_glyph other = {0};
    TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, roboto, glyph, &other_size, &other, NULL),
                         ==, TB_FONT_OK);
    TB_TEST_EXPECT(!overlap(other.rect, first.rect));
    TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, roboto, glyph, &aliased, &other, NULL), ==,
                         TB_FONT_OK);
    TB_TEST_EXPECT_UINT(tb_font_atlas_glyph_count(atlas), ==, 3);
    TB_TEST_EXPECT(tb_font_atlas_get_dirty(atlas, &count) != NULL);
    TB_TEST_EXPECT_UINT(count, ==, 2);
    tb_arena_destroy(arena);
    tb_font_atlas_destroy(atlas);
    tb_font_close(roboto);
}

TB_TEST(font_atlas, rounds_the_pen_to_the_nearest_subpixel_bin)
{
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    const tb_font_atlas_options options = {256, 256, 1, 4};
    tb_font_atlas *atlas = tb_font_atlas_create(&options, NULL);
    const uint16_t glyph = tb_font_find_glyph(roboto, 'o');
    tb_font_atlas_glyph at_zero = {0};
    const tb_font_raster_options zero = smooth(20.0f, 0.0f);
    TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, roboto, glyph, &zero, &at_zero, NULL), ==,
                         TB_FONT_OK);
    /* 0.1 rounds to bin 0, the same glyph. */
    tb_font_atlas_glyph placed = {0};
    const tb_font_raster_options near_zero = smooth(20.0f, 0.1f);
    TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, roboto, glyph, &near_zero, &placed, NULL),
                         ==, TB_FONT_OK);
    TB_TEST_EXPECT_UINT(placed.rect.x, ==, at_zero.rect.x);
    TB_TEST_EXPECT_INT(placed.left, ==, at_zero.left);
    TB_TEST_EXPECT_UINT(tb_font_atlas_glyph_count(atlas), ==, 1);
    /* 0.3 rounds to bin 1, a quarter of a pixel: another glyph. */
    const tb_font_raster_options quarter = smooth(20.0f, 0.3f);
    TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, roboto, glyph, &quarter, &placed, NULL), ==,
                         TB_FONT_OK);
    TB_TEST_EXPECT_UINT(tb_font_atlas_glyph_count(atlas), ==, 2);
    TB_TEST_EXPECT(placed.rect.x != at_zero.rect.x || placed.rect.y != at_zero.rect.y);
    /* 0.9 rounds up to the next whole pixel: bin 0, a pixel to the right. */
    const tb_font_raster_options next_pixel = smooth(20.0f, 0.9f);
    TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, roboto, glyph, &next_pixel, &placed, NULL),
                         ==, TB_FONT_OK);
    TB_TEST_EXPECT_UINT(tb_font_atlas_glyph_count(atlas), ==, 2);
    TB_TEST_EXPECT_UINT(placed.rect.x, ==, at_zero.rect.x);
    TB_TEST_EXPECT_UINT(placed.rect.y, ==, at_zero.rect.y);
    TB_TEST_EXPECT_INT(placed.left, ==, at_zero.left + 1);
    TB_TEST_EXPECT_INT(placed.top, ==, at_zero.top);
    tb_font_atlas_destroy(atlas);
    tb_font_close(roboto);
}

TB_TEST(font_atlas, a_blank_glyph_takes_no_room)
{
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    tb_font_atlas *atlas = tb_font_atlas_create(NULL, NULL);
    tb_font_atlas_clear_dirty(atlas);
    const tb_font_raster_options size = smooth(16.0f, 0.0f);
    tb_font_atlas_glyph space = {0};
    TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, roboto, tb_font_find_glyph(roboto, ' '),
                                                 &size, &space, NULL),
                         ==, TB_FONT_OK);
    TB_TEST_EXPECT_UINT(space.rect.width, ==, 0);
    TB_TEST_EXPECT_UINT(space.rect.height, ==, 0);
    TB_TEST_EXPECT_FLOAT(space.advance, >, 0.0f);
    TB_TEST_EXPECT_UINT(tb_font_atlas_glyph_count(atlas), ==, 1);
    size_t count = 0;
    TB_TEST_EXPECT_POINTER(tb_font_atlas_get_dirty(atlas, &count), ==, NULL);
    tb_font_atlas_destroy(atlas);
    tb_font_close(roboto);
}

TB_TEST(font_atlas, packed_glyphs_never_overlap_with_their_padding)
{
    /* Glyphs of random sizes, characters, and pen fractions, until the atlas
     * has been cleared a few times: within each generation, no two glyphs'
     * padded rectangles overlap, and each lies within the image. */
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    tb_font *silkscreen = open_font("Silkscreen-Regular.ttf");
    const tb_font_atlas_options options = {512, 256, 1, 4};
    tb_font_atlas *atlas = tb_font_atlas_create(&options, NULL);
    enum
    {
        CAPACITY = 4096
    };
    static tb_font_atlas_rect placed[CAPACITY];
    size_t placed_count = 0;
    uint64_t generation = tb_font_atlas_generation(atlas);
    size_t requests = 0;
    while (tb_font_atlas_generation(atlas) < 4 && requests < 20000)
    {
        requests++;
        tb_font *font = tb_test_random_below(4) == 0 ? silkscreen : roboto;
        const uint32_t code_point = (uint32_t)tb_test_random_between('!', '~');
        const float size = (float)tb_test_random_between(6, 72);
        const float fraction = (float)tb_test_random_below(100) / 100.0f;
        const tb_font_raster_options raster = smooth(size, fraction);
        tb_font_atlas_glyph glyph = {0};
        TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, font,
                                                     tb_font_find_glyph(font, code_point), &raster,
                                                     &glyph, NULL),
                             ==, TB_FONT_OK);
        if (tb_font_atlas_generation(atlas) != generation)
        {
            generation = tb_font_atlas_generation(atlas);
            placed_count = 0;
        }
        if (glyph.rect.width == 0)
        {
            continue;
        }
        const tb_font_atlas_rect grown = padded(glyph.rect, 1);
        TB_TEST_REQUIRE_UINT((uint64_t)grown.x + grown.width, <=, 512);
        TB_TEST_REQUIRE_UINT((uint64_t)grown.y + grown.height, <=, 256);
        bool known = false;
        for (size_t index = 0; index < placed_count; index++)
        {
            const bool same = placed[index].x == glyph.rect.x && placed[index].y == glyph.rect.y;
            known = known || same;
            if (!same)
            {
                TB_TEST_CONTEXT("request %u", (unsigned)requests);
                TB_TEST_REQUIRE(!overlap(padded(placed[index], 1), grown));
            }
        }
        if (!known)
        {
            TB_TEST_REQUIRE_UINT(placed_count, <, CAPACITY);
            placed[placed_count] = glyph.rect;
            placed_count++;
        }
    }
    TB_TEST_EXPECT_UINT(tb_font_atlas_generation(atlas), >=, 4);
    tb_font_atlas_destroy(atlas);
    tb_font_close(silkscreen);
    tb_font_close(roboto);
}

TB_TEST(font_atlas, a_full_atlas_clears_and_counts_its_generation_up)
{
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    const tb_font_atlas_options options = {64, 64, 1, 1};
    tb_font_atlas *atlas = tb_font_atlas_create(&options, NULL);
    const tb_font_raster_options size = smooth(24.0f, 0.0f);
    const uint16_t first_glyph = tb_font_find_glyph(roboto, 'A');
    tb_font_atlas_glyph first = {0};
    TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, roboto, first_glyph, &size, &first, NULL),
                         ==, TB_FONT_OK);
    tb_font_atlas_clear_dirty(atlas);
    /* Letters of 24 pixels fill 64 by 64 within a dozen. */
    size_t held = 1;
    uint32_t code_point = 'B';
    while (tb_font_atlas_generation(atlas) == 0)
    {
        TB_TEST_REQUIRE_UINT(code_point, <=, 'Z');
        tb_font_atlas_glyph glyph = {0};
        TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, roboto,
                                                     tb_font_find_glyph(roboto, code_point), &size,
                                                     &glyph, NULL),
                             ==, TB_FONT_OK);
        code_point++;
        held++;
    }
    TB_TEST_EXPECT_UINT(held, >, 2);
    /* The glyph that did not fit is the only one now, and the whole image
     * changed: the GPU's copy must be replaced. */
    TB_TEST_EXPECT_UINT(tb_font_atlas_generation(atlas), ==, 1);
    TB_TEST_EXPECT_UINT(tb_font_atlas_glyph_count(atlas), ==, 1);
    size_t count = 0;
    const tb_font_atlas_rect *dirty = tb_font_atlas_get_dirty(atlas, &count);
    TB_TEST_REQUIRE_UINT(count, ==, 1);
    TB_TEST_EXPECT_UINT(dirty[0].width, ==, 64);
    TB_TEST_EXPECT_UINT(dirty[0].height, ==, 64);
    /* The first glyph is gone, and is drawn again when asked for. */
    tb_font_atlas_glyph again = {0};
    TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, roboto, first_glyph, &size, &again, NULL),
                         ==, TB_FONT_OK);
    TB_TEST_EXPECT_UINT(tb_font_atlas_glyph_count(atlas), ==, 2);
    /* Resetting forgets everything and clears the image. */
    tb_font_atlas_reset(atlas);
    TB_TEST_EXPECT_UINT(tb_font_atlas_generation(atlas), ==, 2);
    TB_TEST_EXPECT_UINT(tb_font_atlas_glyph_count(atlas), ==, 0);
    tb_font_atlas_image image = {0};
    tb_font_atlas_get_image(atlas, &image);
    size_t lit = 0;
    for (size_t index = 0; index < image.stride * image.height; index++)
    {
        lit += image.pixels[index] != 0 ? 1u : 0u;
    }
    TB_TEST_EXPECT_UINT(lit, ==, 0);
    tb_font_atlas_destroy(atlas);
    tb_font_close(roboto);
}

TB_TEST(font_atlas, a_glyph_larger_than_the_atlas_fails_and_changes_nothing)
{
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    const tb_font_atlas_options options = {32, 32, 1, 1};
    tb_font_atlas *atlas = tb_font_atlas_create(&options, NULL);
    const tb_font_raster_options small = smooth(12.0f, 0.0f);
    tb_font_atlas_glyph glyph = {0};
    TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, roboto, tb_font_find_glyph(roboto, 'a'),
                                                 &small, &glyph, NULL),
                         ==, TB_FONT_OK);
    tb_font_atlas_clear_dirty(atlas);
    /* At 48 pixels an M is wider than 30, and with its padding than 32. */
    const tb_font_raster_options large = smooth(48.0f, 0.0f);
    tb_font_error error = {0};
    TB_TEST_EXPECT_UINT(tb_font_atlas_get_glyph(atlas, roboto, tb_font_find_glyph(roboto, 'M'),
                                                &large, &glyph, &error),
                        ==, TB_FONT_TOO_LARGE);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the glyph is larger than the atlas");
    TB_TEST_EXPECT_UINT(glyph.rect.width, ==, 0);
    TB_TEST_EXPECT_UINT(tb_font_atlas_generation(atlas), ==, 0);
    TB_TEST_EXPECT_UINT(tb_font_atlas_glyph_count(atlas), ==, 1);
    size_t count = 0;
    TB_TEST_EXPECT_POINTER(tb_font_atlas_get_dirty(atlas, &count), ==, NULL);
    tb_font_atlas_destroy(atlas);
    tb_font_close(roboto);
}

TB_TEST(font_atlas, many_changes_merge_into_one_rectangle_around_them)
{
    tb_font *silkscreen = open_font("Silkscreen-Regular.ttf");
    const tb_font_atlas_options options = {1024, 1024, 1, 1};
    tb_font_atlas *atlas = tb_font_atlas_create(&options, NULL);
    tb_font_atlas_clear_dirty(atlas);
    const tb_font_raster_options size = {8.0f, 0.0f, TB_FONT_RASTER_ALIASED};
    tb_font_atlas_rect around = {0};
    size_t drawn = 0;
    for (uint32_t code_point = '!'; code_point <= '~'; code_point++)
    {
        tb_font_atlas_glyph glyph = {0};
        TB_TEST_REQUIRE_UINT(tb_font_atlas_get_glyph(atlas, silkscreen,
                                                     tb_font_find_glyph(silkscreen, code_point),
                                                     &size, &glyph, NULL),
                             ==, TB_FONT_OK);
        if (glyph.rect.width == 0)
        {
            continue;
        }
        if (drawn == 0)
        {
            around = glyph.rect;
        }
        const uint32_t right = glyph.rect.x + glyph.rect.width > around.x + around.width
                                   ? glyph.rect.x + glyph.rect.width
                                   : around.x + around.width;
        const uint32_t bottom = glyph.rect.y + glyph.rect.height > around.y + around.height
                                    ? glyph.rect.y + glyph.rect.height
                                    : around.y + around.height;
        around.x = glyph.rect.x < around.x ? glyph.rect.x : around.x;
        around.y = glyph.rect.y < around.y ? glyph.rect.y : around.y;
        around.width = right - around.x;
        around.height = bottom - around.y;
        drawn++;
        size_t count = 0;
        const tb_font_atlas_rect *dirty = tb_font_atlas_get_dirty(atlas, &count);
        if (drawn <= TB_FONT_ATLAS_MAX_DIRTY_RECTS)
        {
            TB_TEST_EXPECT_UINT(count, ==, drawn);
        }
        else
        {
            /* Merged, then added to again. */
            TB_TEST_EXPECT_UINT(count, <=, drawn - TB_FONT_ATLAS_MAX_DIRTY_RECTS);
        }
        TB_TEST_REQUIRE_POINTER(dirty, !=, NULL);
    }
    TB_TEST_REQUIRE_UINT(drawn, >, TB_FONT_ATLAS_MAX_DIRTY_RECTS);
    /* Every glyph's rectangle is within one of those listed. */
    size_t count = 0;
    const tb_font_atlas_rect *dirty = tb_font_atlas_get_dirty(atlas, &count);
    TB_TEST_EXPECT_UINT(dirty[0].x, <=, around.x);
    TB_TEST_EXPECT_UINT(dirty[0].y, <=, around.y);
    tb_font_atlas_destroy(atlas);
    tb_font_close(silkscreen);
}

TB_TEST(font_atlas, a_glyph_that_cannot_be_drawn_is_not_kept)
{
    /* Glyph 1 spans 32000 units of 1000 to the em, too large to draw at
     * 2000 pixels; glyph 2's contours end out of order. */
    test_font *made = test_font_create();
    test_glyph glyphs[3] = {0};
    const test_point points[] = {
        {-16000, -16000, true}, {-16000, 16000, true}, {16000, 16000, true}, {16000, -16000, true}};
    const uint16_t ends[] = {3};
    test_simple_glyph(&glyphs[1], points, 4, ends, 1);
    const uint16_t bad_ends[] = {3, 1};
    test_simple_glyph(&glyphs[2], points, 4, bad_ends, 2);
    test_font_set_glyphs(made, glyphs, 3);
    uint8_t *bytes = tb_malloc(FONT_BUFFER);
    const size_t length = test_font_write(made, bytes, FONT_BUFFER);
    tb_font *font = NULL;
    TB_TEST_REQUIRE_UINT(tb_font_open(bytes, length, NULL, &font, NULL), ==, TB_FONT_OK);
    tb_font_atlas *atlas = tb_font_atlas_create(NULL, NULL);
    tb_font_atlas_clear_dirty(atlas);
    tb_font_atlas_glyph glyph = {0};
    tb_font_error error = {0};
    const tb_font_raster_options huge = smooth(2000.0f, 0.0f);
    TB_TEST_EXPECT_UINT(tb_font_atlas_get_glyph(atlas, font, 1, &huge, &glyph, &error), ==,
                        TB_FONT_TOO_LARGE);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the glyph's bitmap is larger than the limit");
    const tb_font_raster_options small = smooth(12.0f, 0.0f);
    TB_TEST_EXPECT_UINT(tb_font_atlas_get_glyph(atlas, font, 2, &small, &glyph, &error), ==,
                        TB_FONT_MALFORMED);
    TB_TEST_EXPECT_STRING(error.table, ==, "glyf");
    TB_TEST_EXPECT_UINT(tb_font_atlas_glyph_count(atlas), ==, 0);
    size_t count = 0;
    TB_TEST_EXPECT_POINTER(tb_font_atlas_get_dirty(atlas, &count), ==, NULL);
    tb_font_atlas_destroy(atlas);
    tb_font_close(font);
    tb_free(bytes);
    test_font_destroy(made);
}
