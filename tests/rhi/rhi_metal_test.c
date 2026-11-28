#include "badger/arena.h"
#include "badger/shader.h"
#include "badger/str.h"
#include "badger/window.h"
#include "rhi/metal/metal_internal.h"
#include "tb_test.h"
#include "test_data.h"

extern const tb_test_file tb_rhi_hlsl_files[];
extern const size_t tb_rhi_hlsl_files_count;

TB_TEST(rhi_metal, fixed_bindings_are_read_within_the_shader_payload)
{
    const char valid[] = "// tb_bind 1 0 4 2\n// tb_bind 6 2 64 1";
    tb_metal_bindings bindings = {0};
    tb_rhi_shader_desc shader = {.code = valid, .size = sizeof(valid) - 1};
    TB_TEST_REQUIRE(tb_metal_read_bindings(&shader, &bindings));
    TB_TEST_REQUIRE_UINT(bindings.count, ==, 2);
    TB_TEST_EXPECT_UINT(bindings.slots[0].heap, ==, TB_MSL_TEXTURES);
    TB_TEST_EXPECT_UINT(bindings.slots[0].slot, ==, 2);
    TB_TEST_EXPECT_UINT(bindings.slots[1].buffer, ==, 2);
    TB_TEST_EXPECT_UINT(bindings.slots[1].offset, ==, 64);
    const char *const invalid[] = {
        "// tb_bind 1 0 4",    "// tb_bind 1 -1 4 0", "// tb_bind 1 0 4294967296 0",
        "// tb_bind 0 0 0 16", "// tb_bind 6 0 0 16", "// tb_bind 7 0 0 1",
        "// tb_bind 1 3 0 1"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    {
        bindings = (tb_metal_bindings){0};
        shader.code = invalid[i];
        shader.size = tb_str_length(invalid[i]);
        TB_TEST_EXPECT(!tb_metal_read_bindings(&shader, &bindings));
    }
}

TB_TEST(rhi_metal, unsupported_device_requests_report_the_missing_capability)
{
    tb_rhi_device *device = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_rhi_device_options options = {.backend = TB_RHI_BACKEND_METAL,
                                     .adapter = TB_RHI_ADAPTER_SOFTWARE};
    TB_TEST_EXPECT_UINT(tb_rhi_device_create(&options, &device, &error), ==, TB_RHI_UNAVAILABLE);
    TB_TEST_EXPECT_POINTER(device, ==, NULL);
    TB_TEST_EXPECT_STRING(error.message, ==, "Metal requires a hardware GPU");
    options.adapter = TB_RHI_ADAPTER_HIGH_PERFORMANCE;
    const tb_rhi_result opened = tb_rhi_device_create(&options, &device, &error);
    if (opened == TB_RHI_UNAVAILABLE)
    {
        TB_TEST_SKIP("%s", error.message);
    }
    TB_TEST_REQUIRE_UINT(opened, ==, TB_RHI_OK);
    tb_rhi_device_destroy(device);
    options.bindless_resources = TB_MSL_RESOURCE_COUNT + 1;
    TB_TEST_EXPECT_UINT(tb_rhi_device_create(&options, &device, &error), ==, TB_RHI_UNAVAILABLE);
    TB_TEST_EXPECT_POINTER(device, ==, NULL);
    TB_TEST_EXPECT(tb_str_starts_with(error.message, "Metal argument-buffer capacities"));
}

static tb_rhi_shader triangle(tb_rhi_device *device, bool vertex)
{
    tb_shader_file source = {.path = "/Engine/Private/triangle.hlsl"};
    for (size_t i = 0; i < tb_rhi_hlsl_files_count; i++)
    {
        if (tb_str_equal("triangle.hlsl", tb_rhi_hlsl_files[i].name))
        {
            source.text = (const char *)tb_rhi_hlsl_files[i].bytes;
            source.length = tb_rhi_hlsl_files[i].length;
        }
    }
    TB_TEST_REQUIRE_POINTER(source.text, !=, NULL);
    tb_shader_compile_request request = {
        .source = {.path = source.path, .files = &source, .file_count = 1},
        .target = TB_SHADER_TARGET_MSL,
        .stage = vertex ? TB_SHADER_STAGE_VERTEX : TB_SHADER_STAGE_PIXEL,
        .entry_point = vertex ? "MainVS" : "MainPS"};
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_compiled compiled = {0};
    TB_TEST_REQUIRE_UINT(tb_shader_compile(&request, arena, &compiled), ==, TB_SHADER_OK);
    tb_rhi_shader shader = {0};
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    const tb_rhi_result result = tb_rhi_shader_create(
        device,
        &(tb_rhi_shader_desc){.stage = vertex ? TB_RHI_STAGE_VERTEX : TB_RHI_STAGE_PIXEL,
                              .code = compiled.code,
                              .size = compiled.code_size},
        &shader, &error);
    tb_arena_destroy(arena);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(result, ==, TB_RHI_OK);
    return shader;
}

TB_TEST(rhi_metal, raster_triangle_reads_each_draws_constants)
{
    tb_rhi_device *device = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    const tb_rhi_result result = tb_rhi_device_create_on(
        &tb_rhi_metal_backend, &(tb_rhi_device_options){0}, &device, &error);
    if (result == TB_RHI_UNAVAILABLE)
    {
        TB_TEST_SKIP("%s", error.message);
    }
    TB_TEST_REQUIRE_UINT(result, ==, TB_RHI_OK);
    tb_rhi_texture target = {0};
    tb_rhi_buffer readback = {0};
    const tb_rhi_format format = TB_RHI_FORMAT_R8G8B8A8_UNORM;
    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(
            device,
            &(tb_rhi_texture_desc){
                .format = format, .width = 16, .height = 16, .usage = TB_RHI_TEXTURE_RENDER_TARGET},
            &target, &error),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_buffer_create(
            device, &(tb_rhi_buffer_desc){.size = 16 * 256, .memory = TB_RHI_MEMORY_READBACK},
            &readback, &error),
        ==, TB_RHI_OK);
    tb_rhi_graphics_pipeline_desc desc = {.colors = {format}, .color_count = 1};
    desc.vertex = triangle(device, true);
    desc.pixel = triangle(device, false);
    tb_rhi_pipeline pipeline = {0};
    const tb_rhi_result built = tb_rhi_graphics_pipeline_create(device, &desc, &pipeline, &error);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(built, ==, TB_RHI_OK);
    tb_rhi_shader_destroy(device, desc.vertex);
    tb_rhi_shader_destroy(device, desc.pixel);
    const float colors[][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}};
    for (size_t frame = 0; frame < 2; frame++)
    {
        tb_rhi_command_list *const list = tb_rhi_command_list_begin(device);
        tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = target,
                                                   .before = frame == 0 ? TB_RHI_STATE_COMMON
                                                                        : TB_RHI_STATE_COPY_SOURCE,
                                                   .after = TB_RHI_STATE_RENDER_TARGET});
        tb_rhi_cmd_begin_pass(
            list, &(tb_rhi_pass){.colors = {{.texture = target, .load = TB_RHI_LOAD_CLEAR}},
                                 .color_count = 1});
        tb_rhi_cmd_viewport(list, 0, 0, 16, 16, 0, 1);
        tb_rhi_cmd_scissor(list, (tb_rhi_rect){.width = 16, .height = 16});
        tb_rhi_cmd_begin_marker(list, "triangle");
        tb_rhi_cmd_pipeline(list, pipeline);
        tb_rhi_cmd_push_constants(list, colors[frame], sizeof(colors[frame]));
        tb_rhi_cmd_draw(list, 3, 1, 0, 0);
        tb_rhi_cmd_end_marker(list);
        tb_rhi_cmd_end_pass(list);
        tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = target,
                                                   .before = TB_RHI_STATE_RENDER_TARGET,
                                                   .after = TB_RHI_STATE_COPY_SOURCE});
        tb_rhi_cmd_copy_texture_to_buffer(
            list, &(tb_rhi_texture_copy){.buffer = readback, .row_pitch = 256, .texture = target});
        uint64_t submission = tb_rhi_submit(device, &list, 1);
        TB_TEST_REQUIRE_UINT(tb_rhi_wait(device, submission), ==, TB_RHI_OK);
        for (size_t y = 0; y < 16; y++)
        {
            uint32_t row[16] = {0};
            tb_rhi_buffer_read(device, readback, y * 256, row, sizeof(row));
            for (size_t x = 0; x < 16; x++)
            {
                TB_TEST_EXPECT_HEX(row[x], ==, frame == 0 ? 0xff0000ffu : 0xff00ff00u);
            }
        }
    }
    tb_rhi_pipeline_destroy(device, pipeline);
    tb_rhi_texture_destroy(device, target);
    tb_rhi_buffer_destroy(device, readback);
    tb_rhi_device_destroy(device);
}

TB_TEST(rhi_metal, swap_chain_tracks_backing_pixels_and_presents_after_resize)
{
    tb_rhi_device *device = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    const tb_rhi_result opened = tb_rhi_device_create_on(
        &tb_rhi_metal_backend, &(tb_rhi_device_options){0}, &device, &error);
    if (opened == TB_RHI_UNAVAILABLE)
    {
        TB_TEST_SKIP("%s", error.message);
    }
    TB_TEST_REQUIRE_UINT(opened, ==, TB_RHI_OK);
    tb_window_system *system = NULL;
    tb_window_error window_error = TB_WINDOW_ERROR_INIT;
    if (tb_window_system_open(NULL, &system, &window_error) != TB_WINDOW_OK)
    {
        tb_rhi_device_destroy(device);
        TB_TEST_SKIP("%s", window_error.message);
    }
    tb_window *window = NULL;
    TB_TEST_REQUIRE_UINT(tb_window_create(system, &(tb_window_options){.width = 160, .height = 90},
                                          &window, &window_error),
                         ==, TB_WINDOW_OK);
    tb_window_state size = {0};
    tb_window_state_of(window, &size);
    tb_rhi_swap_chain *chain = NULL;
    TB_TEST_REQUIRE_UINT(tb_rhi_swap_chain_create(device, window,
                                                  &(tb_rhi_swap_chain_desc){.images = 2}, &chain,
                                                  &error),
                         ==, TB_RHI_OK);
    for (size_t frame = 0; frame < 3; frame++)
    {
        const uint32_t width = frame == 1 ? 96 : size.pixel_width;
        const uint32_t height = frame == 1 ? 64 : size.pixel_height;
        if (frame > 0)
        {
            TB_TEST_REQUIRE_UINT(tb_rhi_swap_chain_resize(chain, frame == 1 ? width : 0,
                                                          frame == 1 ? height : 0, frame == 1,
                                                          &error),
                                 ==, TB_RHI_OK);
        }
        tb_rhi_texture target = tb_rhi_swap_chain_current(chain);
        tb_rhi_texture_desc desc = {0};
        tb_rhi_texture_desc_of(device, target, &desc);
        TB_TEST_EXPECT_UINT(desc.width, ==, width);
        TB_TEST_EXPECT_UINT(desc.height, ==, height);
        tb_rhi_command_list *const list = tb_rhi_command_list_begin(device);
        tb_rhi_cmd_begin_pass(list, &(tb_rhi_pass){.color_count = 1,
                                                   .colors = {{.texture = target,
                                                               .load = TB_RHI_LOAD_CLEAR,
                                                               .clear = {0, 1, 0, 1}}}});
        tb_rhi_cmd_end_pass(list);
        (void)tb_rhi_submit(device, &list, 1);
        TB_TEST_REQUIRE_UINT(tb_rhi_swap_chain_present(chain), ==, TB_RHI_OK);
        TB_TEST_EXPECT_UINT(tb_rhi_swap_chain_current(chain).index, !=, target.index);
    }
    tb_rhi_swap_chain_destroy(chain);
    tb_window_destroy(window);
    tb_window_system_close(system);
    tb_rhi_device_destroy(device);
}
