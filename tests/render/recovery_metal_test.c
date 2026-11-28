#include "badger/canvas.h"
#include "badger/window.h"
#include "render/renderer_internal.h"
#include "tb_test.h"

static tb_rhi_result fail_upload(tb_renderer *renderer, tb_renderer_source *source,
                                 tb_rhi_error *error)
{
    (void)renderer;
    (void)source;
    *error = (tb_rhi_error){.result = TB_RHI_OUT_OF_MEMORY,
                            .message = "injected staging allocation failure"};
    return error->result;
}

TB_TEST(renderer_metal, recovery_keeps_canvas_edits_and_restarts_temporal_history)
{
    tb_window_system *system = NULL;
    if (tb_window_system_open(NULL, &system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    tb_window *window = NULL;
    TB_TEST_REQUIRE_UINT(
        tb_window_create(system, &(tb_window_options){.width = 80, .height = 60}, &window, NULL),
        ==, TB_WINDOW_OK);
    tb_renderer *renderer = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    const tb_renderer_options options = {.window = window,
                                         .profile = TB_RENDERER_PROFILE_FULL,
                                         .device = {.backend = TB_RHI_BACKEND_METAL}};
    const tb_rhi_result opened = tb_renderer_create(&options, &renderer, &error);
    if (opened == TB_RHI_UNAVAILABLE)
    {
        tb_window_destroy(window);
        tb_window_system_close(system);
        TB_TEST_SKIP("%s", error.message);
    }
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(opened, ==, TB_RHI_OK);
    tb_image *const image = tb_image_create(
        renderer, &(tb_image_desc){.width = 2, .height = 2, .filter = TB_IMAGE_NEAREST}, NULL, 0);
    uint8_t patch[4] = {0, 255, 0, 255};
    tb_image_update(renderer, image, (tb_rhi_rect){1, 1, 1, 1}, patch, sizeof(patch));
    patch[1] = 0; /* the caller's pixels can now be reused */
    const tb_camera camera = tb_camera_orthographic(1);
    for (unsigned frame = 0; frame < 2; frame++)
    {
        if (frame == 1)
        {
            TB_TEST_EXPECT(renderer->history_valid);
            tb_rhi_device_report_loss(renderer->device);
            TB_TEST_EXPECT_UINT(tb_rhi_swap_chain_present(renderer->chain), ==, TB_RHI_DEVICE_LOST);
            /* Fail after the real image has been uploaded, then retry the entire recovery. */
            tb_renderer_source failing = {.upload = fail_upload};
            tb_list_push_back(&renderer->sources, &failing.link);
            TB_TEST_EXPECT_UINT(tb_renderer_recover_device(renderer, &error), ==,
                                TB_RHI_OUT_OF_MEMORY);
            TB_TEST_EXPECT_UINT(tb_rhi_wait_idle(renderer->device), ==, TB_RHI_DEVICE_LOST);
            tb_list_remove(&failing.link);
            TB_TEST_REQUIRE_UINT(tb_renderer_recover_device(renderer, &error), ==, TB_RHI_OK);
            TB_TEST_EXPECT(!renderer->history_valid);
            TB_TEST_EXPECT_UINT(renderer->jitter_frame, ==, 0);
        }
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
    }
    tb_image_destroy(renderer, image);
    tb_renderer_destroy(renderer);
    tb_window_destroy(window);
    tb_window_system_close(system);
}
