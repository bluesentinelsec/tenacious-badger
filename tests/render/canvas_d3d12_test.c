/* The canvas on D3D12, on WARP, where Windows builds it: shapes, images and
 * sprites land on the pixels they should, layers in order, blended and
 * anti-aliased. */
#include "badger/canvas.h"
#include "badger/window.h"

#include "tb_test.h"

#include <stdlib.h>

/* The backend the case runs on: each runs on D3D12, then on D3D11. */
static tb_rhi_backend tb_test_backend = TB_RHI_BACKEND_D3D12;

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
static void shapes_images_and_sprites_land_where_they_should_case(void)
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

    open_screen(&opened, tb_test_backend);
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

TB_TEST(canvas_frame, shapes_images_and_sprites_land_where_they_should_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    shapes_images_and_sprites_land_where_they_should_case();
}

TB_TEST(canvas_frame, shapes_images_and_sprites_land_where_they_should_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    shapes_images_and_sprites_land_where_they_should_case();
}
