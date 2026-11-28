#include "badger/canvas.h"
#include "badger/rhi_shader_library.h"
#include "badger/vfs.h"
#include "badger/window.h"
#include "render/renderer_internal.h"
#include "tb_test.h"

static void expect_green_frame(tb_renderer *renderer)
{
    const tb_camera camera = tb_camera_orthographic(1);
    tb_renderer_capture_next_frame(renderer);
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    tb_canvas_fill_rect(renderer, (tb_canvas_rect){0, 0, 32, 32}, (tb_vec4){0, 1, 0, 1});
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
    uint32_t width = 0, height = 0;
    const uint8_t *const pixels = tb_renderer_captured(renderer, &width, &height);
    TB_TEST_REQUIRE_POINTER(pixels, !=, NULL);
    TB_TEST_REQUIRE_UINT(width, >=, 32);
    TB_TEST_REQUIRE_UINT(height, >=, 32);
    const uint8_t green[] = {0, 255, 0, 255};
    TB_TEST_EXPECT_BYTES(pixels + ((size_t)width * 16 + 16) * 4, green, sizeof(green));
}

static void transfer_canvas(tb_renderer *source, tb_renderer *destination)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_shader_library_writer *const writer = tb_shader_library_writer_create();
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_library *empty = NULL;
    const void *bytes = NULL;
    size_t size = 0;
    TB_TEST_REQUIRE(tb_shader_library_writer_finish(writer, "empty", arena, &bytes, &size));
    TB_TEST_REQUIRE_UINT(tb_shader_library_open(bytes, size, &empty), ==, TB_SHADER_LIBRARY_OK);
    tb_arena_destroy(arena);
    tb_shader_library_writer_destroy(writer);
    tb_rhi_pipeline transferred = {0};
    TB_TEST_EXPECT_UINT(tb_rhi_pipeline_create_from_library(source->device, source->canvas_pipeline,
                                                            destination->device, empty,
                                                            &transferred, &error),
                        ==, TB_RHI_UNAVAILABLE);
    tb_shader_library_close(empty);
    expect_green_frame(source);

    TB_TEST_REQUIRE_UINT(tb_rhi_pipeline_create_from_library(
                             source->device, source->canvas_pipeline, destination->device,
                             destination->shader_library, &transferred, &error),
                         ==, TB_RHI_OK);
    tb_rhi_pipeline_destroy(destination->device, destination->canvas_pipeline);
    destination->canvas_pipeline = transferred;
}

TB_TEST(renderer_library, a_cooked_canvas_draws_after_releasing_the_source_renderer)
{
    tb_window_system *system = NULL;
    if (tb_window_system_open(NULL, &system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no window system");
    }
    tb_vfs *const vfs = tb_vfs_create(NULL);
    TB_TEST_REQUIRE_UINT(tb_vfs_mount_directory(vfs, "", 0, TB_TEST_SHADER_DIRECTORY, 0), ==,
                         TB_VFS_OK);
    tb_window *windows[2] = {0};
    tb_renderer *renderers[2] = {0};
    const tb_rhi_backend backends[] = {TB_TEST_SOURCE_BACKEND, TB_TEST_DESTINATION_BACKEND};
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    bool unavailable = false;
    for (size_t i = 0; i < 2; i++)
    {
        TB_TEST_REQUIRE_UINT(tb_window_create(system,
                                              &(tb_window_options){.width = 80, .height = 60},
                                              &windows[i], NULL),
                             ==, TB_WINDOW_OK);
        const tb_renderer_options options = {
            .window = windows[i],
            .shader_vfs = vfs,
            .shader_library_only = true,
            .device = {.backend = backends[i], .adapter = TB_TEST_ADAPTER, .validation = true}};
        const tb_rhi_result opened = tb_renderer_create(&options, &renderers[i], &error);
        if (opened == TB_RHI_UNAVAILABLE)
        {
            unavailable = true;
            goto done;
        }
        TB_TEST_CONTEXT("%s", error.message);
        TB_TEST_REQUIRE_UINT(opened, ==, TB_RHI_OK);
        if ((renderers[i]->info.features & TB_RHI_FEATURE_HEAP_INDICES) == 0)
        {
            unavailable = true;
            goto done;
        }
        TB_TEST_REQUIRE_UINT(renderers[i]->canvas_pipeline.generation, !=, 0);
    }
    transfer_canvas(renderers[0], renderers[1]);
    tb_renderer_destroy(renderers[0]);
    renderers[0] = NULL;
    expect_green_frame(renderers[1]);
done:
    for (size_t i = 0; i < 2; i++)
    {
        tb_renderer_destroy(renderers[i]);
        tb_window_destroy(windows[i]);
    }
    tb_vfs_destroy(vfs);
    tb_window_system_close(system);
    if (unavailable)
    {
        TB_TEST_SKIP("two devices with indexed canvas resources required: %s", error.message);
    }
}
