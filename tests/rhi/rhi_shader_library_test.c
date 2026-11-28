#include "badger/rhi_shader_library.h"
#include "badger/str.h"
#include "rhi/rhi_internal.h"
#include "tb_test.h"

static tb_rhi_device *open_device(tb_rhi_backend_table *table, tb_rhi_backend backend)
{
    tb_rhi_device *device = NULL;
    *table = tb_rhi_null_backend;
    table->backend = backend;
    TB_TEST_REQUIRE_UINT(tb_rhi_device_create_on(table, &(tb_rhi_device_options){0}, &device, NULL),
                         ==, TB_RHI_OK);
    return device;
}

static tb_shader_compile_request shader_request(tb_shader_stage stage, const tb_shader_define *mode)
{
    static const char text[] = "uint mode = MODE;\n";
    static const tb_shader_file file = {"/Game/Variant.hlsl", text, sizeof(text) - 1};
    return (tb_shader_compile_request){.source = {.path = file.path,
                                                  .files = &file,
                                                  .file_count = 1,
                                                  .defines = mode,
                                                  .define_count = 1},
                                       .entry_point = "main",
                                       .stage = stage};
}

static void cook(tb_shader_library_writer *writer, tb_shader_compile_request request,
                 tb_shader_target target, uint8_t byte)
{
    tb_shader_key key = {0};
    request.target = target;
    TB_TEST_REQUIRE_UINT(tb_shader_key_make(&request, &key), ==, TB_SHADER_OK);
    TB_TEST_REQUIRE(tb_shader_library_writer_add(writer, &key, &byte, sizeof(byte)));
}

static tb_shader_library *open_library(tb_shader_library_writer *writer)
{
    tb_arena *const arena = tb_arena_create(0);
    const void *bytes = NULL;
    size_t size = 0;
    tb_shader_library *library = NULL;
    TB_TEST_REQUIRE(tb_shader_library_writer_finish(writer, "fixture", arena, &bytes, &size));
    TB_TEST_REQUIRE_UINT(tb_shader_library_open(bytes, size, &library), ==, TB_SHADER_LIBRARY_OK);
    tb_arena_destroy(arena);
    return library;
}

static tb_rhi_shader make_shader(tb_rhi_device *device, const tb_shader_compile_request *request)
{
    const uint8_t code = 7;
    tb_rhi_shader shader = {0};
    TB_TEST_REQUIRE_UINT(tb_rhi_shader_create_compiled(device, request, &code, 1, &shader, NULL),
                         ==, TB_RHI_OK);
    return shader;
}

static tb_rhi_result reject_pixel(void *state, const tb_rhi_shader_desc *desc, void **object,
                                  tb_rhi_error *error)
{
    if (desc->stage == TB_RHI_STAGE_PIXEL)
    {
        return tb_rhi_fail(error, TB_RHI_SYSTEM_ERROR, "injected pixel shader failure");
    }
    return tb_rhi_null_backend.shader_create(state, desc, object, error);
}

TB_TEST(rhi_shader_library, graphics_selects_the_cooked_permutation_after_original_handles_expire)
{
    tb_rhi_backend_table source_table = {0}, target_table = {0};
    tb_rhi_device *const source = open_device(&source_table, TB_RHI_BACKEND_NULL);
    tb_rhi_device *const target = open_device(&target_table, TB_RHI_BACKEND_METAL);
    tb_shader_define mode = {"MODE", "2"};
    const tb_shader_compile_request vertex = shader_request(TB_SHADER_STAGE_VERTEX, &mode);
    const tb_shader_compile_request pixel = shader_request(TB_SHADER_STAGE_PIXEL, &mode);
    const tb_rhi_graphics_pipeline_desc graphics = {.vertex = make_shader(source, &vertex),
                                                    .pixel = make_shader(source, &pixel),
                                                    .depth_write = true};
    tb_rhi_pipeline pipeline = {0}, made = {0};
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    TB_TEST_REQUIRE_UINT(tb_rhi_graphics_pipeline_create(source, &graphics, &pipeline, NULL), ==,
                         TB_RHI_OK);
    tb_rhi_shader_destroy(source, graphics.vertex);
    tb_rhi_shader_destroy(source, graphics.pixel);

    tb_shader_library_writer *const writer = tb_shader_library_writer_create();
    cook(writer, vertex, TB_SHADER_TARGET_MSL, 11);
    mode.value = "1";
    cook(writer, pixel, TB_SHADER_TARGET_MSL, 13);
    tb_shader_library *library = open_library(writer);
    TB_TEST_EXPECT_UINT(
        tb_rhi_pipeline_create_from_library(source, pipeline, target, library, &made, &error), ==,
        TB_RHI_UNAVAILABLE);
    TB_TEST_EXPECT_UINT(made.generation, ==, 0);
    TB_TEST_EXPECT_UINT(
        tb_str_find(error.message, 0, TB_STR_END, "/Game/Variant.hlsl main (pixel)"), !=,
        TB_STR_NOT_FOUND);
    tb_shader_library_close(library);

    mode.value = "2";
    cook(writer, pixel, TB_SHADER_TARGET_MSL, 17);
    library = open_library(writer);
    tb_shader_library_writer_destroy(writer);
    mode.value = NULL;
    target_table.shader_create = reject_pixel;
    TB_TEST_EXPECT_UINT(
        tb_rhi_pipeline_create_from_library(source, pipeline, target, library, &made, &error), ==,
        TB_RHI_SYSTEM_ERROR);
    TB_TEST_EXPECT_UINT(made.generation, ==, 0);
    target_table.shader_create = tb_rhi_null_backend.shader_create;
    TB_TEST_REQUIRE_UINT(
        tb_rhi_pipeline_create_from_library(source, pipeline, target, library, &made, &error), ==,
        TB_RHI_OK);
    tb_shader_library_close(library);
    tb_rhi_device_destroy(source);
    const tb_rhi_pipeline_data *const kept =
        tb_rhi_slot_of(target, TB_RHI_KIND_PIPELINE, made.index, made.generation)->desc.pipeline;
    TB_TEST_EXPECT(kept->graphics.depth_write);
    TB_TEST_EXPECT_BYTES(kept->first->desc.code, &(uint8_t){11}, 1);
    TB_TEST_EXPECT_BYTES(kept->pixel->desc.code, &(uint8_t){17}, 1);
    tb_rhi_device_destroy(target);
}

TB_TEST(rhi_shader_library, a_compute_pipeline_keeps_its_identity_across_two_transfers)
{
    tb_rhi_backend_table first = {0}, second = {0}, third = {0};
    tb_rhi_device *const source = open_device(&first, TB_RHI_BACKEND_NULL);
    tb_rhi_device *const dx11 = open_device(&second, TB_RHI_BACKEND_D3D11);
    tb_rhi_device *const dx12 = open_device(&third, TB_RHI_BACKEND_D3D12);
    const tb_shader_define mode = {"MODE", "3"};
    const tb_shader_compile_request request = shader_request(TB_SHADER_STAGE_COMPUTE, &mode);
    const tb_rhi_compute_pipeline_desc desc = {.compute = make_shader(source, &request)};
    tb_rhi_pipeline original = {0}, intermediate = {0}, result = {0};
    TB_TEST_REQUIRE_UINT(tb_rhi_compute_pipeline_create(source, &desc, &original, NULL), ==,
                         TB_RHI_OK);
    tb_rhi_shader_destroy(source, desc.compute);
    tb_shader_library_writer *const writer = tb_shader_library_writer_create();
    cook(writer, request, TB_SHADER_TARGET_DXBC, 23);
    cook(writer, request, TB_SHADER_TARGET_DXIL, 29);
    tb_shader_library *const library = open_library(writer);
    tb_shader_library_writer_destroy(writer);
    TB_TEST_EXPECT_UINT(
        tb_rhi_pipeline_create_from_library(source, original, source, library, &intermediate, NULL),
        ==, TB_RHI_UNAVAILABLE);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_pipeline_create_from_library(source, original, dx11, library, &intermediate, NULL),
        ==, TB_RHI_OK);
    tb_rhi_device_destroy(source);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_pipeline_create_from_library(dx11, intermediate, dx12, library, &result, NULL), ==,
        TB_RHI_OK);
    tb_rhi_device_destroy(dx11);
    tb_shader_library_close(library);
    const tb_rhi_pipeline_data *const kept =
        tb_rhi_slot_of(dx12, TB_RHI_KIND_PIPELINE, result.index, result.generation)->desc.pipeline;
    TB_TEST_EXPECT_BYTES(kept->first->desc.code, &(uint8_t){29}, 1);
    tb_rhi_device_destroy(dx12);
}

TB_TEST(rhi_shader_library, native_shaders_and_invalid_source_requests_report_failure)
{
    tb_rhi_backend_table table = {0};
    tb_rhi_device *const device = open_device(&table, TB_RHI_BACKEND_METAL);
    const tb_shader_define mode = {"MODE", "4"};
    tb_shader_compile_request request = shader_request(TB_SHADER_STAGE_COMPUTE, &mode);
    tb_rhi_shader shader = {0};
    static const uint8_t code = 31;
    TB_TEST_EXPECT_UINT(tb_rhi_shader_create_compiled(device, &request, &code, 1, &shader, NULL),
                        ==, TB_RHI_UNAVAILABLE);
    request.target = TB_SHADER_TARGET_MSL;
    request.stage = TB_SHADER_STAGE_MESH;
    TB_TEST_EXPECT_UINT(tb_rhi_shader_create_compiled(device, &request, &code, 1, &shader, NULL),
                        ==, TB_RHI_UNAVAILABLE);
    request.stage = TB_SHADER_STAGE_COMPUTE;
    request.entry_point = NULL;
    TB_TEST_EXPECT_UINT(tb_rhi_shader_create_compiled(device, &request, &code, 1, &shader, NULL),
                        ==, TB_RHI_SYSTEM_ERROR);
    TB_TEST_EXPECT_UINT(shader.generation, ==, 0);
    const tb_rhi_shader_desc raw = {.stage = TB_RHI_STAGE_VERTEX, .code = &code, .size = 1};
    TB_TEST_REQUIRE_UINT(tb_rhi_shader_create(device, &raw, &shader, NULL), ==, TB_RHI_OK);
    tb_rhi_pipeline pipeline = {0}, made = {0};
    TB_TEST_REQUIRE_UINT(
        tb_rhi_graphics_pipeline_create(device, &(tb_rhi_graphics_pipeline_desc){.vertex = shader},
                                        &pipeline, NULL),
        ==, TB_RHI_OK);
    tb_rhi_shader_destroy(device, shader);
    tb_shader_library_writer *const writer = tb_shader_library_writer_create();
    tb_shader_library *const empty = open_library(writer);
    tb_shader_library_writer_destroy(writer);
    TB_TEST_EXPECT_UINT(
        tb_rhi_pipeline_create_from_library(device, pipeline, device, empty, &made, NULL), ==,
        TB_RHI_UNAVAILABLE);
    tb_shader_library_close(empty);
    tb_rhi_device_destroy(device);
}
