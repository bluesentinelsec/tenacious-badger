#include "badger/arena.h"
#include "badger/shader.h"
#include "badger/str.h"
#include "rhi/metal/metal_internal.h"
#include "tb_test.h"
#include "test_data.h"

extern const tb_test_file tb_rhi_hlsl_files[];
extern const size_t tb_rhi_hlsl_files_count;

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
