#include "badger/arena.h"
#include "badger/shader.h"
#include "badger/str.h"
#include "tb_test.h"
#include "test_data.h"

#include <string.h>

extern const tb_test_file tb_rhi_hlsl_files[];
extern const size_t tb_rhi_hlsl_files_count;

static tb_shader_result compile_stage(const tb_shader_file *file, const char *entry,
                                      tb_shader_stage stage, tb_arena *arena,
                                      tb_shader_compiled *compiled)
{
    const tb_shader_compile_request request = {
        .source = {.path = file->path, .files = file, .file_count = 1},
        .entry_point = entry,
        .stage = stage,
        .target = TB_SHADER_TARGET_MSL};
    return tb_shader_compile(&request, arena, compiled);
}

TB_TEST(shader_msl, triangle_uses_vertex_id_and_draw_constants)
{
    const char *const entries[] = {"MainVS", "MainPS"};
    const tb_shader_stage stages[] = {TB_SHADER_STAGE_VERTEX, TB_SHADER_STAGE_PIXEL};
    const char *const attributes[] = {"[[vertex_id]]", "[[buffer(0)]]"};
    tb_shader_file source = {.path = "/Engine/Private/triangle.hlsl"};
    for (size_t index = 0; index < tb_rhi_hlsl_files_count; index++)
    {
        if (tb_str_equal(tb_rhi_hlsl_files[index].name, "triangle.hlsl"))
        {
            source.text = (const char *)tb_rhi_hlsl_files[index].bytes;
            source.length = tb_rhi_hlsl_files[index].length;
            break;
        }
    }
    TB_TEST_REQUIRE_POINTER(source.text, !=, NULL);
    tb_arena *const arena = tb_arena_create(0);
    for (size_t index = 0; index < 2; index++)
    {
        tb_shader_compiled compiled = {0};
        TB_TEST_REQUIRE_UINT(
            compile_stage(&source, entries[index], stages[index], arena, &compiled), ==,
            TB_SHADER_OK);
        char *const text = tb_arena_calloc(arena, compiled.code_size + 1, 1, 1);
        memcpy(text, compiled.code, compiled.code_size);
        TB_TEST_EXPECT_UINT(tb_str_find(text, 0, compiled.code_size, attributes[index]), !=,
                            TB_STR_NOT_FOUND);
    }
    tb_arena_destroy(arena);
}

TB_TEST(shader_msl, unsupported_stage_reports_the_source_location)
{
    const char text[] = "[numthreads(1,1,1)] void Compute() {}";
    const tb_shader_file file = {"/Engine/Private/Compute.hlsl", text, sizeof(text) - 1};
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_compiled compiled = {0};
    TB_TEST_REQUIRE_UINT(compile_stage(&file, "Compute", TB_SHADER_STAGE_COMPUTE, arena, &compiled),
                         ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(compiled.diagnostic_count, >, 0);
    TB_TEST_EXPECT_STRING(compiled.diagnostics[0].path, ==, file.path);
    TB_TEST_EXPECT_UINT(compiled.diagnostics[0].line, ==, 1);
    TB_TEST_EXPECT_POINTER(compiled.code, ==, NULL);
    tb_arena_destroy(arena);
}
