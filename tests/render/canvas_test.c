/* The canvas: views fit a frame and map points both ways; pixels made in
 * memory are what they say; and on the null backend, a frame's quads are
 * drawn in one draw and counted. canvas_d3d12_test.c checks the pixels. */
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

/* The design's view is over the viewport, in its units, clipped to it; a
 * viewport stretched by `ignore` stretches the view each way, its scale
 * the smaller, which text is drawn at. */
TB_TEST(canvas_view, a_view_of_the_viewport_is_in_the_designs_units)
{
    const tb_viewport_policy keep = TB_VIEWPORT_POLICY_INIT;
    tb_viewport_policy ignore = TB_VIEWPORT_POLICY_INIT;
    const tb_viewport kept =
        tb_viewport_compute(&keep, &(tb_viewport_frame){.width = 1920, .height = 1200});
    tb_canvas_view view = tb_canvas_view_viewport(&kept);

    TB_TEST_EXPECT_FLOAT(view.origin.x, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(view.origin.y, ==, 60.0f);
    TB_TEST_EXPECT_FLOAT(view.scale, ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(view.stretch.x, ==, 0.0f);
    TB_TEST_EXPECT_INT(view.clip.y, ==, 60);
    TB_TEST_EXPECT_UINT(view.clip.height, ==, 1080);
    tb_vec2 pixel = tb_canvas_view_to_pixels(&view, (tb_vec2){960, 540});
    TB_TEST_EXPECT_FLOAT(pixel.y, ==, 600.0f);

    ignore.aspect = TB_VIEWPORT_ASPECT_IGNORE;
    const tb_viewport stretched =
        tb_viewport_compute(&ignore, &(tb_viewport_frame){.width = 960, .height = 1080});
    view = tb_canvas_view_viewport(&stretched);
    TB_TEST_EXPECT_FLOAT(view.scale, ==, 0.5f);
    TB_TEST_EXPECT_FLOAT(view.stretch.x, ==, 0.5f);
    TB_TEST_EXPECT_FLOAT(view.stretch.y, ==, 1.0f);
    pixel = tb_canvas_view_to_pixels(&view, (tb_vec2){1920, 1080});
    TB_TEST_EXPECT_FLOAT(pixel.x, ==, 960.0f);
    TB_TEST_EXPECT_FLOAT(pixel.y, ==, 1080.0f);
    const tb_vec2 point = tb_canvas_view_from_pixels(&view, (tb_vec2){480, 540});
    TB_TEST_EXPECT_FLOAT(point.x, ==, 960.0f);
    TB_TEST_EXPECT_FLOAT(point.y, ==, 540.0f);

    /* A viewport of no size clips nothing away, as the whole frame. */
    const tb_viewport empty =
        tb_viewport_compute(&keep, &(tb_viewport_frame){.width = 0, .height = 0});
    view = tb_canvas_view_viewport(&empty);
    TB_TEST_EXPECT_UINT(view.clip.width, ==, 0);
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
