/* Textures and materials on the null backend, which every platform runs:
 * textures know their size and mip chain, borrowed textures stay their
 * caller's, and frames draw materials beside plain meshes as the
 * anisotropy changes. */
#include "badger/canvas.h"
#include "badger/engine_settings.h"
#include "badger/material.h"
#include "badger/memory.h"
#include "badger/settings.h"
#include "badger/window.h"

#include "tb_test.h"

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
} screen;

/* A window and a renderer on the null backend, or a skipped case where
 * there is no windowing. */
static void open_screen(screen *opened, uint32_t anisotropy)
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
                             .title = "materials", .width = 160, .height = 120, .fixed_size = true},
                         &opened->window, NULL),
        ==, TB_WINDOW_OK);
    options.window = opened->window;
    options.device = (tb_rhi_device_options){.backend = TB_RHI_BACKEND_NULL};
    options.anisotropy = anisotropy;
    TB_TEST_REQUIRE_UINT(tb_renderer_create(&options, &opened->renderer, &error), ==, TB_RHI_OK);
}

static void close_screen(screen *opened)
{
    tb_renderer_destroy(opened->renderer);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

/* Pixels of a texture of a size, a checkerboard. */
static uint8_t *checker_pixels(uint32_t width, uint32_t height, size_t *size)
{
    uint8_t *pixels = NULL;

    *size = (size_t)width * height * 4u;
    pixels = tb_malloc(*size);
    tb_image_fill_checkerboard(pixels, *size, width, height, 1, (tb_vec4){1, 1, 1, 1},
                               (tb_vec4){0, 0, 0, 1});
    return pixels;
}

/* A texture has the full mip chain of its size unless it asks for none:
 * every level down to one texel. */
TB_TEST(material_texture, a_texture_has_the_mip_chain_of_its_size)
{
    screen opened = {0};
    size_t size = 0;
    uint8_t *const pixels = checker_pixels(16, 6, &size);
    tb_texture *color = NULL;
    tb_texture *data = NULL;
    tb_texture *normal = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mips = 0;

    open_screen(&opened, 0);
    color = tb_texture_create(opened.renderer, &(tb_texture_desc){.width = 16, .height = 6}, pixels,
                              size);
    data = tb_texture_create(opened.renderer,
                             &(tb_texture_desc){.width = 16,
                                                .height = 6,
                                                .content = TB_TEXTURE_DATA,
                                                .no_mipmaps = true,
                                                .name = "data"},
                             pixels, size);
    normal = tb_texture_create(
        opened.renderer, &(tb_texture_desc){.width = 16, .height = 6, .content = TB_TEXTURE_NORMAL},
        pixels, size);
    tb_texture_size(color, &width, &height, &mips);
    TB_TEST_EXPECT_UINT(width, ==, 16);
    TB_TEST_EXPECT_UINT(height, ==, 6);
    TB_TEST_EXPECT_UINT(mips, ==, 5);
    tb_texture_size(data, &width, &height, &mips);
    TB_TEST_EXPECT_UINT(mips, ==, 1);
    tb_texture_size(normal, &width, &height, NULL);
    TB_TEST_EXPECT_UINT(width, ==, 16);
    tb_texture_destroy(opened.renderer, normal);
    tb_texture_destroy(opened.renderer, data);
    tb_texture_destroy(opened.renderer, color);
    tb_texture_destroy(opened.renderer, NULL);
    tb_free(pixels);
    close_screen(&opened);
}

/* A texture of the caller's own RHI texture borrows it: destroying the
 * texture leaves the RHI's texture alive, the caller's to destroy. */
TB_TEST(material_texture, a_texture_borrows_a_callers_rhi_texture)
{
    screen opened = {0};
    tb_rhi_device *device = NULL;
    tb_rhi_texture made = {0};
    tb_rhi_texture_desc desc = {0};
    tb_texture *texture = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mips = 0;

    open_screen(&opened, 0);
    device = tb_renderer_device(opened.renderer);
    TB_TEST_REQUIRE_UINT(tb_rhi_texture_create(device,
                                               &(tb_rhi_texture_desc){
                                                   .format = TB_RHI_FORMAT_R8G8B8A8_SRGB,
                                                   .width = 32,
                                                   .height = 8,
                                                   .mips = 2,
                                                   .usage = TB_RHI_TEXTURE_SAMPLED,
                                               },
                                               &made, NULL),
                         ==, TB_RHI_OK);
    texture = tb_texture_from_rhi(opened.renderer, made);
    tb_texture_size(texture, &width, &height, &mips);
    TB_TEST_EXPECT_UINT(width, ==, 32);
    TB_TEST_EXPECT_UINT(height, ==, 8);
    TB_TEST_EXPECT_UINT(mips, ==, 2);
    tb_texture_destroy(opened.renderer, texture);
    /* Still alive: describing a stale handle would be a contract violation. */
    tb_rhi_texture_desc_of(device, made, &desc);
    TB_TEST_EXPECT_UINT(desc.width, ==, 32);
    tb_rhi_texture_destroy(device, made);
    close_screen(&opened);
}

/* Frames draw meshes with materials, with textures and without, beside
 * plain meshes; the anisotropy changes between frames; and materials go
 * before their textures. */
TB_TEST(material_frame, materials_draw_on_the_null_backend)
{
    screen opened = {0};
    size_t size = 0;
    uint8_t *const pixels = checker_pixels(8, 8, &size);
    tb_mesh *cube = NULL;
    tb_texture *base = NULL;
    tb_texture *normal = NULL;
    tb_texture *surface = NULL;
    tb_material *plain = NULL;
    tb_material *textured = NULL;
    tb_material_desc desc = TB_MATERIAL_DESC_INIT;
    tb_camera camera = tb_camera_perspective(tb_radians(60.0f));

    open_screen(&opened, 4);
    cube = tb_mesh_create_shape(opened.renderer, TB_MESH_CUBE, 0);
    base = tb_texture_create(opened.renderer, &(tb_texture_desc){.width = 8, .height = 8}, pixels,
                             size);
    normal = tb_texture_create(
        opened.renderer, &(tb_texture_desc){.width = 8, .height = 8, .content = TB_TEXTURE_NORMAL},
        pixels, size);
    surface = tb_texture_create(
        opened.renderer, &(tb_texture_desc){.width = 8, .height = 8, .content = TB_TEXTURE_DATA},
        pixels, size);
    plain = tb_material_create(opened.renderer, &desc);
    desc.base_color = base;
    desc.normal = normal;
    desc.metallic_roughness = surface;
    desc.metallic = 1.0f;
    desc.unlit = true;
    desc.name = "textured";
    textured = tb_material_create(opened.renderer, &desc);
    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 1, 4}, (tb_vec3){0, 0, 0}));
    for (int frame = 0; frame < 3; frame++)
    {
        if (frame == 1)
        {
            tb_renderer_set_anisotropy(opened.renderer, 16);
        }
        TB_TEST_REQUIRE(tb_renderer_begin_frame(opened.renderer, &camera, (tb_vec4){0, 0, 0, 1}));
        tb_renderer_draw_mesh_material(opened.renderer, cube, tb_mat4_identity(), plain);
        tb_renderer_draw_mesh(opened.renderer, cube, tb_mat4_translation((tb_vec3){2, 0, 0}),
                              (tb_vec4){1, 0, 0, 1});
        tb_renderer_draw_mesh_material(opened.renderer, cube,
                                       tb_mat4_translation((tb_vec3){-2, 0, 0}), textured);
        TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
    }
    tb_material_destroy(opened.renderer, textured);
    tb_material_destroy(opened.renderer, plain);
    tb_material_destroy(opened.renderer, NULL);
    tb_texture_destroy(opened.renderer, surface);
    tb_texture_destroy(opened.renderer, normal);
    tb_texture_destroy(opened.renderer, base);
    tb_mesh_destroy(opened.renderer, cube);
    tb_free(pixels);
    close_screen(&opened);
}

/* The setting's choices are the renderer's anisotropy: off is 1, and each
 * choice after doubles it. */
TB_TEST(material_settings, anisotropic_filtering_comes_from_the_setting)
{
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};

    tb_engine_settings_add(settings, NULL, &engine);
    TB_TEST_EXPECT_UINT(tb_engine_settings_anisotropy(&engine), ==, 8);
    TB_TEST_REQUIRE_UINT(tb_setting_set_choice(engine.graphics.anisotropic_filtering, 0), ==,
                         TB_SETTING_OK);
    TB_TEST_EXPECT_UINT(tb_engine_settings_anisotropy(&engine), ==, 1);
    TB_TEST_REQUIRE_UINT(tb_setting_set_choice(engine.graphics.anisotropic_filtering, 4), ==,
                         TB_SETTING_OK);
    TB_TEST_EXPECT_UINT(tb_engine_settings_anisotropy(&engine), ==, 16);
    tb_settings_destroy(settings);
}
