/* The canvas: views fit a frame and map points both ways; pixels made in
 * memory are what they say; on the null backend, a frame's quads are drawn
 * in one draw and counted; and on D3D12 (WARP, on Windows) shapes, images
 * and sprites land on the pixels they should, layers in order, blended and
 * anti-aliased. */
#include "badger/assert.h"
#include "badger/canvas.h"
#include "badger/memory.h"
#include "badger/window.h"

#include "tb_test.h"

#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Views.
 * ------------------------------------------------------------------------- */

/* A game's resolution fits a frame by whole pixels, in its middle, clipped
 * to what it shows; or by any scale, when pixels need not stay whole. */
TB_TEST(canvas_view, a_view_fits_a_resolution_in_the_middle_of_a_frame)
{
    tb_canvas_view view = tb_canvas_view_fit(100, 100, 30, 20, true);

    TB_TEST_EXPECT_FLOAT(view.scale, ==, 3.0f);
    TB_TEST_EXPECT_FLOAT(view.origin.x, ==, 5.0f);
    TB_TEST_EXPECT_FLOAT(view.origin.y, ==, 20.0f);
    TB_TEST_EXPECT_INT(view.clip.x, ==, 5);
    TB_TEST_EXPECT_INT(view.clip.y, ==, 20);
    TB_TEST_EXPECT_UINT(view.clip.width, ==, 90);
    TB_TEST_EXPECT_UINT(view.clip.height, ==, 60);

    view = tb_canvas_view_fit(100, 50, 10, 10, false);
    TB_TEST_EXPECT_FLOAT(view.scale, ==, 5.0f);
    TB_TEST_EXPECT_FLOAT(view.origin.x, ==, 25.0f);
    TB_TEST_EXPECT_FLOAT(view.origin.y, ==, 0.0f);

    /* Whole pixels round an odd margin down, so the view starts on a pixel. */
    view = tb_canvas_view_fit(101, 64, 32, 32, true);
    TB_TEST_EXPECT_FLOAT(view.scale, ==, 2.0f);
    TB_TEST_EXPECT_FLOAT(view.origin.x, ==, 18.0f);

    /* A frame smaller than the resolution still shows it at one pixel a
     * unit, overflowing the frame; a frame of no size, at scale 1. */
    view = tb_canvas_view_fit(10, 10, 20, 20, true);
    TB_TEST_EXPECT_FLOAT(view.scale, ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(view.origin.x, ==, -5.0f);
    view = tb_canvas_view_fit(0, 0, 20, 20, false);
    TB_TEST_EXPECT_FLOAT(view.scale, ==, 1.0f);
}

/* A point goes to its pixel and back: how a mouse finds what it points at. */
TB_TEST(canvas_view, points_map_to_pixels_and_back)
{
    const tb_canvas_view view = {.origin = {10, -4}, .scale = 2.5f};
    const tb_vec2 pixel = tb_canvas_view_to_pixels(&view, (tb_vec2){4, 8});
    const tb_vec2 point = tb_canvas_view_from_pixels(&view, pixel);
    const tb_canvas_view pixels = tb_canvas_view_pixels();

    TB_TEST_EXPECT_FLOAT(pixel.x, ==, 20.0f);
    TB_TEST_EXPECT_FLOAT(pixel.y, ==, 16.0f);
    TB_TEST_EXPECT_FLOAT(point.x, ==, 4.0f);
    TB_TEST_EXPECT_FLOAT(point.y, ==, 8.0f);
    TB_TEST_EXPECT_FLOAT(pixels.scale, ==, 1.0f);
    TB_TEST_EXPECT_UINT(pixels.clip.width, ==, 0);
}

/* ---------------------------------------------------------------------------
 * Pixels made in memory.
 * ------------------------------------------------------------------------- */

static uint32_t rgba_at(const uint8_t *pixels, uint32_t width, uint32_t x, uint32_t y)
{
    const uint8_t *const rgba = &pixels[((size_t)y * width + x) * 4u];
    return (uint32_t)rgba[0] << 24 | (uint32_t)rgba[1] << 16 | (uint32_t)rgba[2] << 8 |
           (uint32_t)rgba[3];
}

/* Squares of `cell` pixels alternate, the first color at the top left. */
TB_TEST(canvas_image, a_checkerboard_alternates_squares)
{
    uint8_t pixels[8 * 4 * 4] = {0};

    tb_image_fill_checkerboard(pixels, sizeof(pixels), 8, 4, 2, (tb_vec4){1, 0, 0, 1},
                               (tb_vec4){0, 0, 1, 0.5f});
    TB_TEST_EXPECT_HEX(rgba_at(pixels, 8, 0, 0), ==, 0xFF0000FFu);
    TB_TEST_EXPECT_HEX(rgba_at(pixels, 8, 1, 1), ==, 0xFF0000FFu);
    TB_TEST_EXPECT_HEX(rgba_at(pixels, 8, 2, 0), ==, 0x0000FF80u);
    TB_TEST_EXPECT_HEX(rgba_at(pixels, 8, 0, 2), ==, 0x0000FF80u);
    TB_TEST_EXPECT_HEX(rgba_at(pixels, 8, 3, 3), ==, 0xFF0000FFu);
    TB_TEST_EXPECT_HEX(rgba_at(pixels, 8, 7, 3), ==, 0xFF0000FFu);
}

/* Each pixel is the gradient's color at its middle: never quite either end
 * at the edges, and evenly spaced between. Colors outside 0 to 1 clamp. */
TB_TEST(canvas_image, gradients_run_across_down_and_out)
{
    uint8_t pixels[4 * 4 * 4] = {0};

    tb_image_fill_gradient(pixels, sizeof(pixels), 4, 4, TB_IMAGE_GRADIENT_HORIZONTAL,
                           (tb_vec4){0, 0, 0, 1}, (tb_vec4){2, 1, 0, 1});
    /* Red runs 0 to 2, clamped at 1: 0.25 and 0.75 of the way are 0.5 and 1.5. */
    TB_TEST_EXPECT_UINT(pixels[0], ==, 64);
    TB_TEST_EXPECT_UINT(pixels[1], ==, 32);
    TB_TEST_EXPECT_UINT(pixels[3 * 4], ==, 255);
    TB_TEST_EXPECT_UINT(pixels[3 * 4 + 1], ==, 223);
    TB_TEST_EXPECT_UINT(pixels[12 * 4 + 1], ==, 32);

    tb_image_fill_gradient(pixels, sizeof(pixels), 4, 4, TB_IMAGE_GRADIENT_VERTICAL,
                           (tb_vec4){0, 0, 0, 0}, (tb_vec4){1, 1, 1, 1});
    TB_TEST_EXPECT_UINT(pixels[3], ==, 32);
    TB_TEST_EXPECT_UINT(pixels[3 * 4 + 3], ==, 32);
    TB_TEST_EXPECT_UINT(pixels[12 * 4 + 3], ==, 223);

    tb_image_fill_gradient(pixels, sizeof(pixels), 4, 4, TB_IMAGE_GRADIENT_RADIAL,
                           (tb_vec4){1, 1, 1, 1}, (tb_vec4){0, 0, 0, 1});
    /* The four middle pixels are as near the middle as each other, and
     * nearer than the corners. */
    TB_TEST_EXPECT_UINT(pixels[5 * 4], ==, pixels[10 * 4]);
    TB_TEST_EXPECT_UINT(pixels[5 * 4], >, pixels[0]);
    TB_TEST_EXPECT_UINT(pixels[0], ==, 0);
}

/* Noise is opaque gray, the same for the same seed, different for another,
 * and smooth: neighbors within a cell differ little. */
TB_TEST(canvas_image, noise_is_smooth_and_repeatable)
{
    enum
    {
        side = 32
    };
    uint8_t *const first = tb_calloc(side * side, 4);
    uint8_t *const second = tb_calloc(side * side, 4);
    size_t differing = 0;
    int largest_step = 0;

    tb_image_fill_noise(first, side * side * 4, side, side, 8, 42);
    tb_image_fill_noise(second, side * side * 4, side, side, 8, 42);
    TB_TEST_EXPECT_BYTES(first, second, side * side * 4);
    tb_image_fill_noise(second, side * side * 4, side, side, 8, 43);
    for (size_t index = 0; index < side * side; index++)
    {
        TB_TEST_REQUIRE_UINT(first[index * 4 + 3], ==, 255);
        TB_TEST_REQUIRE_UINT(first[index * 4], ==, first[index * 4 + 1]);
        differing += first[index * 4] != second[index * 4] ? 1u : 0u;
        if (index % side != 0)
        {
            const int step = abs((int)first[index * 4] - (int)first[(index - 1) * 4]);
            largest_step = step > largest_step ? step : largest_step;
        }
    }
    TB_TEST_EXPECT_UINT(differing, >, side * side / 2);
    /* A cell of 8 pixels moves at most 255 * 1.5 / 8 a pixel, smoothstep's
     * steepest. */
    TB_TEST_EXPECT_INT(largest_step, <=, 48);
    tb_free(first);
    tb_free(second);
}

/* ---------------------------------------------------------------------------
 * Frames.
 * ------------------------------------------------------------------------- */

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
} screen;

/* A window and a renderer on it, or a skipped case where there is none. */
static void open_screen(screen *opened, tb_rhi_backend backend)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};

    *opened = (screen){0};
    if (tb_window_system_open(NULL, &opened->system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(opened->system,
                         &(tb_window_options){
                             .title = "canvas", .width = 160, .height = 120, .fixed_size = true},
                         &opened->window, NULL),
        ==, TB_WINDOW_OK);
    options.window = opened->window;
    options.device = (tb_rhi_device_options){
        .backend = backend, .adapter = TB_RHI_ADAPTER_SOFTWARE, .validation = true};
    if (tb_renderer_create(&options, &opened->renderer, &error) != TB_RHI_OK)
    {
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no renderer: %s", error.message);
    }
}

static void close_screen(screen *opened)
{
    tb_renderer_destroy(opened->renderer);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

static void begin(tb_renderer *renderer)
{
    const tb_camera camera = tb_camera_orthographic(10.0f);
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
}

/* On the null backend, every quad of a frame is drawn in one draw, whatever
 * its layer; what has no size is not drawn; images come and go. */
TB_TEST(canvas_frame, a_frame_draws_its_quads_in_one_draw_on_the_null_backend)
{
    screen opened = {0};
    tb_canvas_stats stats = {0};
    tb_image *image = NULL;
    uint8_t pixels[4 * 2 * 4] = {0};
    uint32_t width = 0;
    uint32_t height = 0;
    tb_canvas_view view = {0};

    open_screen(&opened, TB_RHI_BACKEND_NULL);
    tb_image_fill_checkerboard(pixels, sizeof(pixels), 4, 2, 1, (tb_vec4){1, 1, 1, 1},
                               (tb_vec4){0, 0, 0, 1});
    image = tb_image_create(opened.renderer,
                            &(tb_image_desc){.width = 4, .height = 2, .filter = TB_IMAGE_NEAREST},
                            pixels, sizeof(pixels));
    tb_image_size(image, &width, &height);
    TB_TEST_EXPECT_UINT(width, ==, 4);
    TB_TEST_EXPECT_UINT(height, ==, 2);
    tb_image_update(opened.renderer, image, (tb_rhi_rect){1, 0, 2, 1}, pixels, 2 * 4);
    tb_image_update(opened.renderer, image, (tb_rhi_rect){0}, pixels, sizeof(pixels));

    begin(opened.renderer);
    view = tb_canvas_view_logical(opened.renderer);
    TB_TEST_EXPECT(view.scale > 0.0f);
    tb_canvas_set_view(opened.renderer, &view);
    tb_canvas_set_layer(opened.renderer, 3);
    tb_canvas_fill_rect(opened.renderer, (tb_canvas_rect){0, 0, 10, 10}, (tb_vec4){1, 0, 0, 1});
    tb_canvas_set_layer(opened.renderer, -2);
    tb_canvas_stroke_rect(opened.renderer, (tb_canvas_rect){0, 0, 10, 10}, 1,
                          (tb_vec4){1, 1, 1, 1});
    tb_canvas_fill_rounded_rect(opened.renderer, (tb_canvas_rect){0, 0, 10, 10}, 2,
                                (tb_vec4){1, 1, 1, 1});
    tb_canvas_stroke_rounded_rect(opened.renderer, (tb_canvas_rect){0, 0, 10, 10}, 2, 1,
                                  (tb_vec4){1, 1, 1, 1});
    tb_canvas_fill_circle(opened.renderer, (tb_vec2){5, 5}, 3, (tb_vec4){1, 1, 1, 1});
    tb_canvas_stroke_circle(opened.renderer, (tb_vec2){5, 5}, 3, 1, (tb_vec4){1, 1, 1, 1});
    tb_canvas_line(opened.renderer, (tb_vec2){0, 0}, (tb_vec2){10, 10}, 2, (tb_vec4){1, 1, 1, 1});
    tb_canvas_draw_image(opened.renderer, image, (tb_vec2){20, 20}, (tb_vec4){1, 1, 1, 1});
    tb_canvas_draw_sprite(opened.renderer, &(tb_canvas_sprite){.image = image,
                                                               .source = {1, 0, 1, 1},
                                                               .position = {30, 30},
                                                               .size = {-16, 16},
                                                               .pivot = {0.5f, 0.5f},
                                                               .rotation = 1.0f,
                                                               .tint = {1, 1, 1, 1}});
    tb_canvas_draw_sprite(
        opened.renderer,
        &(tb_canvas_sprite){.position = {1, 1}, .size = {4, 4}, .tint = {0, 1, 0, 1}});
    /* None of these is drawn: no size, no length, no alpha. */
    tb_canvas_fill_rect(opened.renderer, (tb_canvas_rect){0, 0, 0, 10}, (tb_vec4){1, 1, 1, 1});
    tb_canvas_fill_circle(opened.renderer, (tb_vec2){5, 5}, 0, (tb_vec4){1, 1, 1, 1});
    tb_canvas_line(opened.renderer, (tb_vec2){3, 3}, (tb_vec2){3, 3}, 2, (tb_vec4){1, 1, 1, 1});
    tb_canvas_draw_sprite(opened.renderer, &(tb_canvas_sprite){.image = image});
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
    tb_canvas_stats_of(opened.renderer, &stats);
    TB_TEST_EXPECT_UINT(stats.quads, ==, 10);
    TB_TEST_EXPECT_UINT(stats.dropped, ==, 0);
    TB_TEST_EXPECT_UINT(stats.draws, ==, 1);

    /* A frame that draws nothing in 2D has no 2D draw. */
    begin(opened.renderer);
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
    tb_canvas_stats_of(opened.renderer, &stats);
    TB_TEST_EXPECT_UINT(stats.quads, ==, 0);
    TB_TEST_EXPECT_UINT(stats.draws, ==, 0);

    tb_image_destroy(opened.renderer, image);
    tb_image_destroy(opened.renderer, NULL);
    close_screen(&opened);
}

/* More quads than a frame's uploads hold: the first in order are drawn, the
 * rest dropped and counted. */
TB_TEST(canvas_frame, quads_past_the_frame_uploads_are_dropped)
{
    enum
    {
        quads = 200000
    };
    screen opened = {0};
    tb_canvas_stats stats = {0};

    open_screen(&opened, TB_RHI_BACKEND_NULL);
    begin(opened.renderer);
    for (size_t index = 0; index < quads; index++)
    {
        tb_canvas_fill_rect(opened.renderer, (tb_canvas_rect){0, 0, 1, 1}, (tb_vec4){1, 1, 1, 1});
    }
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
    tb_canvas_stats_of(opened.renderer, &stats);
    TB_TEST_EXPECT_UINT(stats.quads, >, 0);
    TB_TEST_EXPECT_UINT(stats.dropped, >, 0);
    TB_TEST_EXPECT_UINT(stats.quads + stats.dropped, ==, quads);
    close_screen(&opened);
}

static void draw_outside_a_frame(void *context)
{
    tb_canvas_fill_rect(context, (tb_canvas_rect){0, 0, 1, 1}, (tb_vec4){1, 1, 1, 1});
}

/* Drawing 2D outside a frame is a contract violation, caught. */
TB_TEST(canvas_frame, drawing_outside_a_frame_traps)
{
    screen opened = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    open_screen(&opened, TB_RHI_BACKEND_NULL);
    TB_TEST_EXPECT_TRAP(draw_outside_a_frame, opened.renderer, "the canvas drawn outside a frame");
    close_screen(&opened);
}

/* ---------------------------------------------------------------------------
 * Golden pixels on D3D12.
 * ------------------------------------------------------------------------- */

/* A pixel of a capture as 0xAARRGGBB. */
static uint32_t pixel_at(const uint8_t *pixels, uint32_t width, uint32_t x, uint32_t y)
{
    const uint8_t *const bgra = &pixels[((size_t)y * width + x) * 4u];
    return (uint32_t)bgra[0] | (uint32_t)bgra[1] << 8 | (uint32_t)bgra[2] << 16 |
           (uint32_t)bgra[3] << 24;
}

/* One channel of a pixel, 0 blue, 1 green, 2 red. */
static int channel_at(const uint8_t *pixels, uint32_t width, uint32_t x, uint32_t y, int channel)
{
    return pixels[((size_t)y * width + x) * 4u + (size_t)channel];
}

/* The scene the D3D12 case draws in the top left 160 by 120 pixels, over
 * black. */
static void draw_scene(tb_renderer *renderer, const tb_image *checker, const tb_image *strip)
{
    const tb_canvas_view clipped = {.scale = 1.0f, .clip = {140, 0, 20, 20}};
    const tb_canvas_view doubled = {.origin = {0, 100}, .scale = 2.0f};

    /* Edges on whole pixels are crisp. */
    tb_canvas_fill_rect(renderer, (tb_canvas_rect){10, 10, 40, 30}, (tb_vec4){1, 0, 0, 1});
    /* Layer 1 is over layer 0, though drawn first. */
    tb_canvas_set_layer(renderer, 1);
    tb_canvas_fill_rect(renderer, (tb_canvas_rect){20, 20, 20, 20}, (tb_vec4){0, 1, 0, 1});
    tb_canvas_set_layer(renderer, 0);
    tb_canvas_fill_rect(renderer, (tb_canvas_rect){25, 25, 20, 20}, (tb_vec4){0, 0, 1, 1});
    /* Half-transparent white over black is half gray. */
    tb_canvas_fill_rect(renderer, (tb_canvas_rect){100, 10, 20, 20}, (tb_vec4){1, 1, 1, 0.5f});
    /* A rounded rectangle leaves its corners out. */
    tb_canvas_fill_rounded_rect(renderer, (tb_canvas_rect){60, 10, 30, 30}, 10,
                                (tb_vec4){1, 0.6f, 0, 1});
    /* An outline, its inside empty. */
    tb_canvas_stroke_rect(renderer, (tb_canvas_rect){100, 40, 30, 30}, 2, (tb_vec4){0, 1, 1, 1});
    /* A circle whose edge crosses a pixel's middle covers half of it. */
    tb_canvas_fill_circle(renderer, (tb_vec2){130.5f, 90.5f}, 15, (tb_vec4){1, 1, 0, 1});
    /* A checkerboard of 4 by 4 texels drawn 8 pixels a texel, nearest. */
    tb_canvas_draw_sprite(renderer, &(tb_canvas_sprite){.image = checker,
                                                        .position = {10, 50},
                                                        .size = {32, 32},
                                                        .tint = {1, 1, 1, 1}});
    /* The second texel of a strip of red and green: green. */
    tb_canvas_draw_sprite(renderer, &(tb_canvas_sprite){.image = strip,
                                                        .source = {1, 0, 1, 1},
                                                        .position = {50, 50},
                                                        .size = {10, 10},
                                                        .tint = {1, 1, 1, 1}});
    /* A quarter turn clockwise: 20 across becomes 20 down, 10 down becomes
     * 10 to the left. */
    tb_canvas_draw_sprite(renderer, &(tb_canvas_sprite){.position = {80, 50},
                                                        .size = {20, 10},
                                                        .rotation = TB_PI * 0.5f,
                                                        .tint = {1, 0, 1, 1}});
    /* A line at a slant, its edges smoothed. */
    tb_canvas_line(renderer, (tb_vec2){60, 75}, (tb_vec2){100, 95}, 3, (tb_vec4){1, 1, 1, 1});
    /* Only the part inside the view's clip is drawn. */
    tb_canvas_set_view(renderer, &clipped);
    tb_canvas_fill_rect(renderer, (tb_canvas_rect){130, 0, 30, 10}, (tb_vec4){1, 1, 1, 1});
    /* Two pixels a unit: a line 1 unit thick is 2 pixels thick. */
    tb_canvas_set_view(renderer, &doubled);
    tb_canvas_line(renderer, (tb_vec2){5, 5}, (tb_vec2){30, 5}, 1, (tb_vec4){1, 1, 1, 1});
}

/* On D3D12, the 2D layer is drawn over the frame: each shape, image and
 * sprite on its pixels, layers in order, blended and anti-aliased. Pixels
 * well inside a shape are exact; pixels on an edge have a tolerance. */
TB_TEST(canvas_frame, shapes_images_and_sprites_land_where_they_should_on_d3d12)
{
    static const uint8_t strip_pixels[] = {255, 0, 0, 255, 0, 255, 0, 255};
    screen opened = {0};
    uint8_t checker_pixels[4 * 4 * 4] = {0};
    tb_image *checker = NULL;
    tb_image *strip = NULL;
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    tb_canvas_stats stats = {0};
    int smoothed = 0;

    open_screen(&opened, TB_RHI_BACKEND_D3D12);
    tb_image_fill_checkerboard(checker_pixels, sizeof(checker_pixels), 4, 4, 1,
                               (tb_vec4){1, 1, 1, 1}, (tb_vec4){0, 0, 1, 1});
    checker = tb_image_create(opened.renderer,
                              &(tb_image_desc){.width = 4, .height = 4, .filter = TB_IMAGE_NEAREST},
                              checker_pixels, sizeof(checker_pixels));
    strip = tb_image_create(opened.renderer,
                            &(tb_image_desc){.width = 2, .height = 1, .filter = TB_IMAGE_NEAREST},
                            strip_pixels, sizeof(strip_pixels));
    tb_renderer_capture_next_frame(opened.renderer);
    begin(opened.renderer);
    draw_scene(opened.renderer, checker, strip);
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
    tb_canvas_stats_of(opened.renderer, &stats);
    if (stats.draws == 0)
    {
        tb_image_destroy(opened.renderer, checker);
        tb_image_destroy(opened.renderer, strip);
        close_screen(&opened);
        TB_TEST_SKIP("no bindless resources on this device");
    }
    pixels = tb_renderer_captured(opened.renderer, &width, &height);
    TB_TEST_REQUIRE(pixels != NULL);
    /* At least the window's logical size: more on a display scaled up. */
    TB_TEST_REQUIRE_UINT(width, >=, 160);
    TB_TEST_REQUIRE_UINT(height, >=, 120);

    /* The red rectangle, crisp at its edges. */
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 12, 12), ==, 0xFFFF0000u);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 10, 10), ==, 0xFFFF0000u);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 49, 39), ==, 0xFFFF0000u);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 9, 25), ==, 0xFF000000u);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 50, 25), ==, 0xFF000000u);
    /* Green, on layer 1, over blue where they overlap; blue past green. */
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 30, 30), ==, 0xFF00FF00u);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 42, 42), ==, 0xFF0000FFu);
    /* Half gray: premultiplied white at half alpha over black. */
    TB_TEST_EXPECT_INT(abs(channel_at(pixels, width, 110, 20, 2) - 128), <=, 1);
    TB_TEST_EXPECT_INT(abs(channel_at(pixels, width, 110, 20, 1) - 128), <=, 1);
    /* The rounded rectangle: orange inside, its corner left out. */
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 75, 25), ==, 0xFFFF9900u);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 75, 10), ==, 0xFFFF9900u);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 60, 10), ==, 0xFF000000u);
    /* The outline: cyan on its edge, black inside. */
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 101, 55), ==, 0xFF00FFFFu);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 115, 55), ==, 0xFF000000u);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 103, 55), ==, 0xFF000000u);
    /* The circle: yellow in the middle, half covered where its edge crosses
     * a pixel's middle, black past it. */
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 130, 90), ==, 0xFFFFFF00u);
    TB_TEST_EXPECT_INT(abs(channel_at(pixels, width, 145, 90, 2) - 128), <=, 8);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 147, 90), ==, 0xFF000000u);
    /* The checkerboard, 8 pixels a texel: white, then blue. */
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 10, 50), ==, 0xFFFFFFFFu);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 17, 57), ==, 0xFFFFFFFFu);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 18, 57), ==, 0xFF0000FFu);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 18, 58), ==, 0xFFFFFFFFu);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 41, 81), ==, 0xFFFFFFFFu);
    /* The strip's second texel. */
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 55, 55), ==, 0xFF00FF00u);
    /* The turned sprite spans 70 to 80 across and 50 to 70 down. */
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 75, 65), ==, 0xFFFF00FFu);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 85, 55), ==, 0xFF000000u);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 75, 72), ==, 0xFF000000u);
    /* The slanted line: white on it, black off it, and partly covered
     * pixels between. */
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 80, 85), ==, 0xFFFFFFFFu);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 80, 90), ==, 0xFF000000u);
    for (uint32_t y = 80; y < 92; y++)
    {
        const int value = channel_at(pixels, width, 80, y, 1);
        smoothed += value > 16 && value < 240 ? 1 : 0;
    }
    TB_TEST_EXPECT_INT(smoothed, >=, 2);
    /* Clipped: only from x 140 on. */
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 135, 5), ==, 0xFF000000u);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 145, 5), ==, 0xFFFFFFFFu);
    /* The doubled line: rows 109 and 110, from x 10 to 60. */
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 30, 109), ==, 0xFFFFFFFFu);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 30, 110), ==, 0xFFFFFFFFu);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 30, 112), ==, 0xFF000000u);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 8, 110), ==, 0xFF000000u);

    tb_image_destroy(opened.renderer, checker);
    tb_image_destroy(opened.renderer, strip);
    close_screen(&opened);
}
