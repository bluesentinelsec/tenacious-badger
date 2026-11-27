/* Sprite sheets must still address the right pixels after sorting and trimming. */
#include "badger/sprite.h"
#include "tb_test.h"

TB_TEST(media_sprite_atlas, trim_extrudes_the_converted_edges_and_preserves_the_pivot)
{
    tb_sprite_sheet *const sheet = tb_sprite_sheet_create(NULL);
    tb_bitmap *const source = tb_bitmap_create(5, 4, TB_BITMAP_BGRA8, NULL);
    tb_bitmap *const blank = tb_bitmap_create(3, 4, TB_BITMAP_RGBA8, NULL);
    const tb_sprite_atlas_input inputs[] = {{"blank", blank, 0, 0}, {"hero", source, 2, 3}};
    const tb_sprite_atlas_options options = {.max_width = 8,
                                             .max_height = 4,
                                             .padding = 1,
                                             .extrude = 1,
                                             .trim = true,
                                             .power_of_two = true};
    tb_bitmap *atlas = NULL;
    tb_bitmap_info info = {0};
    tb_bitmap_set_pixel(source, 1, 2, (tb_bitmap_color){255, 0, 0, 128});
    tb_bitmap_set_pixel(source, 2, 2, (tb_bitmap_color){0, 200, 0, 255});
    tb_bitmap_premultiply(source);

    TB_TEST_REQUIRE(tb_sprite_atlas_pack(inputs, 2, &options, NULL, sheet, &atlas) == TB_SPRITE_OK);
    tb_bitmap_info_of(atlas, &info);
    TB_TEST_EXPECT_UINT(info.width, ==, 8);
    TB_TEST_EXPECT_UINT(info.height, ==, 4);
    TB_TEST_EXPECT(info.format == TB_BITMAP_RGBA8 && !info.premultiplied);
    const tb_sprite_frame *const empty = tb_sprite_sheet_frame(sheet, 0);
    const tb_sprite_frame *const hero = tb_sprite_sheet_frame(sheet, 1);
    TB_TEST_EXPECT_STRING(empty->name, ==, "blank");
    TB_TEST_EXPECT_UINT(empty->rect.width, ==, 1);
    TB_TEST_EXPECT_UINT(empty->rect.height, ==, 1);
    TB_TEST_EXPECT_UINT(empty->source_height, ==, 4);
    TB_TEST_EXPECT_STRING(hero->name, ==, "hero");
    TB_TEST_EXPECT(hero->offset_x == 1 && hero->offset_y == 2);
    TB_TEST_EXPECT(hero->source_width == 5 && hero->source_height == 4);
    TB_TEST_EXPECT(hero->pivot_x == 2 && hero->pivot_y == 3);
    TB_TEST_EXPECT(hero->rect.width == 2 && hero->rect.height == 1);
    TB_TEST_REQUIRE(hero->rect.x >= 1 && hero->rect.y >= 1);
    const uint32_t x = (uint32_t)hero->rect.x;
    const uint32_t y = (uint32_t)hero->rect.y;
    for (uint32_t row = y - 1u; row <= y + 1u; row++)
    {
        for (uint32_t col = x - 1u; col <= x + 2u; col++)
        {
            const tb_bitmap_color pixel = tb_bitmap_get_pixel(atlas, col, row);
            if (col <= x)
            {
                TB_TEST_EXPECT(pixel.r == 255 && pixel.g == 0 && pixel.a == 128);
            }
            else
            {
                TB_TEST_EXPECT(pixel.r == 0 && pixel.g == 200 && pixel.a == 255);
            }
        }
    }
    const uint32_t gap = empty->rect.x < hero->rect.x ? x - 2u : x + 3u;
    TB_TEST_EXPECT_UINT(tb_bitmap_get_pixel(atlas, gap, y).a, ==, 0);
    tb_bitmap_destroy(atlas);
    tb_bitmap_destroy(blank);
    tb_bitmap_destroy(source);
    tb_sprite_sheet_destroy(sheet);
}

TB_TEST(media_sprite_atlas, different_sizes_tile_a_fixed_bin_without_overlap)
{
    tb_sprite_sheet *const sheet = tb_sprite_sheet_create(NULL);
    tb_bitmap *const large = tb_bitmap_create(3, 3, TB_BITMAP_GRAY8, NULL);
    tb_bitmap *const strip = tb_bitmap_create(2, 3, TB_BITMAP_GRAY8, NULL);
    tb_bitmap *const bottom = tb_bitmap_create(5, 1, TB_BITMAP_GRAY8, NULL);
    const tb_sprite_atlas_input inputs[] = {
        {"bottom", bottom, 0, 0}, {"large", large, 0, 0}, {"strip", strip, 0, 0}};
    const tb_sprite_atlas_options options = {.max_width = 5, .max_height = 4};
    tb_bitmap *const sources[] = {bottom, large, strip};
    tb_bitmap *atlas = NULL;
    for (size_t i = 0; i < TB_TEST_COUNT_OF(inputs); i++)
    {
        const uint8_t channel = (uint8_t)(i + 1);
        tb_bitmap_fill(sources[i], (tb_bitmap_color){channel, channel, channel, 255});
    }
    TB_TEST_REQUIRE(tb_sprite_atlas_pack(inputs, 3, &options, NULL, sheet, &atlas) == TB_SPRITE_OK);
    for (size_t i = 0; i < TB_TEST_COUNT_OF(inputs); i++)
    {
        const tb_sprite_frame *const frame = tb_sprite_sheet_frame(sheet, i);
        for (uint32_t y = 0; y < frame->rect.height; y++)
        {
            for (uint32_t x = 0; x < frame->rect.width; x++)
            {
                const tb_bitmap_color pixel = tb_bitmap_get_pixel(
                    atlas, (uint32_t)frame->rect.x + x, (uint32_t)frame->rect.y + y);
                TB_TEST_EXPECT_UINT(pixel.r, ==, i + 1);
                TB_TEST_EXPECT_UINT(pixel.a, ==, 255);
            }
        }
    }
    tb_bitmap_destroy(atlas);
    tb_bitmap_destroy(large);
    tb_bitmap_destroy(strip);
    tb_bitmap_destroy(bottom);
    tb_sprite_sheet_destroy(sheet);
}

TB_TEST(media_sprite_atlas, refuses_invalid_or_unplaceable_inputs_atomically)
{
    tb_sprite_sheet *const sheet = tb_sprite_sheet_create(NULL);
    tb_bitmap *const source = tb_bitmap_create(3, 1, TB_BITMAP_RGBA8, NULL);
    tb_sprite_atlas_input inputs[] = {{"first", source, 0, 0}, {"first", source, 0, 0}};
    const tb_sprite_frame existing = {.name = "before", .rect = {0, 0, 1, 1}};
    tb_sprite_atlas_options options = {.max_width = 3, .power_of_two = true};
    tb_bitmap *atlas = NULL;
    TB_TEST_REQUIRE(tb_sprite_sheet_add_frame(sheet, &existing, NULL) == TB_SPRITE_OK);
    TB_TEST_EXPECT(tb_sprite_atlas_pack(inputs, 1, &options, NULL, sheet, &atlas) ==
                   TB_SPRITE_TOO_LARGE);
    options = (tb_sprite_atlas_options){.extrude = UINT32_MAX};
    TB_TEST_EXPECT(tb_sprite_atlas_pack(inputs, 1, &options, NULL, sheet, &atlas) ==
                   TB_SPRITE_TOO_LARGE);
    TB_TEST_EXPECT(tb_sprite_atlas_pack(inputs, 2, NULL, NULL, sheet, &atlas) ==
                   TB_SPRITE_NAME_TAKEN);
    inputs[1].name = "before";
    TB_TEST_EXPECT(tb_sprite_atlas_pack(inputs, 2, NULL, NULL, sheet, &atlas) ==
                   TB_SPRITE_NAME_TAKEN);
    inputs[1].bitmap = NULL;
    TB_TEST_EXPECT(tb_sprite_atlas_pack(inputs, 2, NULL, NULL, sheet, &atlas) == TB_SPRITE_INVALID);
    TB_TEST_EXPECT_UINT(tb_sprite_sheet_frame_count(sheet), ==, 1);
    TB_TEST_EXPECT_STRING(tb_sprite_sheet_frame(sheet, 0)->name, ==, "before");
    TB_TEST_EXPECT_POINTER(atlas, ==, NULL);
    tb_bitmap_destroy(source);
    tb_sprite_sheet_destroy(sheet);
}
