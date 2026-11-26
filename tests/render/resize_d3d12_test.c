/* Live resizing on D3D12, on WARP: while the player drags a window's border,
 * the renderer resizes its swap chain at nearly every frame. A swap chain
 * resizes many times in a row and presents at each size, also into a window
 * larger than its images, which it shows unscaled; and the renderer follows
 * a window through many sizes, with its viewport recomputed for each,
 * renders nothing while it is minimized, and renders again once it is
 * restored, without losing its device. */
#include "badger/renderer.h"
#include "badger/rhi.h"
#include "badger/viewport.h"
#include "badger/window.h"

#include "tb_test.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The sizes a drag passes through, in a row. */
#define RESIZES 40u

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
} screen;

static void open_screen(screen *opened)
{
    *opened = (screen){0};
    if (tb_window_system_open(NULL, &opened->system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(opened->system,
                         &(tb_window_options){.title = "resize", .width = 320, .height = 180},
                         &opened->window, NULL),
        ==, TB_WINDOW_OK);
}

static void close_screen(screen *opened)
{
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

/* Read what the window system queued, so that the window's state is now. */
static void drain(const screen *opened)
{
    tb_window_event event = {0};

    while (tb_window_system_poll(opened->system, &event))
    {
    }
}

/* The size a drag reaches at a step: wider and taller, then narrower and
 * shorter, as a hand moves. */
static void dragged_size(uint32_t step, uint32_t *width, uint32_t *height)
{
    *width = 160u + (step * 37u) % 360u;
    *height = 120u + (step * 23u) % 240u;
}

/* Clear the swap chain's current image and present it. */
static void present_cleared(tb_rhi_device *device, tb_rhi_swap_chain *chain)
{
    tb_rhi_command_list *const list = tb_rhi_command_list_begin(device);
    const tb_rhi_texture image = tb_rhi_swap_chain_current(chain);
    tb_rhi_pass pass = {.color_count = 1};

    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = image,
                                               .before = TB_RHI_STATE_PRESENT,
                                               .after = TB_RHI_STATE_RENDER_TARGET});
    pass.colors[0] = (tb_rhi_color_target){
        .texture = image, .load = TB_RHI_LOAD_CLEAR, .clear = {0.2f, 0.4f, 0.6f, 1.0f}};
    tb_rhi_cmd_begin_pass(list, &pass);
    tb_rhi_cmd_end_pass(list);
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = image,
                                               .before = TB_RHI_STATE_RENDER_TARGET,
                                               .after = TB_RHI_STATE_PRESENT});
    (void)tb_rhi_submit(device, &list, 1);
    TB_TEST_EXPECT_UINT(tb_rhi_swap_chain_present(chain), ==, TB_RHI_OK);
}

/* A swap chain resized at every frame of a drag presents at each size, and
 * one smaller than its window presents too: its images are unscaled. */
TB_TEST(rhi_d3d12, a_swap_chain_resizes_many_times_in_a_row)
{
    screen opened = {0};
    tb_rhi_device *device = NULL;
    tb_rhi_swap_chain *chain = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_rhi_texture_desc desc = {0};

    open_screen(&opened);
    if (tb_rhi_device_create(&(tb_rhi_device_options){.backend = TB_RHI_BACKEND_D3D12,
                                                      .adapter = TB_RHI_ADAPTER_SOFTWARE,
                                                      .validation = true},
                             &device, &error) != TB_RHI_OK)
    {
        close_screen(&opened);
        TB_TEST_SKIP("no WARP device: %s", error.message);
    }
    TB_TEST_REQUIRE_UINT(tb_rhi_swap_chain_create(device, opened.window,
                                                  &(tb_rhi_swap_chain_desc){.vsync = false}, &chain,
                                                  &error),
                         ==, TB_RHI_OK);
    for (uint32_t step = 0; step < RESIZES; step++)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        TB_TEST_CONTEXT("step %u", step);
        dragged_size(step, &width, &height);
        TB_TEST_REQUIRE_UINT(tb_rhi_swap_chain_resize(chain, width, height, false, &error), ==,
                             TB_RHI_OK);
        tb_rhi_texture_desc_of(device, tb_rhi_swap_chain_current(chain), &desc);
        TB_TEST_EXPECT_UINT(desc.width, ==, width);
        TB_TEST_EXPECT_UINT(desc.height, ==, height);
        present_cleared(device, chain);
    }
    /* The window grows before its images do, as while the player drags. */
    TB_TEST_REQUIRE_UINT(tb_rhi_swap_chain_resize(chain, 100, 80, false, &error), ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_window_configure(opened.window, &(tb_window_options){.width = 480, .height = 270}, NULL),
        ==, TB_WINDOW_OK);
    drain(&opened);
    present_cleared(device, chain);
    TB_TEST_EXPECT_UINT(tb_rhi_wait_idle(device), ==, TB_RHI_OK);
    tb_rhi_swap_chain_destroy(chain);
    tb_rhi_device_destroy(device);
    close_screen(&opened);
}

/* Begin and end a frame, saying whether it began. */
static bool render_frame(tb_renderer *renderer)
{
    tb_camera camera = tb_camera_perspective(tb_radians(60.0f));

    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 0, 3}, (tb_vec3){0, 0, 0}));
    if (!tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}))
    {
        return false;
    }
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
    return true;
}

/* The policy's viewport of a frame of the window's size, as the renderer
 * must have it for the frame. */
static tb_viewport expected_viewport(const tb_viewport_policy *policy, const tb_window_state *state)
{
    return tb_viewport_compute(policy,
                               &(tb_viewport_frame){.width = state->pixel_width,
                                                    .height = state->pixel_height,
                                                    .display_scale = (float)state->content_scale});
}

/* The renderer follows a window resized at every frame, with the viewport
 * of each new size, as the default 16:9 design expanded has it; renders nothing
 * while the window is minimized; and renders at its size again once
 * restored. */
TB_TEST(renderer_frame, follows_a_window_through_many_sizes_on_d3d12)
{
    screen opened = {0};
    tb_renderer *renderer = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};
    tb_window_state state = {0};
    tb_window_native native = {0};
    const tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
    tb_viewport viewport = {0};
    tb_viewport expected = {0};
    uint32_t width = 0;
    uint32_t height = 0;

    open_screen(&opened);
    options.window = opened.window;
    options.device = (tb_rhi_device_options){
        .backend = TB_RHI_BACKEND_D3D12, .adapter = TB_RHI_ADAPTER_SOFTWARE, .validation = true};
    if (tb_renderer_create(&options, &renderer, &error) != TB_RHI_OK)
    {
        close_screen(&opened);
        TB_TEST_SKIP("no renderer: %s", error.message);
    }
    tb_renderer_set_viewport_policy(renderer, &policy, 0.0f);
    for (uint32_t step = 0; step < RESIZES; step++)
    {
        TB_TEST_CONTEXT("step %u", step);
        dragged_size(step, &width, &height);
        TB_TEST_REQUIRE_UINT(
            tb_window_configure(opened.window,
                                &(tb_window_options){.width = width, .height = height}, NULL),
            ==, TB_WINDOW_OK);
        drain(&opened);
        tb_window_state_of(opened.window, &state);
        TB_TEST_REQUIRE(render_frame(renderer));
        tb_renderer_size(renderer, &width, &height);
        TB_TEST_EXPECT_UINT(width, ==, state.pixel_width);
        TB_TEST_EXPECT_UINT(height, ==, state.pixel_height);
        viewport = tb_renderer_viewport(renderer);
        expected = expected_viewport(&policy, &state);
        TB_TEST_EXPECT_UINT(viewport.x, ==, expected.x);
        TB_TEST_EXPECT_UINT(viewport.y, ==, expected.y);
        TB_TEST_EXPECT_UINT(viewport.width, ==, expected.width);
        TB_TEST_EXPECT_UINT(viewport.height, ==, expected.height);
        TB_TEST_EXPECT_UINT(viewport.x + viewport.width, <=, width);
        TB_TEST_EXPECT_UINT(viewport.y + viewport.height, <=, height);
    }

    tb_window_native_of(opened.window, &native);
    (void)ShowWindow((HWND)native.window, SW_MINIMIZE);
    drain(&opened);
    tb_window_state_of(opened.window, &state);
    TB_TEST_EXPECT(state.minimized);
    TB_TEST_EXPECT(!render_frame(renderer));
    (void)ShowWindow((HWND)native.window, SW_RESTORE);
    drain(&opened);
    tb_window_state_of(opened.window, &state);
    TB_TEST_EXPECT(!state.minimized);
    TB_TEST_EXPECT(render_frame(renderer));
    tb_renderer_size(renderer, &width, &height);
    TB_TEST_EXPECT_UINT(width, ==, state.pixel_width);
    TB_TEST_EXPECT_UINT(height, ==, state.pixel_height);

    tb_renderer_destroy(renderer);
    close_screen(&opened);
}
