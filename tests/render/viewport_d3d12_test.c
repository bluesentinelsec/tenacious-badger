/* The viewport render mode on D3D12, on WARP, where Windows builds it: what
 * is drawn in the design's units lands in the viewport's target at a pixel
 * a unit, and is upscaled into the viewport, in whole blocks at whole
 * pixels a unit and filtered linearly otherwise; the bars are the policy's
 * color exactly; and what is drawn in a view of the frame is drawn over
 * the upscaled frame at its own pixels. */
#include "badger/canvas.h"
#include "badger/memory.h"
#include "badger/window.h"

#include "tb_test.h"

#include <math.h>
#include <stdlib.h>

/* The backend the case runs on: each runs on D3D12, then on D3D11. */
static tb_rhi_backend tb_test_backend = TB_RHI_BACKEND_D3D12;

/* The frame the cases ask for: 1280 by 720, four times a 320 by 180
 * design. A display that gives another size changes only how many pixels
 * a unit the cases expect. */
#define FRAME_WIDTH 1280u
#define FRAME_HEIGHT 720u
/* The most a linearly filtered channel may differ from what the filter's
 * arithmetic on the CPU gives: rounding on the GPU, and WARP's sampling
 * precision. */
#define LINEAR_TOLERANCE 2

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
} screen;

/* A window and a renderer on it, or a skipped case where there is none. */
static void open_screen(screen *opened)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};

    *opened = (screen){0};
    if (tb_window_system_open(NULL, &opened->system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(tb_window_create(opened->system,
                                          &(tb_window_options){.title = "viewport",
                                                               .width = FRAME_WIDTH,
                                                               .height = FRAME_HEIGHT,
                                                               .fixed_size = true},
                                          &opened->window, NULL),
                         ==, TB_WINDOW_OK);
    options.window = opened->window;
    options.device = (tb_rhi_device_options){
        .backend = tb_test_backend, .adapter = TB_RHI_ADAPTER_SOFTWARE, .validation = true};
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

/* An opaque image whose every pixel differs from its neighbours': its red
 * of its column, its green of its row, its blue of both. */
static uint32_t pattern_rgb(uint32_t x, uint32_t y)
{
    return (x * 37u + y * 11u) % 256u << 16 | (y * 53u + x * 5u) % 256u << 8 |
           (x * 13u + y * 29u) % 256u;
}

/* An opaque image of `pattern_rgb`, filtered nearest, a texel a unit. */
static tb_image *make_pattern(tb_renderer *renderer, uint32_t width, uint32_t height)
{
    const size_t size = (size_t)width * height * 4u;
    uint8_t *const pixels = tb_calloc(size, 1);
    tb_image *image = NULL;

    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            const uint32_t rgb = pattern_rgb(x, y);
            uint8_t *const pixel = &pixels[((size_t)y * width + x) * 4u];
            pixel[0] = (uint8_t)(rgb >> 16);
            pixel[1] = (uint8_t)(rgb >> 8);
            pixel[2] = (uint8_t)rgb;
            pixel[3] = UINT8_MAX;
        }
    }
    image = tb_image_create(
        renderer,
        &(tb_image_desc){
            .width = width, .height = height, .filter = TB_IMAGE_NEAREST, .name = "pattern"},
        pixels, size);
    tb_free(pixels);
    return image;
}

/* An opaque image of two ramps, red across and green down, from 0 to 255:
 * what a linear filter turns into the same ramps at any scale. */
static tb_image *make_ramps(tb_renderer *renderer, uint32_t width, uint32_t height)
{
    const size_t size = (size_t)width * height * 4u;
    uint8_t *const pixels = tb_calloc(size, 1);
    tb_image *image = NULL;

    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            uint8_t *const pixel = &pixels[((size_t)y * width + x) * 4u];
            pixel[0] = (uint8_t)(x * 255u / (width - 1u));
            pixel[1] = (uint8_t)(y * 255u / (height - 1u));
            pixel[3] = UINT8_MAX;
        }
    }
    image = tb_image_create(
        renderer,
        &(tb_image_desc){
            .width = width, .height = height, .filter = TB_IMAGE_NEAREST, .name = "ramps"},
        pixels, size);
    tb_free(pixels);
    return image;
}

/* Draw a frame of an image over the design, in the design's view, and a
 * magenta pixel of the frame's view at (`*mark`, `*mark`) unless `mark` is
 * NULL, then keep its pixels. */
static const uint8_t *draw_frame(tb_renderer *renderer, const tb_image *image, const uint32_t *mark,
                                 uint32_t *width, uint32_t *height)
{
    const tb_camera camera = tb_camera_orthographic(10.0f);
    const tb_canvas_view frame = tb_canvas_view_pixels();

    tb_renderer_capture_next_frame(renderer);
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    const tb_viewport viewport = tb_renderer_viewport(renderer);
    const tb_canvas_view design = tb_canvas_view_viewport(&viewport);
    tb_canvas_set_view(renderer, &design);
    tb_canvas_draw_image(renderer, image, (tb_vec2){0, 0}, (tb_vec4){1, 1, 1, 1});
    if (mark != NULL)
    {
        tb_canvas_set_view(renderer, &frame);
        tb_canvas_fill_rect(renderer, (tb_canvas_rect){(float)*mark, (float)*mark, 1, 1},
                            (tb_vec4){1, 0, 1, 1});
    }
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
    return tb_renderer_captured(renderer, width, height);
}

/* A design of 320 by 180 at whole pixels a unit, on a 1280 by 720 frame:
 * the target is the design, a pixel a unit, and the frame shows each of
 * its pixels as an exact block of 4 by 4, nearest-neighbour. A pixel drawn
 * in the frame's view is one pixel of the frame, over the blocks, not a
 * block itself. */
static void whole_pixels_a_unit_upscale_into_exact_blocks_case(void)
{
    screen opened = {0};
    tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
    tb_image *pattern = NULL;
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mismatched = 0;
    const uint32_t mark = 5;

    open_screen(&opened);
    policy.design_width = 320;
    policy.design_height = 180;
    policy.aspect = TB_VIEWPORT_ASPECT_KEEP;
    policy.scale_mode = TB_VIEWPORT_SCALE_INTEGER;
    policy.render_mode = TB_VIEWPORT_RENDER_VIEWPORT;
    tb_renderer_set_viewport_policy(opened.renderer, &policy, 0.0f);
    pattern = make_pattern(opened.renderer, 320, 180);
    pixels = draw_frame(opened.renderer, pattern, &mark, &width, &height);
    TB_TEST_REQUIRE(pixels != NULL);
    const tb_viewport viewport = tb_renderer_viewport(opened.renderer);
    TB_TEST_REQUIRE_UINT(viewport.target_width, ==, 320u);
    TB_TEST_REQUIRE_UINT(viewport.target_height, ==, 180u);
    TB_TEST_REQUIRE_FLOAT(viewport.scale.x, ==, viewport.scale.y);
    const uint32_t block = (uint32_t)viewport.scale.x;
    TB_TEST_REQUIRE_FLOAT((float)block, ==, viewport.scale.x);
    if (width == FRAME_WIDTH && height == FRAME_HEIGHT)
    {
        TB_TEST_EXPECT_UINT(block, ==, 4u);
    }
    for (uint32_t y = 0; y < viewport.height; y++)
    {
        for (uint32_t x = 0; x < viewport.width; x++)
        {
            const uint32_t frame_x = viewport.x + x;
            const uint32_t frame_y = viewport.y + y;
            if (frame_x == mark && frame_y == mark)
            {
                continue;
            }
            const uint32_t expected = 0xFF000000u | pattern_rgb(x / block, y / block);
            if (pixel_at(pixels, width, frame_x, frame_y) != expected)
            {
                mismatched++;
            }
        }
    }
    TB_TEST_EXPECT_UINT(mismatched, ==, 0u);
    /* The frame's pixel, at the frame's resolution over the upscale. */
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, mark, mark), ==, 0xFFFF00FFu);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, mark + 1u, mark), !=, 0xFFFF00FFu);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, mark, mark + 1u), !=, 0xFFFF00FFu);
    tb_image_destroy(opened.renderer, pattern);
    close_screen(&opened);
}

TB_TEST(viewport_render, whole_pixels_a_unit_upscale_into_exact_blocks_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    whole_pixels_a_unit_upscale_into_exact_blocks_case();
}

TB_TEST(viewport_render, whole_pixels_a_unit_upscale_into_exact_blocks_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    whole_pixels_a_unit_upscale_into_exact_blocks_case();
}

/* The red a linear filter gives at a pixel of the frame, along a ramp of
 * `texels` from 0 to 255 shown across `pixels` of the viewport: the texel
 * centers' ramp, held at its ends. */
static float linear_ramp(uint32_t pixel, uint32_t pixels, uint32_t texels)
{
    const float texel = ((float)pixel + 0.5f) / (float)pixels * (float)texels - 0.5f;
    const float held = fminf(fmaxf(texel, 0.0f), (float)(texels - 1u));
    return held * 255.0f / (float)(texels - 1u);
}

/* A design of 320 by 200, kept, at fractional scales, on a 16:9 frame:
 * pillarboxed, with the bars the policy's color exactly, every pixel of
 * them, and the target upscaled linearly, each pixel of the viewport the
 * ramps' value where it samples, within rounding. */
static void fractional_scales_filter_linearly_between_exact_bars_case(void)
{
    screen opened = {0};
    tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
    tb_image *ramps = NULL;
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bar_pixels = 0;
    uint32_t wrong_bars = 0;
    int worst = 0;

    open_screen(&opened);
    policy.design_width = 320;
    policy.design_height = 200;
    policy.aspect = TB_VIEWPORT_ASPECT_KEEP;
    policy.render_mode = TB_VIEWPORT_RENDER_VIEWPORT;
    TB_TEST_REQUIRE(tb_viewport_parse_color("#204060", &policy.bar_color));
    tb_renderer_set_viewport_policy(opened.renderer, &policy, 0.0f);
    ramps = make_ramps(opened.renderer, 320, 200);
    pixels = draw_frame(opened.renderer, ramps, NULL, &width, &height);
    TB_TEST_REQUIRE(pixels != NULL);
    const tb_viewport viewport = tb_renderer_viewport(opened.renderer);
    TB_TEST_REQUIRE_UINT(viewport.target_width, ==, 320u);
    TB_TEST_REQUIRE_UINT(viewport.target_height, ==, 200u);
    TB_TEST_REQUIRE(viewport.x > 1u || viewport.y > 1u);
    TB_TEST_REQUIRE(viewport.scale.x != floorf(viewport.scale.x));
    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            const bool inside = x >= viewport.x && x < viewport.x + viewport.width &&
                                y >= viewport.y && y < viewport.y + viewport.height;
            if (inside)
            {
                continue;
            }
            bar_pixels++;
            if (pixel_at(pixels, width, x, y) != 0xFF204060u)
            {
                wrong_bars++;
            }
        }
    }
    TB_TEST_EXPECT_UINT(bar_pixels, >, 0u);
    TB_TEST_EXPECT_UINT(wrong_bars, ==, 0u);
    for (uint32_t y = 0; y < viewport.height; y++)
    {
        for (uint32_t x = 0; x < viewport.width; x++)
        {
            const float red = linear_ramp(x, viewport.width, 320);
            const float green = linear_ramp(y, viewport.height, 200);
            const int red_off = abs(channel_at(pixels, width, viewport.x + x, viewport.y + y, 2) -
                                    (int)lroundf(red));
            const int green_off = abs(channel_at(pixels, width, viewport.x + x, viewport.y + y, 1) -
                                      (int)lroundf(green));
            worst = red_off > worst ? red_off : worst;
            worst = green_off > worst ? green_off : worst;
        }
    }
    TB_TEST_EXPECT_INT(worst, <=, LINEAR_TOLERANCE);
    tb_image_destroy(opened.renderer, ramps);
    close_screen(&opened);
}

TB_TEST(viewport_render, fractional_scales_filter_linearly_between_exact_bars_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    fractional_scales_filter_linearly_between_exact_bars_case();
}

TB_TEST(viewport_render, fractional_scales_filter_linearly_between_exact_bars_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    fractional_scales_filter_linearly_between_exact_bars_case();
}
