/* Sheet descriptions must preserve names, timings and trim offsets on import
 * and export, including failures partway through an append. */
#include "badger/fmt.h"
#include "badger/sprite.h"
#include "badger/str.h"
#include "badger/vfs.h"
#include "tb_test.h"
#include "test_allocator.h"

static tb_sprite_result load(tb_sprite_sheet *sheet, const char *text, tb_sprite_error *error)
{
    return tb_sprite_sheet_load(sheet, text, tb_str_length(text), "hero.json", error);
}

TB_TEST(media_sprite, grids_respect_spacing_and_roll_back_name_conflicts)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_sprite_sheet *const sheet = tb_sprite_sheet_create(&allocator);
    tb_sprite_grid grid = {.name = "cell",
                           .image_width = 9,
                           .image_height = 6,
                           .margin_x = 1,
                           .margin_y = 1,
                           .spacing_x = 1,
                           .spacing_y = 1,
                           .cell_width = 2,
                           .cell_height = 2,
                           .count = 5};
    size_t first = SIZE_MAX, count = 0;
    TB_TEST_REQUIRE_UINT(tb_sprite_sheet_add_grid(sheet, &grid, &first, &count), ==, TB_SPRITE_OK);
    TB_TEST_EXPECT_UINT(first, ==, 0);
    TB_TEST_EXPECT_UINT(count, ==, 5);
    TB_TEST_EXPECT_UINT(tb_sprite_sheet_find_frame(sheet, "cell_4"), ==, 4);
    const tb_sprite_frame *const frame = tb_sprite_sheet_frame(sheet, 4);
    TB_TEST_EXPECT_INT(frame->rect.x, ==, 4);
    TB_TEST_EXPECT_INT(frame->rect.y, ==, 4);
    tb_sprite_frame collision = *frame;
    collision.name = "other_1";
    TB_TEST_REQUIRE_UINT(tb_sprite_sheet_add_frame(sheet, &collision, NULL), ==, TB_SPRITE_OK);
    grid.name = "other";
    TB_TEST_EXPECT_UINT(tb_sprite_sheet_add_grid(sheet, &grid, NULL, NULL), ==,
                        TB_SPRITE_NAME_TAKEN);
    TB_TEST_EXPECT_UINT(tb_sprite_sheet_find_frame(sheet, "other_0"), ==, TB_SPRITE_NONE);
    grid.cell_width = 0;
    TB_TEST_EXPECT_UINT(tb_sprite_sheet_add_grid(sheet, &grid, NULL, NULL), ==, TB_SPRITE_INVALID);
    tb_sprite_sheet_set_image(sheet, "textures/hero.png");
    tb_sprite_sheet_set_image(sheet, tb_sprite_sheet_image(sheet) + 9);
    TB_TEST_EXPECT_STRING(tb_sprite_sheet_image(sheet), ==, "hero.png");
    tb_sprite_sheet_destroy(sheet);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_UINT(tracker.violation_count, ==, 0);
}

TB_TEST(media_sprite, exports_native_frames_and_steps_deterministically)
{
    const char *const text =
        "{\"format\":\"badger-sprites\",\"version\":1,\"image\":\"hero.png\","
        "\"grid\":{\"name\":\"walk\",\"cellWidth\":4,\"cellHeight\":2,\"imageWidth\":12,"
        "\"imageHeight\":2},"
        "\"animations\":[{\"name\":\"walk\",\"loop\":\"ping-pong\",\"steps\":[{\"frame\":\"walk_"
        "0\",\"duration\":10},"
        "{\"frame\":\"walk_1\",\"duration\":20},{\"frame\":\"walk_2\",\"duration\":30}]}]}";
    tb_sprite_sheet *const sheet = tb_sprite_sheet_create(NULL);
    tb_sprite_sheet *const copy = tb_sprite_sheet_create(NULL);
    tb_sprite_error error = {0};
    TB_TEST_REQUIRE_UINT(load(sheet, text, &error), ==, TB_SPRITE_OK);
    const tb_sprite_animation *const anim = tb_sprite_sheet_animation(sheet, 0);
    TB_TEST_EXPECT_UINT(tb_sprite_animation_step_at(anim, 0), ==, 0);
    TB_TEST_EXPECT_UINT(tb_sprite_animation_step_at(anim, 10), ==, 1);
    TB_TEST_EXPECT_UINT(tb_sprite_animation_step_at(anim, 30), ==, 2);
    TB_TEST_EXPECT_UINT(tb_sprite_animation_step_at(anim, 60), ==, 1);
    TB_TEST_EXPECT_UINT(tb_sprite_animation_step_at(anim, 80), ==, 0);
    tb_stream a = {0}, b = {0};
    tb_stream_open_buffer(NULL, &a);
    tb_stream_open_buffer(NULL, &b);
    TB_TEST_REQUIRE_UINT(tb_sprite_sheet_save(sheet, &a, &error), ==, TB_SPRITE_OK);
    size_t len_a = 0, len_b = 0;
    const unsigned char *const bytes_a = tb_stream_buffer_bytes(&a, &len_a);
    TB_TEST_REQUIRE_UINT(tb_sprite_sheet_load(copy, (const char *)bytes_a, len_a, NULL, &error), ==,
                         TB_SPRITE_OK);
    TB_TEST_REQUIRE_UINT(tb_sprite_sheet_save(copy, &b, NULL), ==, TB_SPRITE_OK);
    const unsigned char *const bytes_b = tb_stream_buffer_bytes(&b, &len_b);
    TB_TEST_REQUIRE_UINT(len_a, ==, len_b);
    TB_TEST_EXPECT_BYTES(bytes_a, bytes_b, len_a);
    TB_TEST_EXPECT_UINT(tb_sprite_sheet_find_animation(copy, "walk"), ==, 0);
    TB_TEST_EXPECT_STRING(tb_sprite_sheet_image(copy), ==, "hero.png");
    (void)tb_stream_close(&a);
    (void)tb_stream_close(&b);
    tb_sprite_sheet_destroy(copy);
    tb_sprite_sheet_destroy(sheet);
}

TB_TEST(media_sprite, imports_exporter_frame_arrays_hashes_and_reverse_tags)
{
    const char *const hash =
        "{\"frames\":{\"jump\":{\"frame\":{\"x\":1,\"y\":2,\"w\":3,\"h\":4},"
        "\"sourceSize\":{\"w\":10,\"h\":12},\"spriteSourceSize\":{\"x\":4,\"y\":3,\"w\":3,\"h\":4},"
        "\"pivot\":{\"x\":0.5,\"y\":0.5}}},\"meta\":{\"image\":\"atlas.png\"}}";
    const char *const array =
        "{\"frames\":[{\"filename\":\"one\",\"duration\":20,\"frame\":{\"x\":0,\"y\":0,\"w\":1,"
        "\"h\":1}},"
        "{\"filename\":\"two\",\"duration\":35,\"frame\":{\"x\":1,\"y\":0,\"w\":1,\"h\":1}}],"
        "\"meta\":{\"frameTags\":[{\"name\":\"back\",\"from\":0,\"to\":1,\"direction\":\"reverse\"}"
        "]}}";
    tb_sprite_sheet *const sheet = tb_sprite_sheet_create(NULL);
    TB_TEST_REQUIRE_UINT(load(sheet, hash, NULL), ==, TB_SPRITE_OK);
    const tb_sprite_frame *const frame = tb_sprite_sheet_frame(sheet, 0);
    TB_TEST_EXPECT_FLOAT(frame->pivot_x, ==, 5.0f);
    TB_TEST_EXPECT_FLOAT(frame->pivot_y, ==, 6.0f);
    TB_TEST_EXPECT_INT(frame->offset_x, ==, 4);
    TB_TEST_EXPECT_UINT(frame->source_height, ==, 12);
    TB_TEST_REQUIRE_UINT(load(sheet, array, NULL), ==, TB_SPRITE_OK);
    const tb_sprite_animation *const back = tb_sprite_sheet_animation(sheet, 0);
    TB_TEST_EXPECT_UINT(back->steps[0].frame, ==, 2);
    TB_TEST_EXPECT_UINT(back->steps[0].duration_ms, ==, 35);
    TB_TEST_EXPECT_UINT(back->steps[1].frame, ==, 1);
    TB_TEST_EXPECT_UINT(tb_sprite_animation_step_at(back, 54), ==, 1);
    TB_TEST_EXPECT_UINT(tb_sprite_animation_step_at(back, 55), ==, 0);
    tb_sprite_sheet_destroy(sheet);
}

TB_TEST(media_sprite, malformed_input_rolls_back_and_reports_its_location)
{
    const char *const bad =
        "{\"format\":\"badger-sprites\",\"version\":1,\"image\":\"changed\",\"frames\":[\n"
        "{\"name\":\"first\",\"frame\":{\"x\":0,\"y\":0,\"w\":3,\"h\":2}},\n"
        "{\"name\":\"second\",\"frame\":{}}]}";
    tb_sprite_sheet *const sheet = tb_sprite_sheet_create(NULL);
    tb_sprite_sheet_set_image(sheet, "original");
    tb_sprite_error error = {0};
    TB_TEST_EXPECT_UINT(load(sheet, bad, &error), ==, TB_SPRITE_FILE_INVALID);
    TB_TEST_EXPECT_STRING(error.path, ==, "$.frames[1].frame");
    TB_TEST_EXPECT_UINT(error.line, ==, 3);
    TB_TEST_EXPECT_UINT(error.column, >, 0);
    TB_TEST_EXPECT_UINT(tb_sprite_sheet_frame_count(sheet), ==, 0);
    TB_TEST_EXPECT_STRING(tb_sprite_sheet_image(sheet), ==, "original");
    TB_TEST_EXPECT_UINT(load(sheet, "{", &error), ==, TB_SPRITE_MALFORMED);
    TB_TEST_EXPECT_UINT(load(sheet, "{\"frames\":[{\"rotated\":true}]}", &error), ==,
                        TB_SPRITE_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(load(sheet,
                             "{\"frames\":[{\"filename\":\"a\\u0000b\",\"frame\":{\"x\":0,\"y\":0,"
                             "\"w\":1,\"h\":1}}]}",
                             &error),
                        ==, TB_SPRITE_FILE_INVALID);
    tb_vfs *const vfs = tb_vfs_create(NULL);
    TB_TEST_EXPECT_UINT(tb_sprite_sheet_load_file(sheet, vfs, "missing.json", &error), ==,
                        TB_SPRITE_NOT_FOUND);
    tb_vfs_destroy(vfs);
    tb_sprite_sheet_destroy(sheet);
}

TB_TEST(media_sprite, grid_name_expansion_is_bounded_before_frames_are_added)
{
    char prefix[320] = {0};
    char json[640] = {0};
    for (size_t i = 0; i + 1u < sizeof(prefix); i++)
    {
        prefix[i] = 'x';
    }
    (void)tb_fmt_buffer(json, sizeof(json),
                        "{\"format\":\"badger-sprites\",\"version\":1,\"grid\":{\"name\":\"%s\","
                        "\"cellWidth\":1,\"cellHeight\":1,\"imageWidth\":256,\"imageHeight\":256}}",
                        prefix);
    tb_sprite_sheet *const sheet = tb_sprite_sheet_create(NULL);
    tb_sprite_error error = {0};
    TB_TEST_EXPECT_UINT(load(sheet, json, &error), ==, TB_SPRITE_TOO_LARGE);
    TB_TEST_EXPECT_UINT(tb_sprite_sheet_frame_count(sheet), ==, 0);
    TB_TEST_EXPECT_STRING(error.path, ==, "$.grid");
    tb_sprite_sheet_destroy(sheet);
}
