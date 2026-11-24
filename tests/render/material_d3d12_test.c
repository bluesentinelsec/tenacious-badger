/* Materials on D3D12, on WARP, where Windows builds it: a texture's texels
 * land where they should on a quad facing the camera, its mip chain
 * averages in linear light, and a normal map's up is up. */
#include "badger/canvas.h"
#include "badger/log.h"
#include "badger/material.h"
#include "badger/memory.h"
#include "badger/window.h"

#include "display_color.h"
#include "tb_test.h"

#include <stdlib.h>

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
    tb_mesh *quad;
    tb_log *log;
} screen;

/* What validation warns of fails the case: uploads, mip levels and draws
 * that break D3D12's rules show here even where the pixels come out right. */
static bool hear(void *context, const tb_log_record *record)
{
    (void)context;
    if (record->level >= TB_LOG_WARNING)
    {
        TB_TEST_FAIL("validation: %.*s", (int)record->length, record->message);
    }
    return true;
}

/* A quad from -1 to 1 in X and Y, facing +Z, its texture's top left at its
 * top left. */
static tb_mesh *make_quad(tb_renderer *renderer)
{
    static const tb_mesh_vertex vertices[4] = {{{-1, 1, 0}, {0, 0, 1}, {0, 0}},
                                               {{1, 1, 0}, {0, 0, 1}, {1, 0}},
                                               {{-1, -1, 0}, {0, 0, 1}, {0, 1}},
                                               {{1, -1, 0}, {0, 0, 1}, {1, 1}}};
    static const uint32_t indices[6] = {0, 2, 1, 1, 2, 3};

    return tb_mesh_create(renderer, vertices, 4, indices, 6);
}

/* A window and a renderer on it, or a skipped case where there is none. */
static void open_screen(screen *opened)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};
    tb_rhi_device_info info = {0};
    tb_log_config config = TB_LOG_CONFIG_INIT;

    *opened = (screen){0};
    if (tb_window_system_open(NULL, &opened->system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(opened->system,
                         &(tb_window_options){
                             .title = "materials", .width = 160, .height = 120, .fixed_size = true},
                         &opened->window, NULL),
        ==, TB_WINDOW_OK);
    opened->log = tb_log_create();
    config.minimum = TB_LOG_TRACE;
    config.write_record = hear;
    config.flush = NULL;
    TB_TEST_REQUIRE_UINT(tb_log_configure(opened->log, &config), ==, TB_LOG_OK);
    options.window = opened->window;
    options.log = opened->log;
    options.device = (tb_rhi_device_options){
        .backend = TB_RHI_BACKEND_D3D12, .adapter = TB_RHI_ADAPTER_SOFTWARE, .validation = true};
    if (tb_renderer_create(&options, &opened->renderer, &error) != TB_RHI_OK)
    {
        (void)tb_log_destroy(opened->log);
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no renderer: %s", error.message);
    }
    tb_rhi_device_info_of(tb_renderer_device(opened->renderer), &info);
    if ((info.features & TB_RHI_FEATURE_BINDLESS) == 0)
    {
        tb_renderer_destroy(opened->renderer);
        (void)tb_log_destroy(opened->log);
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no bindless resources on this device");
    }
    opened->quad = make_quad(opened->renderer);
}

static void close_screen(screen *opened)
{
    tb_mesh_destroy(opened->renderer, opened->quad);
    tb_renderer_destroy(opened->renderer);
    (void)tb_log_destroy(opened->log);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

/* Draw quads with materials, through an orthographic camera that shows
 * from -1 to 1 down: each quad fills the view's height. */
static const uint8_t *draw_quads(screen *opened, const tb_material *const *materials,
                                 const tb_mat4 *transforms, size_t count, uint32_t *width,
                                 uint32_t *height)
{
    const tb_camera camera = tb_camera_orthographic(2.0f);
    const uint8_t *pixels = NULL;

    tb_renderer_capture_next_frame(opened->renderer);
    TB_TEST_REQUIRE(tb_renderer_begin_frame(opened->renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    for (size_t index = 0; index < count; index++)
    {
        tb_renderer_draw_mesh_material(opened->renderer, opened->quad, transforms[index],
                                       materials[index]);
    }
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened->renderer), ==, TB_RHI_OK);
    pixels = tb_renderer_captured(opened->renderer, width, height);
    TB_TEST_REQUIRE(pixels != NULL);
    return pixels;
}

/* One channel of a pixel, 0 blue, 1 green, 2 red. */
static int channel_at(const uint8_t *pixels, uint32_t width, uint32_t x, uint32_t y, int channel)
{
    return pixels[((size_t)y * width + x) * 4u + (size_t)channel];
}

/* Whether a pixel is a color, each channel within a tolerance. */
static bool pixel_near(const uint8_t *pixels, uint32_t width, uint32_t x, uint32_t y, int red,
                       int green, int blue, int tolerance)
{
    return abs(channel_at(pixels, width, x, y, 2) - red) <= tolerance &&
           abs(channel_at(pixels, width, x, y, 1) - green) <= tolerance &&
           abs(channel_at(pixels, width, x, y, 0) - blue) <= tolerance;
}

/* A 4 by 4 texture, unlit, on a quad facing the camera: each texel covers a
 * quarter of the quad each way, its top left at the quad's top left. A
 * checkerboard of white and black, its corners marked: red at the top
 * left, green at the top right, blue at the bottom left. */
TB_TEST(material_frame, texels_land_where_they_should_on_d3d12)
{
    screen opened = {0};
    uint8_t texels[4 * 4 * 4] = {0};
    tb_texture *texture = NULL;
    tb_material *material = NULL;
    tb_material_desc desc = TB_MATERIAL_DESC_INIT;
    const tb_mat4 transform = tb_mat4_identity();
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t left = 0;
    uint32_t texel = 0;
    /* Each channel of 255 in sRGB is 1 in linear light, which the output
     * transform shows a little darker. A pixel's middle may miss its texel's
     * by half a pixel at some sizes of the view, so that bilinear filtering
     * brings in up to a sixtieth of each neighbor, and the output
     * transform's toe raises a black that took a little white a long way:
     * the tolerance is what 0.05 in linear light shows as. */
    const int white = display_channel(1.0f);
    const int near = display_channel(0.05f);

    open_screen(&opened);
    tb_image_fill_checkerboard(texels, sizeof(texels), 4, 4, 1, (tb_vec4){1, 1, 1, 1},
                               (tb_vec4){0, 0, 0, 1});
    texels[0] = 255; /* the top left, red */
    texels[1] = 0;
    texels[2] = 0;
    texels[3 * 4 + 0] = 0; /* the top right, green */
    texels[3 * 4 + 1] = 255;
    texels[3 * 4 + 2] = 0;
    texels[12 * 4 + 0] = 0; /* the bottom left, blue */
    texels[12 * 4 + 1] = 0;
    texels[12 * 4 + 2] = 255;
    texture = tb_texture_create(opened.renderer, &(tb_texture_desc){.width = 4, .height = 4},
                                texels, sizeof(texels));
    desc.base_color = texture;
    desc.unlit = true;
    material = tb_material_create(opened.renderer, &desc);
    pixels =
        draw_quads(&opened, (const tb_material *const[]){material}, &transform, 1, &width, &height);
    /* The quad is as wide as it is high, in the middle of the view. */
    left = (width - height) / 2u;
    texel = height / 4u;
    for (uint32_t row = 0; row < 4; row++)
    {
        for (uint32_t column = 0; column < 4; column++)
        {
            const uint32_t x = left + texel * column + texel / 2u;
            const uint32_t y = texel * row + texel / 2u;
            const int expected = (row + column) % 2u == 0 ? white : 0;
            TB_TEST_CONTEXT("texel %u, %u", column, row);
            if (row == 0 && column == 0)
            {
                TB_TEST_EXPECT(pixel_near(pixels, width, x, y, white, 0, 0, near));
            }
            else if (row == 0 && column == 3)
            {
                TB_TEST_EXPECT(pixel_near(pixels, width, x, y, 0, white, 0, near));
            }
            else if (row == 3 && column == 0)
            {
                TB_TEST_EXPECT(pixel_near(pixels, width, x, y, 0, 0, white, near));
            }
            else
            {
                TB_TEST_EXPECT(pixel_near(pixels, width, x, y, expected, expected, expected, near));
            }
        }
    }
    /* Outside the quad: the clear color. */
    TB_TEST_EXPECT(pixel_near(pixels, width, left / 2u, height / 2u, 0, 0, 0, 0));
    tb_material_destroy(opened.renderer, material);
    tb_texture_destroy(opened.renderer, texture);
    close_screen(&opened);
}

/* A checkerboard of single black and white texels, tiled so that many
 * texels fall in each pixel, is sampled from its smaller levels: an even
 * gray. The levels average in linear light, so the gray is half of white
 * in linear (177 once the output transform shows it); averaged in sRGB it
 * would be about a fifth of white in linear (117). */
TB_TEST(material_frame, mip_levels_average_in_linear_light_on_d3d12)
{
    enum
    {
        SIZE = 64
    };
    screen opened = {0};
    uint8_t *texels = NULL;
    tb_texture *texture = NULL;
    tb_material *material = NULL;
    tb_material_desc desc = TB_MATERIAL_DESC_INIT;
    const tb_mat4 transform = tb_mat4_identity();
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mips = 0;

    open_screen(&opened);
    texels = tb_malloc((size_t)SIZE * SIZE * 4u);
    tb_image_fill_checkerboard(texels, (size_t)SIZE * SIZE * 4u, SIZE, SIZE, 1,
                               (tb_vec4){1, 1, 1, 1}, (tb_vec4){0, 0, 0, 1});
    texture = tb_texture_create(opened.renderer, &(tb_texture_desc){.width = SIZE, .height = SIZE},
                                texels, (size_t)SIZE * SIZE * 4u);
    tb_texture_size(texture, &width, &height, &mips);
    TB_TEST_EXPECT_UINT(mips, ==, 7);
    desc.base_color = texture;
    desc.unlit = true;
    desc.uv_scale = (tb_vec2){16, 16};
    material = tb_material_create(opened.renderer, &desc);
    pixels =
        draw_quads(&opened, (const tb_material *const[]){material}, &transform, 1, &width, &height);
    for (uint32_t y = height / 4u; y < height * 3u / 4u; y += height / 8u)
    {
        const uint32_t x = width / 2u - height / 4u + y / 2u;
        TB_TEST_CONTEXT("pixel %u, %u", x, y);
        const int gray = display_channel(0.5f);
        TB_TEST_EXPECT(pixel_near(pixels, width, x, y, gray, gray, gray, 12));
    }
    tb_material_destroy(opened.renderer, material);
    tb_texture_destroy(opened.renderer, texture);
    tb_free(texels);
    close_screen(&opened);
}

/* A normal map's +Y is up in the image: a quad whose normals lean up, lit
 * from above, is brighter than one whose normals lean down, and a flat one
 * is between. */
TB_TEST(material_frame, a_normal_maps_up_is_up_on_d3d12)
{
    static const uint8_t up[4] = {128, 218, 218, 255};
    static const uint8_t down[4] = {128, 38, 218, 255};
    screen opened = {0};
    tb_texture *leaning_up = NULL;
    tb_texture *leaning_down = NULL;
    tb_material *materials[3] = {0};
    tb_material_desc desc = TB_MATERIAL_DESC_INIT;
    const tb_mat4 transforms[3] = {
        tb_mat4_from_trs((tb_vec3){-0.9f, 0, 0}, tb_quat_identity(), (tb_vec3){0.3f, 0.3f, 1}),
        tb_mat4_from_trs((tb_vec3){0, 0, 0}, tb_quat_identity(), (tb_vec3){0.3f, 0.3f, 1}),
        tb_mat4_from_trs((tb_vec3){0.9f, 0, 0}, tb_quat_identity(), (tb_vec3){0.3f, 0.3f, 1})};
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    int brightness[3] = {0};

    open_screen(&opened);
    leaning_up = tb_texture_create(
        opened.renderer, &(tb_texture_desc){.width = 1, .height = 1, .content = TB_TEXTURE_NORMAL},
        up, sizeof(up));
    leaning_down = tb_texture_create(
        opened.renderer, &(tb_texture_desc){.width = 1, .height = 1, .content = TB_TEXTURE_NORMAL},
        down, sizeof(down));
    desc.normal = leaning_up;
    materials[0] = tb_material_create(opened.renderer, &desc);
    desc.normal = NULL;
    materials[1] = tb_material_create(opened.renderer, &desc);
    desc.normal = leaning_down;
    materials[2] = tb_material_create(opened.renderer, &desc);
    pixels =
        draw_quads(&opened, (const tb_material *const *)materials, transforms, 3, &width, &height);
    for (int quad = 0; quad < 3; quad++)
    {
        /* The middle of each quad: -0.9, 0 and 0.9 across, of a view
         * height / 2 pixels a unit. */
        const float unit = (float)height * 0.5f;
        const uint32_t x = (uint32_t)((float)width * 0.5f + ((float)quad - 1.0f) * 0.9f * unit);
        brightness[quad] = channel_at(pixels, width, x, height / 2u, 1);
    }
    TB_TEST_EXPECT_INT(brightness[0], >, brightness[1] + 20);
    TB_TEST_EXPECT_INT(brightness[1], >, brightness[2] + 20);
    for (int index = 0; index < 3; index++)
    {
        tb_material_destroy(opened.renderer, materials[index]);
    }
    tb_texture_destroy(opened.renderer, leaning_up);
    tb_texture_destroy(opened.renderer, leaning_down);
    close_screen(&opened);
}
