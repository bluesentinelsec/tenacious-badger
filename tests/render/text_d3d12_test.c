/* Text on D3D12, on WARP, where Windows builds it: a string lands its ink
 * where its layout and glyph measures say, pixel fonts draw the exact
 * pixels the CPU rasterizes, colors multiply coverage, glyphs drawn before
 * the atlas clears partway through a frame are drawn right, and labels sit
 * on their points. On displays of high density: text in logical units is
 * drawn at the display's own pixels, on whole rows, and pixel fonts at
 * whole multiples of their size. */
#include "badger/arena.h"
#include "badger/canvas.h"
#include "badger/font.h"
#include "badger/font_layout.h"
#include "badger/font_raster.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/window.h"

#include "tb_test.h"
#include "test_data.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

extern const tb_test_file tb_font_files[];
extern const size_t tb_font_files_count;

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
    tb_font *roboto;
    tb_font *vga;
} screen;

/* The box of the pixels of a capture that are not black. */
typedef struct ink_box
{
    uint32_t left;
    uint32_t top;
    uint32_t right; /* past the last column */
    uint32_t bottom;
    int brightest;
} ink_box;

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

/* A window and a D3D12 renderer on WARP with a glyph atlas of a size, or a
 * skipped case where there is none. */
static void open_screen(screen *opened, uint32_t atlas)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};

    *opened = (screen){0};
    if (tb_window_system_open(NULL, &opened->system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(
            opened->system,
            &(tb_window_options){.title = "text", .width = 160, .height = 120, .fixed_size = true},
            &opened->window, NULL),
        ==, TB_WINDOW_OK);
    options.window = opened->window;
    options.device = (tb_rhi_device_options){
        .backend = TB_RHI_BACKEND_D3D12, .adapter = TB_RHI_ADAPTER_SOFTWARE, .validation = true};
    options.glyph_atlas_size = atlas;
    if (tb_renderer_create(&options, &opened->renderer, &error) != TB_RHI_OK)
    {
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no renderer: %s", error.message);
    }
    opened->roboto = open_font("Roboto-Regular.ttf");
    opened->vga = open_font("Px437_IBM_VGA_8x16.ttf");
}

static void close_screen(screen *opened)
{
    tb_renderer_destroy(opened->renderer);
    tb_font_close(opened->roboto);
    tb_font_close(opened->vga);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

static void begin(screen *opened)
{
    tb_camera camera = tb_camera_perspective(1.2f);
    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 0, 5}, (tb_vec3){0, 0, 0}));
    TB_TEST_REQUIRE(tb_renderer_begin_frame(opened->renderer, &camera, (tb_vec4){0, 0, 0, 1}));
}

/* End a frame captured, and give its pixels; skip where there is no
 * bindless canvas. */
static const uint8_t *end_captured(screen *opened, uint32_t *width, uint32_t *height)
{
    tb_canvas_stats stats = {0};
    const uint8_t *pixels = NULL;

    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened->renderer), ==, TB_RHI_OK);
    tb_canvas_stats_of(opened->renderer, &stats);
    if (stats.draws == 0)
    {
        close_screen(opened);
        TB_TEST_SKIP("no bindless resources on this device");
    }
    pixels = tb_renderer_captured(opened->renderer, width, height);
    TB_TEST_REQUIRE(pixels != NULL);
    return pixels;
}

/* One channel of a pixel, 0 blue, 1 green, 2 red. */
static int channel_at(const uint8_t *pixels, uint32_t width, uint32_t x, uint32_t y, int channel)
{
    return pixels[((size_t)y * width + x) * 4u + (size_t)channel];
}

static ink_box find_ink(const uint8_t *pixels, uint32_t width, uint32_t height)
{
    ink_box box = {.left = width, .top = height};

    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            const int red = channel_at(pixels, width, x, y, 2);
            if (red == 0)
            {
                continue;
            }
            box.left = x < box.left ? x : box.left;
            box.top = y < box.top ? y : box.top;
            box.right = x + 1 > box.right ? x + 1 : box.right;
            box.bottom = y + 1 > box.bottom ? y + 1 : box.bottom;
            box.brightest = red > box.brightest ? red : box.brightest;
        }
    }
    return box;
}

/* A string at a size lands its ink in the box that its layout and the
 * glyphs' bitmaps, measured on the CPU, say: within the pixel that the
 * atlas's subpixel bins may move a glyph by. Some pixels are fully inked. */
TB_TEST(canvas_text_frame, a_string_lands_its_ink_where_its_layout_says_on_d3d12)
{
    screen opened = {0};
    tb_arena *const arena = tb_arena_create(0);
    tb_font_layout layout = {0};
    const tb_vec2 origin = {12.25f, 30.0f};
    int32_t expected_left = INT32_MAX;
    int32_t expected_top = INT32_MAX;
    int32_t expected_right = INT32_MIN;
    int32_t expected_bottom = INT32_MIN;
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    ink_box ink = {0};

    open_screen(&opened, 0);
    tb_font_layout_text(opened.roboto, TB_TEXT("Hello, AV"), 28.0f, NULL, arena, &layout);
    for (size_t index = 0; index < layout.glyph_count; index++)
    {
        const tb_font_layout_glyph *const glyph = &layout.glyphs[index];
        const float pen = origin.x + glyph->x;
        const int32_t baseline = (int32_t)roundf(origin.y + layout.lines[0].baseline);
        tb_font_glyph_image image = {0};
        const tb_font_raster_options options = {28.0f, pen - floorf(pen), TB_FONT_RASTER_SMOOTH};
        TB_TEST_REQUIRE_UINT(
            tb_font_measure_glyph(opened.roboto, glyph->glyph, &options, arena, &image, NULL), ==,
            TB_FONT_OK);
        if (image.width == 0)
        {
            continue;
        }
        const int32_t left = (int32_t)floorf(pen) + image.left;
        const int32_t top = baseline - image.top;
        expected_left = left < expected_left ? left : expected_left;
        expected_top = top < expected_top ? top : expected_top;
        expected_right = left + (int32_t)image.width > expected_right ? left + (int32_t)image.width
                                                                      : expected_right;
        expected_bottom = top + (int32_t)image.height > expected_bottom
                              ? top + (int32_t)image.height
                              : expected_bottom;
    }
    tb_renderer_capture_next_frame(opened.renderer);
    begin(&opened);
    tb_canvas_draw_text(opened.renderer, opened.roboto, TB_TEXT("Hello, AV"), 28.0f, origin,
                        (tb_vec4){1, 1, 1, 1}, NULL);
    pixels = end_captured(&opened, &width, &height);
    ink = find_ink(pixels, width, height);
    /* A bitmap's box is around its outline's control points too, so ink
     * lies inside it, and reaches within a pixel of each edge. */
    TB_TEST_EXPECT_INT(abs((int32_t)ink.left - expected_left), <=, 1);
    TB_TEST_EXPECT_INT(abs((int32_t)ink.top - expected_top), <=, 1);
    TB_TEST_EXPECT_INT(abs((int32_t)ink.right - expected_right), <=, 1);
    TB_TEST_EXPECT_INT(abs((int32_t)ink.bottom - expected_bottom), <=, 1);
    TB_TEST_EXPECT_INT(ink.brightest, ==, 255);
    tb_arena_destroy(arena);
    close_screen(&opened);
}

/* Check that a pixel font's glyph drawn by the canvas is, pixel for pixel,
 * the CPU's aliased bitmap of it in a color, with nothing around it. */
static void expect_exact_glyph(const uint8_t *pixels, uint32_t width, const tb_font *font,
                               uint32_t code_point, float size, int32_t pen_x, int32_t baseline,
                               const int color[3])
{
    tb_arena *const arena = tb_arena_create(0);
    tb_font_glyph_image image = {0};
    const tb_font_raster_options options = {size, 0.0f, TB_FONT_RASTER_ALIASED};
    size_t wrong = 0;

    TB_TEST_REQUIRE_UINT(tb_font_rasterize_glyph(font, tb_font_find_glyph(font, code_point),
                                                 &options, arena, &image, NULL),
                         ==, TB_FONT_OK);
    TB_TEST_REQUIRE_UINT(image.width, >, 0);
    /* The bitmap and a pixel around it. */
    for (int32_t row = -1; row <= (int32_t)image.height; row++)
    {
        for (int32_t column = -1; column <= (int32_t)image.width; column++)
        {
            const bool inside = row >= 0 && column >= 0 && row < (int32_t)image.height &&
                                column < (int32_t)image.width;
            const bool inked =
                inside && image.pixels[(size_t)row * image.width + (size_t)column] == 255;
            const uint32_t x = (uint32_t)(pen_x + image.left + column);
            const uint32_t y = (uint32_t)(baseline - image.top + row);
            for (int channel = 0; channel < 3; channel++)
            {
                const int expected = inked ? color[channel] : 0;
                wrong += channel_at(pixels, width, x, y, channel) != expected ? 1u : 0u;
            }
        }
    }
    TB_TEST_EXPECT_UINT(wrong, ==, 0);
    tb_arena_destroy(arena);
}

/* A pixel font draws exactly the pixels its aliased bitmap has, at its
 * size and at twice it, in the text's color: coverage times color. */
TB_TEST(canvas_text_frame, pixel_fonts_draw_their_exact_pixels_on_d3d12)
{
    static const int amber[3] = {0, 128, 255};
    static const int white[3] = {255, 255, 255};
    screen opened = {0};
    tb_arena *const arena = tb_arena_create(0);
    const tb_font_layout_options pixel = {.pixel_size = 16.0f};
    tb_font_layout layout = {0};
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t ascent = 0;
    int32_t doubled_ascent = 0;

    open_screen(&opened, 0);
    tb_font_layout_text(opened.vga, TB_TEXT("A"), 16.0f, &pixel, arena, &layout);
    ascent = (int32_t)layout.ascent;
    tb_font_layout_text(opened.vga, TB_TEXT("A"), 32.0f, &pixel, arena, &layout);
    doubled_ascent = (int32_t)layout.ascent;
    tb_renderer_capture_next_frame(opened.renderer);
    begin(&opened);
    /* A fraction of a pixel rounds to the nearest whole one. */
    tb_canvas_draw_text(opened.renderer, opened.vga, TB_TEXT("A"), 16.0f, (tb_vec2){10.4f, 10},
                        (tb_vec4){1, 1, 1, 1}, &pixel);
    tb_canvas_draw_text(opened.renderer, opened.vga, TB_TEXT("A"), 32.0f, (tb_vec2){40, 10},
                        (tb_vec4){1, 128.0f / 255.0f, 0, 1}, &pixel);
    pixels = end_captured(&opened, &width, &height);
    expect_exact_glyph(pixels, width, opened.vga, 'A', 16.0f, 10, 10 + ascent, white);
    expect_exact_glyph(pixels, width, opened.vga, 'A', 32.0f, 40, 10 + doubled_ascent, amber);
    tb_arena_destroy(arena);
    close_screen(&opened);
}

/* A glyph drawn before the atlas clears, in the same frame, is placed again
 * and copied to the texture, and drawn exactly. */
TB_TEST(canvas_text_frame, glyphs_drawn_before_the_atlas_clears_are_drawn_right_on_d3d12)
{
    static const int white[3] = {255, 255, 255};
    screen opened = {0};
    tb_arena *const arena = tb_arena_create(0);
    const tb_font_layout_options pixel = {.pixel_size = 16.0f};
    tb_font_layout layout = {0};
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;

    open_screen(&opened, 128);
    tb_font_layout_text(opened.vga, TB_TEXT("A"), 16.0f, &pixel, arena, &layout);
    /* A large glyph fills most of the atlas, far off the frame. */
    begin(&opened);
    tb_canvas_draw_text(opened.renderer, opened.roboto, TB_TEXT("@"), 100.0f,
                        (tb_vec2){-1000, -1000}, (tb_vec4){1, 1, 1, 1}, NULL);
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
    /* The A fits beside it; the next large glyph clears the atlas. */
    tb_renderer_capture_next_frame(opened.renderer);
    begin(&opened);
    tb_canvas_draw_text(opened.renderer, opened.vga, TB_TEXT("A"), 16.0f, (tb_vec2){20, 20},
                        (tb_vec4){1, 1, 1, 1}, &pixel);
    tb_canvas_draw_text(opened.renderer, opened.roboto, TB_TEXT("@"), 90.0f,
                        (tb_vec2){-1000, -1000}, (tb_vec4){1, 1, 1, 1}, NULL);
    pixels = end_captured(&opened, &width, &height);
    expect_exact_glyph(pixels, width, opened.vga, 'A', 16.0f, 20, 20 + (int32_t)layout.ascent,
                       white);
    tb_arena_destroy(arena);
    close_screen(&opened);
}

/* A label is centered on where its point is drawn: the origin, seen from
 * straight ahead, is the middle of the frame. */
TB_TEST(canvas_text_frame, labels_are_centered_on_their_points_on_d3d12)
{
    screen opened = {0};
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    ink_box ink = {0};

    open_screen(&opened, 0);
    tb_renderer_set_debug_font(opened.renderer, opened.roboto, 12.0f);
    tb_renderer_capture_next_frame(opened.renderer);
    begin(&opened);
    tb_debug_text(opened.renderer, (tb_vec3){0, 0, 0}, TB_TEXT("HH"), (tb_vec4){1, 1, 1, 1});
    pixels = end_captured(&opened, &width, &height);
    ink = find_ink(pixels, width, height);
    TB_TEST_REQUIRE_UINT(ink.right, >, ink.left);
    /* H is as high above the middle as below it, give or take its place
     * in the line's box, and as far left as right. */
    TB_TEST_EXPECT_INT(abs((int32_t)(ink.left + ink.right) - (int32_t)width), <=, 2);
    TB_TEST_EXPECT_INT(abs((int32_t)(ink.top + ink.bottom) - (int32_t)height), <=, 8);
    close_screen(&opened);
}

/* ---------------------------------------------------------------------------
 * Displays of high density.
 * ------------------------------------------------------------------------- */

/* Draw a string in white in a view, in a captured frame of its own, and give
 * a copy of the frame's pixels, which the next capture would replace. The
 * caller frees the copy. */
static uint8_t *draw_captured(screen *opened, const tb_font *font, tb_text text, float size,
                              tb_canvas_view view, tb_vec2 position,
                              const tb_font_layout_options *options, uint32_t *width,
                              uint32_t *height)
{
    const uint8_t *pixels = NULL;
    uint8_t *copy = NULL;
    size_t bytes = 0;

    tb_renderer_capture_next_frame(opened->renderer);
    begin(opened);
    tb_canvas_set_view(opened->renderer, &view);
    tb_canvas_draw_text(opened->renderer, font, text, size, position, (tb_vec4){1, 1, 1, 1},
                        options);
    pixels = end_captured(opened, width, height);
    bytes = (size_t)*width * *height * 4u;
    copy = tb_malloc(bytes);
    memcpy(copy, pixels, bytes);
    return copy;
}

/* How many bytes of two captures of one size differ. */
static size_t count_differences(const uint8_t *first, const uint8_t *second, uint32_t width,
                                uint32_t height)
{
    const size_t bytes = (size_t)width * height * 4u;
    size_t different = 0;

    for (size_t index = 0; index < bytes; index++)
    {
        different += first[index] != second[index] ? 1u : 0u;
    }
    return different;
}

/* Text in logical units at a display scale of 150% or 200% is laid out and
 * rasterized at the display's own pixels: it is, pixel for pixel, the text
 * at 1.5 or 2 times its size in a view of pixels, and not a bitmap of the
 * size at 100% scaled up. Its ink is 1.5 or 2 times as large as at 100%,
 * within the pixel that each edge may round by, and some of it is fully
 * inked, as crisp text is. */
TB_TEST(canvas_text_frame, logical_text_is_rasterized_at_the_displays_pixels_on_d3d12)
{
    static const float scales[] = {1.5f, 2.0f};
    const tb_canvas_view pixels_view = {.scale = 1.0f};
    const tb_vec2 position = {10.0f, 8.0f};
    const tb_text text = TB_TEXT("Hg");
    screen opened = {0};
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t *at_100 = NULL;
    ink_box small = {0};

    open_screen(&opened, 0);
    at_100 = draw_captured(&opened, opened.roboto, text, 16.0f, pixels_view, position, NULL, &width,
                           &height);
    small = find_ink(at_100, width, height);
    TB_TEST_REQUIRE_UINT(small.right, >, small.left);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(scales); index++)
    {
        const float scale = scales[index];
        const tb_canvas_view logical = {.scale = scale};
        const tb_vec2 in_pixels = {position.x * scale, position.y * scale};
        uint8_t *const scaled = draw_captured(&opened, opened.roboto, text, 16.0f, logical,
                                              position, NULL, &width, &height);
        uint8_t *const native = draw_captured(&opened, opened.roboto, text, 16.0f * scale,
                                              pixels_view, in_pixels, NULL, &width, &height);
        const ink_box large = find_ink(scaled, width, height);

        TB_TEST_CONTEXT("at %.0f%%", (double)(scale * 100.0f));
        TB_TEST_EXPECT_UINT(count_differences(scaled, native, width, height), ==, 0);
        TB_TEST_EXPECT_FLOAT_NEAR((float)(large.right - large.left),
                                  (float)(small.right - small.left) * scale, 2.0f);
        TB_TEST_EXPECT_FLOAT_NEAR((float)(large.bottom - large.top),
                                  (float)(small.bottom - small.top) * scale, 2.0f);
        TB_TEST_EXPECT_INT(large.brightest, ==, 255);
        tb_free(scaled);
        tb_free(native);
    }
    tb_free(at_100);
    close_screen(&opened);
}

/* Text placed at a fraction of a pixel at 150% has its baseline on the
 * nearest whole row of the display: it is the same image as the text placed
 * on a whole pixel whose baseline is that row, not one blurred across two
 * rows. */
TB_TEST(canvas_text_frame, text_at_a_fraction_of_a_pixel_is_drawn_on_whole_rows_on_d3d12)
{
    const float scale = 1.5f;
    /* 15 and 15.45 pixels. */
    const tb_vec2 position = {10.0f, 10.3f};
    screen opened = {0};
    tb_arena *arena = NULL;
    tb_font_layout layout = {0};
    uint32_t width = 0;
    uint32_t height = 0;
    float baseline = 0.0f;
    float row = 0.0f;
    uint8_t *fractional = NULL;
    uint8_t *whole = NULL;

    open_screen(&opened, 0);
    arena = tb_arena_create(0);
    tb_font_layout_text(opened.roboto, TB_TEXT("Hg"), 16.0f * scale, NULL, arena, &layout);
    baseline = layout.lines[0].baseline;
    row = roundf(position.y * scale + baseline);
    fractional = draw_captured(&opened, opened.roboto, TB_TEXT("Hg"), 16.0f,
                               (tb_canvas_view){.scale = scale}, position, NULL, &width, &height);
    whole = draw_captured(
        &opened, opened.roboto, TB_TEXT("Hg"), 16.0f * scale, (tb_canvas_view){.scale = 1.0f},
        (tb_vec2){position.x * scale, row - roundf(baseline)}, NULL, &width, &height);
    TB_TEST_EXPECT_UINT(count_differences(fractional, whole, width, height), ==, 0);
    tb_free(fractional);
    tb_free(whole);
    tb_arena_destroy(arena);
    close_screen(&opened);
}

/* The pixel font, at 16 logical pixels to the em on displays at 125%,
 * 150%, and 200%, is drawn at the whole multiple of its design size nearest
 * the size in the display's pixels (20, 24, and 32), halves up and at
 * least once: 16, 32, and 32 pixels. Placed at a fraction of a pixel, it
 * lands on the nearest whole pixel, and is exactly its aliased bitmap; and
 * it measures, in units, what it draws. */
TB_TEST(canvas_text_frame, the_pixel_font_lands_on_whole_multiples_at_any_scale_on_d3d12)
{
    static const struct
    {
        float scale;
        float drawn;
    } cases[] = {{1.25f, 16.0f}, {1.5f, 32.0f}, {2.0f, 32.0f}};
    static const int white[3] = {255, 255, 255};
    const tb_font_layout_options pixel = {.pixel_size = 16.0f};
    const tb_vec2 position = {10.3f, 10.3f};
    screen opened = {0};
    tb_arena *arena = NULL;

    open_screen(&opened, 0);
    arena = tb_arena_create(0);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        const float scale = cases[index].scale;
        const tb_canvas_view view = {.scale = scale};
        tb_font_layout layout = {0};
        tb_vec2 measured = {0};
        const uint8_t *pixels = NULL;
        uint32_t width = 0;
        uint32_t height = 0;

        TB_TEST_CONTEXT("at %.0f%%", (double)(scale * 100.0f));
        tb_font_layout_text(opened.vga, TB_TEXT("A"), cases[index].drawn, &pixel, arena, &layout);
        tb_renderer_capture_next_frame(opened.renderer);
        begin(&opened);
        tb_canvas_set_view(opened.renderer, &view);
        measured = tb_canvas_measure_text(opened.renderer, opened.vga, TB_TEXT("A"), 16.0f, &pixel);
        tb_canvas_draw_text(opened.renderer, opened.vga, TB_TEXT("A"), 16.0f, position,
                            (tb_vec4){1, 1, 1, 1}, &pixel);
        pixels = end_captured(&opened, &width, &height);
        TB_TEST_EXPECT_FLOAT_NEAR(measured.x * scale, layout.width, 0.001f);
        TB_TEST_EXPECT_FLOAT_NEAR(measured.y * scale, layout.height, 0.001f);
        expect_exact_glyph(pixels, width, opened.vga, 'A', cases[index].drawn,
                           (int32_t)roundf(position.x * scale),
                           (int32_t)roundf(position.y * scale + layout.lines[0].baseline), white);
        tb_arena_reset(arena);
    }
    tb_arena_destroy(arena);
    close_screen(&opened);
}
