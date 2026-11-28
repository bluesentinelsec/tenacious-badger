#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/hash.h"
#include "badger/material.h"
#include "badger/shader_library.h"
#include "badger/vfs.h"
#include "badger/window.h"
#include "render/renderer_internal.h"
#include "tb_test.h"

static tb_rhi_result reject_upload(tb_renderer_upload_target target, tb_renderer_source *source,
                                   tb_rhi_error *error)
{
    (void)target;
    (void)source;
    *error = (tb_rhi_error){.result = TB_RHI_OUT_OF_MEMORY, .message = "injected upload failure"};
    return error->result;
}

static void capture_sprite(tb_renderer *renderer, tb_image *image, uint64_t *hash)
{
    const tb_camera camera = tb_camera_orthographic(1);
    tb_renderer_capture_next_frame(renderer);
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    tb_canvas_draw_sprite(
        renderer, &(tb_canvas_sprite){.image = image, .size = {32, 32}, .tint = {1, 1, 1, 1}});
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
    uint32_t width = 0, height = 0;
    const uint8_t *const pixels = tb_renderer_captured(renderer, &width, &height);
    TB_TEST_REQUIRE_POINTER(pixels, !=, NULL);
    TB_TEST_REQUIRE_UINT(width, >=, 32);
    TB_TEST_REQUIRE_UINT(height, >=, 32);
    const uint8_t green[] = {0, 255, 0, 255};
    TB_TEST_EXPECT_BYTES(pixels + ((size_t)width * 24 + 24) * 4, green, sizeof(green));
    *hash = tb_hash_bytes(pixels, (size_t)width * height * 4);
}

static void check_uploads(tb_rhi_device *device, tb_rhi_texture image, tb_rhi_texture cube,
                          uint32_t mip, tb_rhi_buffer vertices, tb_rhi_buffer indices)
{
    tb_rhi_buffer readback = {0};
    const tb_rhi_buffer_desc desc = {.memory = TB_RHI_MEMORY_READBACK, .size = 2048};
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(device, &desc, &readback, NULL), ==, TB_RHI_OK);
    tb_rhi_command_list *const list = tb_rhi_command_list_begin(device);
    const tb_rhi_barrier barriers[] = {
        {.texture = image, .before = TB_RHI_STATE_SHADER_READ, .after = TB_RHI_STATE_COPY_SOURCE},
        {.texture = cube, .before = TB_RHI_STATE_SHADER_READ, .after = TB_RHI_STATE_COPY_SOURCE},
        {.buffer = vertices,
         .before = TB_RHI_STATE_VERTEX_INDEX,
         .after = TB_RHI_STATE_COPY_SOURCE},
        {.buffer = indices,
         .before = TB_RHI_STATE_VERTEX_INDEX,
         .after = TB_RHI_STATE_COPY_SOURCE}};
    tb_rhi_cmd_barriers(list, barriers, TB_TEST_COUNT_OF(barriers));
    tb_rhi_cmd_copy_texture_to_buffer(
        list, &(tb_rhi_texture_copy){.texture = image, .buffer = readback, .row_pitch = 256});
    tb_rhi_cmd_copy_texture_to_buffer(list, &(tb_rhi_texture_copy){.texture = cube,
                                                                   .layer = 5,
                                                                   .mip = mip,
                                                                   .buffer = readback,
                                                                   .offset = 512,
                                                                   .row_pitch = 256});
    tb_rhi_cmd_copy_buffer(list, readback, 1024, vertices, 0, sizeof(tb_mesh_vertex));
    tb_rhi_cmd_copy_buffer(list, readback, 1280, indices, 0, 3 * sizeof(uint32_t));
    TB_TEST_REQUIRE_UINT(tb_rhi_wait(device, tb_rhi_submit(device, &list, 1)), ==, TB_RHI_OK);
    uint8_t rgba[4] = {0};
    tb_rhi_buffer_read(device, readback, 260, rgba, sizeof(rgba));
    const uint8_t green[] = {0, 255, 0, 255};
    TB_TEST_EXPECT_BYTES(rgba, green, sizeof(green));
    tb_rhi_buffer_read(device, readback, 512, rgba, sizeof(rgba));
    const uint8_t last_face[] = {51, 119, 187, 255};
    TB_TEST_EXPECT_BYTES(rgba, last_face, sizeof(last_face));
    tb_mesh_vertex observed = {0};
    tb_rhi_buffer_read(device, readback, 1024, &observed, sizeof(observed));
    TB_TEST_EXPECT(observed.position.x == 7.0f);
    uint32_t triangle[3] = {0};
    tb_rhi_buffer_read(device, readback, 1280, triangle, sizeof(triangle));
    const uint32_t expected[] = {2, 0, 1};
    TB_TEST_EXPECT_BYTES(triangle, expected, sizeof(expected));
}

TB_TEST(renderer_staging, a_failed_upload_preserves_the_frame_and_retry_restores_scene_bytes)
{
    tb_window_system *system = NULL;
    if (tb_window_system_open(NULL, &system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no window system");
    }
    tb_vfs *const vfs = tb_vfs_create(NULL);
    TB_TEST_REQUIRE_UINT(tb_vfs_mount_directory(vfs, "", 0, TB_TEST_SHADER_DIRECTORY, 0), ==,
                         TB_VFS_OK);
    tb_window *window = NULL;
    TB_TEST_REQUIRE_UINT(
        tb_window_create(system, &(tb_window_options){.width = 80, .height = 60}, &window, NULL),
        ==, TB_WINDOW_OK);
    const tb_renderer_options options = {.window = window,
                                         .shader_vfs = vfs,
                                         .shader_library_only = true,
                                         .device = {.backend = TB_TEST_SOURCE_BACKEND,
                                                    .adapter = TB_TEST_ADAPTER,
                                                    .validation = true}};
    tb_renderer *renderer = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    const tb_rhi_result opened = tb_renderer_create(&options, &renderer, &error);
    if (opened == TB_RHI_UNAVAILABLE)
    {
        tb_window_destroy(window);
        tb_vfs_destroy(vfs);
        tb_window_system_close(system);
        TB_TEST_SKIP("%s", error.message);
    }
    TB_TEST_REQUIRE_UINT(opened, ==, TB_RHI_OK);
    tb_renderer_quality quality = TB_RENDERER_QUALITY_INIT;
    quality.temporal_aa = false;
    tb_renderer_set_quality(renderer, &quality);
    tb_image *const image = tb_image_create(
        renderer, &(tb_image_desc){.width = 2, .height = 2, .filter = TB_IMAGE_NEAREST}, NULL, 0);
    const tb_rhi_texture image_handle =
        TB_LIST_ENTRY(tb_list_last(&renderer->sources), tb_renderer_source, link)->texture;
    uint8_t pixel[] = {0, 255, 0, 255};
    tb_image_update(renderer, image, (tb_rhi_rect){1, 1, 1, 1}, pixel, sizeof(pixel));
    pixel[1] = 0;
    uint8_t faces[6][4][4] = {0};
    for (unsigned i = 0; i < 4; i++)
    {
        faces[5][i][0] = 51;
        faces[5][i][1] = 119;
        faces[5][i][2] = 187;
        faces[5][i][3] = 255;
    }
    tb_texture *const cube = tb_texture_create_cube(
        renderer, &(tb_texture_desc){.width = 2, .height = 2, .content = TB_TEXTURE_DATA},
        &faces[0][0][0], sizeof(faces));
    const tb_rhi_texture cube_handle =
        TB_LIST_ENTRY(tb_list_last(&renderer->sources), tb_renderer_source, link)->texture;
    tb_rhi_texture_desc cube_desc = {0};
    tb_rhi_texture_desc_of(renderer->device, cube_handle, &cube_desc);
    tb_mesh_vertex mesh_data[3] = {0};
    mesh_data[0].position.x = 7;
    uint32_t indices[] = {2, 0, 1};
    tb_mesh *const mesh = tb_mesh_create(renderer, mesh_data, 3, indices, 3);
    mesh_data[0].position.x = 0;
    indices[0] = 0;
    tb_shader_library *library = NULL;
    char name[64] = {0};
    const size_t length = tb_fmt_buffer(name, sizeof(name), "Shaders/%s.tbsl",
                                        tb_rhi_backend_name(TB_TEST_DESTINATION_BACKEND));
    TB_TEST_REQUIRE_UINT(tb_shader_library_load(vfs, name, length, &library), ==,
                         TB_SHADER_LIBRARY_OK);
    const tb_rhi_device_options destination = {
        .backend = TB_TEST_DESTINATION_BACKEND, .adapter = TB_TEST_ADAPTER, .validation = true};
    tb_rhi_device *staged = NULL;
    uint64_t before = 0, after = 0;
    capture_sprite(renderer, image, &before);
    tb_renderer_source failing = {.upload = reject_upload};
    tb_list_push_back(&renderer->sources, &failing.link);
    TB_TEST_REQUIRE_UINT(
        tb_renderer_stage_resources(renderer, &destination, library, &staged, &error), ==,
        TB_RHI_OUT_OF_MEMORY);
    TB_TEST_EXPECT_STRING(error.message, ==, "injected upload failure");
    TB_TEST_EXPECT_POINTER(staged, ==, NULL);
    tb_list_remove(&failing.link);
    capture_sprite(renderer, image, &after);
    TB_TEST_EXPECT_UINT(after, ==, before);
    TB_TEST_REQUIRE_UINT(
        tb_renderer_stage_resources(renderer, &destination, library, &staged, &error), ==,
        TB_RHI_OK);
    const tb_rhi_buffer vertex_handle = mesh->vertices, index_handle = mesh->indices;
    TB_TEST_EXPECT_UINT(tb_rhi_texture_index(staged, cube_handle), ==,
                        tb_texture_bindless_index(cube));
    TB_TEST_EXPECT_UINT(tb_rhi_sampler_index(staged, renderer->material_sampler), ==,
                        tb_rhi_sampler_index(renderer->device, renderer->material_sampler));
    tb_mesh_destroy(renderer, mesh);
    tb_texture_destroy(renderer, cube);
    tb_image_destroy(renderer, image);
    tb_renderer_destroy(renderer);
    tb_shader_library_close(library);
    tb_vfs_destroy(vfs);
    tb_window_destroy(window);
    tb_window_system_close(system);
    check_uploads(staged, image_handle, cube_handle, cube_desc.mips - 1, vertex_handle,
                  index_handle);
    tb_rhi_device_destroy(staged);
}
