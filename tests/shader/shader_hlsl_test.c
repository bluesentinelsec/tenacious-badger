/* HLSL of Shader Model 5 written from the compiler's own form, for D3D11:
 * each resource or sampler a shader takes from a descriptor heap gets a
 * register of its own, and the table says where its index is read from;
 * what Shader Model 5 lacks is an error placed in the source; D3DCompiler's
 * messages are placed back in the source; and the shader the D3D11 backend
 * reads is written and read back whole. Every case runs on every platform,
 * with no D3DCompiler: a test's own stands in for it. */
#include "badger/arena.h"
#include "badger/fmt.h"
#include "badger/shader.h"
#include "badger/str.h"
#include "shader/shader_check.h"
#include "shader/shader_dxbc.h"
#include "shader/shader_form.h"
#include "shader/shader_hlsl.h"
#include "shader/shader_lower.h"
#include "shader/shader_reflect.h"
#include "tb_test.h"

#include <string.h>

#define MAIN_PATH "/Engine/Private/Main.hlsl"

typedef struct written
{
    tb_arena *arena;
    tb_shader_unit unit;
    tb_shader_reflection reflection;
    tb_array *diagnostics; /* tb_shader_diagnostic */
    tb_shader_hlsl hlsl;
    bool ok;
} written;

/* Write the HLSL of an entry point of a source that checks and lowers. */
static void write_source(written *out, const char *source, const char *entry, tb_shader_stage stage)
{
    out->arena = tb_arena_create(0);
    const tb_allocator allocator = tb_arena_allocator(out->arena);
    out->diagnostics = TB_ARRAY_CREATE(&allocator, tb_shader_diagnostic);
    tb_shader_file file = {MAIN_PATH, source, tb_str_length(source)};
    tb_shader_preprocess_request request = {0};
    request.path = MAIN_PATH;
    request.files = &file;
    request.file_count = 1;
    TB_TEST_REQUIRE_UINT(tb_shader_preprocess_unit(&request, out->arena, &out->unit), ==,
                         TB_SHADER_OK);
    tb_shader_syntax syntax = {0};
    TB_TEST_REQUIRE_UINT(tb_shader_parse(&out->unit, out->arena, &syntax), ==, TB_SHADER_OK);
    tb_shader_checked checked = {0};
    const tb_shader_result result =
        tb_shader_check(&syntax, TB_SHADER_MODEL_DEFAULT, out->arena, &checked);
    if (checked.error_count > 0)
    {
        TB_TEST_CONTEXT("check error: %s", checked.diagnostics[0].message);
    }
    TB_TEST_REQUIRE_UINT(result, ==, TB_SHADER_OK);
    tb_shader_form form = {0};
    tb_shader_lowered lowered = {0};
    TB_TEST_REQUIRE_UINT(tb_shader_lower(&syntax, &checked, out->arena, &form, &lowered), ==,
                         TB_SHADER_OK);
    uint32_t function = TB_SHADER_FORM_NONE;
    for (size_t index = 0; index < tb_shader_form_function_count(&form); index++)
    {
        if (tb_str_equal(tb_shader_form_function_at(&form, (uint32_t)index)->name, entry))
        {
            function = (uint32_t)index;
        }
    }
    TB_TEST_REQUIRE_UINT(function, !=, TB_SHADER_FORM_NONE);
    (void)tb_shader_form_add_entry_point(&form, function, (tb_shader_form_stage)stage);
    const uint32_t entry_index = (uint32_t)tb_array_length(form.entry_points) - 1;
    tb_shader_form extracted = {0};
    TB_TEST_REQUIRE(tb_shader_form_extract(&form, entry_index, out->arena, &extracted));
    TB_TEST_REQUIRE_POINTER(tb_shader_reflect(&extracted, out->arena, &out->reflection), ==, NULL);
    /* The extracted form lives in the arena; the request borrows it. */
    tb_shader_form *const kept =
        tb_arena_alloc(out->arena, sizeof(*kept), _Alignof(tb_shader_form));
    *kept = extracted;
    tb_shader_hlsl_request write = {0};
    write.form = kept;
    write.reflection = &out->reflection;
    write.files = out->unit.files;
    write.file_count = out->unit.file_count;
    out->ok = tb_shader_write_hlsl(&write, out->arena, out->diagnostics, &out->hlsl);
}

static bool has_text(const written *out, const char *part)
{
    return out->hlsl.text != NULL &&
           tb_str_find(out->hlsl.text, 0, out->hlsl.length, part) != TB_STR_NOT_FOUND;
}

static const tb_shader_diagnostic *diagnostic_at(const written *out, size_t index)
{
    return TB_ARRAY_AT(out->diagnostics, tb_shader_diagnostic, index);
}

/* A shader whose one error says `part`, at a line and a column of the source. */
static void expect_refused(const char *source, const char *entry, tb_shader_stage stage,
                           const char *part, size_t line, size_t column)
{
    written out = {0};
    write_source(&out, source, entry, stage);
    TB_TEST_CONTEXT("source: %s", source);
    TB_TEST_EXPECT_UINT(out.ok, ==, false);
    TB_TEST_EXPECT_POINTER(out.hlsl.text, ==, NULL);
    size_t found = 0;
    for (size_t index = 0; index < tb_array_length(out.diagnostics); index++)
    {
        const tb_shader_diagnostic *const diagnostic = diagnostic_at(&out, index);
        if (tb_str_find(diagnostic->message, 0, TB_STR_END, part) == TB_STR_NOT_FOUND)
        {
            continue;
        }
        found++;
        TB_TEST_EXPECT_UINT(diagnostic->severity, ==, TB_SHADER_ERROR);
        TB_TEST_EXPECT_STRING(diagnostic->path, ==, line > 0 ? MAIN_PATH : "");
        TB_TEST_EXPECT_UINT(diagnostic->line, ==, line);
        TB_TEST_EXPECT_UINT(diagnostic->column, ==, column);
    }
    /* Said once, where it is first used. */
    TB_TEST_EXPECT_UINT(found, ==, 1);
    tb_arena_destroy(out.arena);
}

/* -------------------------------------------------------------------------
 * Descriptor heaps.
 * ------------------------------------------------------------------------- */

static const char heap_source[] =
    "struct Inner { uint4 Ids; };\n"
    "struct DrawConstants { float4 Tint; uint Albedo; uint Buffer; Inner Nested[2]; };\n"
    "ConstantBuffer<DrawConstants> Draw : register(b0);\n"
    "cbuffer Scene : register(b1) { float4 Color; uint Sky; };\n"
    "Texture2D<float4> Fixed : register(t0);\n"
    "float4 main(float2 uv : TEXCOORD0) : SV_Target\n"
    "{\n"
    "    Texture2D<float4> albedo = ResourceDescriptorHeap[Draw.Albedo];\n"
    "    SamplerState filter = SamplerDescriptorHeap[NonUniformResourceIndex(Draw.Buffer)];\n"
    "    TextureCube<float4> sky = ResourceDescriptorHeap[Sky];\n"
    "    Texture2D<float4> again = ResourceDescriptorHeap[Draw.Albedo];\n"
    "    Texture2D<float4> nested = ResourceDescriptorHeap[Draw.Nested[1].Ids.z];\n"
    "    return albedo.Sample(filter, uv) + again.Sample(filter, uv) +\n"
    "           sky.Sample(filter, float3(uv, 1)) + nested.Sample(filter, uv) +\n"
    "           Fixed.Sample(filter, uv) * Color;\n"
    "}\n";

static void expect_slot(const tb_shader_dxbc_slot *slot, char register_class,
                        uint32_t register_index, tb_shader_dxbc_view view, uint8_t source,
                        uint32_t offset)
{
    TB_TEST_EXPECT_INT(slot->register_class, ==, register_class);
    TB_TEST_EXPECT_UINT(slot->register_index, ==, register_index);
    TB_TEST_EXPECT_UINT(slot->view, ==, view);
    TB_TEST_EXPECT_UINT(slot->source, ==, source);
    TB_TEST_EXPECT_UINT(slot->offset, ==, offset);
    TB_TEST_EXPECT_UINT(slot->heap, ==,
                        register_class == 's' ? TB_SHADER_DXBC_HEAP_SAMPLER
                                              : TB_SHADER_DXBC_HEAP_RESOURCE);
}

TB_TEST(shader_hlsl, heap_resources_get_registers_read_from_constants)
{
    /* Each resource from a heap is a register after those the shader
     * binds, and its index is where the constant buffer holds it: a
     * member, an element of an array of structs and a component of a
     * vector, laid out as HLSL packs a constant buffer. One taken twice by
     * the same index has one register. */
    written out = {0};
    write_source(&out, heap_source, "main", TB_SHADER_STAGE_PIXEL);
    TB_TEST_REQUIRE(out.ok);
    TB_TEST_REQUIRE_UINT(out.hlsl.slot_count, ==, 4);
    expect_slot(&out.hlsl.slots[0], 't', 1, TB_SHADER_DXBC_VIEW_TEXTURE2D, 0, 16);
    expect_slot(&out.hlsl.slots[1], 's', 0, TB_SHADER_DXBC_VIEW_SAMPLER, 0, 20);
    expect_slot(&out.hlsl.slots[2], 't', 2, TB_SHADER_DXBC_VIEW_TEXTURE_CUBE, 1, 16);
    expect_slot(&out.hlsl.slots[3], 't', 3, TB_SHADER_DXBC_VIEW_TEXTURE2D, 0, 56);
    /* What the shader reads of b0, b1, and nothing of b2. */
    TB_TEST_EXPECT_UINT(out.hlsl.constant_bytes[0], ==, 64);
    TB_TEST_EXPECT_UINT(out.hlsl.constant_bytes[1], ==, 20);
    TB_TEST_EXPECT_UINT(out.hlsl.constant_bytes[2], ==, 0);
    TB_TEST_CONTEXT("%s", out.hlsl.text);
    TB_TEST_EXPECT(has_text(&out, "Texture2D<float4> tb_t1 : register(t1);"));
    TB_TEST_EXPECT(has_text(&out, "SamplerState tb_s0 : register(s0);"));
    TB_TEST_EXPECT(has_text(&out, "TextureCube<float4> tb_t2 : register(t2);"));
    TB_TEST_EXPECT(has_text(&out, "tb_t1.Sample(tb_s0, "));
    /* Constant buffers are cbuffers, as Shader Model 5 has no ConstantBuffer<T>. */
    TB_TEST_EXPECT(has_text(&out, "cbuffer b0_Draw : register(b0)"));
    TB_TEST_EXPECT(has_text(&out, "cbuffer b1_Scene : register(b1)"));
    TB_TEST_EXPECT(!has_text(&out, "ResourceDescriptorHeap"));
    /* Members are named apart from what Shader Model 5 keeps for itself. */
    TB_TEST_EXPECT(has_text(&out, "uint m_Buffer;"));
    TB_TEST_EXPECT(has_text(&out, "uint4 m_Ids;"));
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_hlsl, an_index_the_shader_computes_is_refused_where_it_is_taken)
{
    expect_refused("struct Indices { uint x; };\n"
                   "ConstantBuffer<Indices> Draw : register(b0);\n"
                   "float4 main(float2 uv : TEXCOORD0) : SV_Target\n"
                   "{\n"
                   "    Texture2D<float4> t = ResourceDescriptorHeap[Draw.x + 1];\n"
                   "    return t.Load(int3(0, 0, 0));\n"
                   "}\n",
                   "main", TB_SHADER_STAGE_PIXEL, "a heap's index must be read as it is", 5, 49);
    /* From a vertex's value, as NonUniformResourceIndex() reads one. */
    expect_refused("float4 main(nointerpolation uint i : INDEX) : SV_Target\n"
                   "{\n"
                   "    Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(i)];\n"
                   "    return t.Load(int3(0, 0, 0));\n"
                   "}\n",
                   "main", TB_SHADER_STAGE_PIXEL, "the same for the whole draw", 3, 49);
}

TB_TEST(shader_hlsl, what_a_heap_cannot_bind_at_shader_model_5_is_refused)
{
    expect_refused("struct Indices { uint x; };\n"
                   "ConstantBuffer<Indices> Draw : register(b0);\n"
                   "float4 main() : SV_Target\n"
                   "{\n"
                   "    StructuredBuffer<float4> b = ResourceDescriptorHeap[Draw.x];\n"
                   "    return b[0];\n"
                   "}\n",
                   "main", TB_SHADER_STAGE_PIXEL,
                   "a StructuredBuffer<float4> from a descriptor heap", 5, 56);
}

/* -------------------------------------------------------------------------
 * What Shader Model 5 lacks, and what it writes otherwise.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_hlsl, what_shader_model_5_lacks_is_an_error_where_it_is_used)
{
    expect_refused("RWByteAddressBuffer Out : register(u0);\n"
                   "[numthreads(64, 1, 1)]\n"
                   "void main(uint i : SV_DispatchThreadID)\n"
                   "{\n"
                   "    Out.Store(0, WaveActiveSum(i));\n"
                   "}\n",
                   "main", TB_SHADER_STAGE_COMPUTE, "the intrinsic WaveActiveSum", 5, 31);
    expect_refused("RWByteAddressBuffer Out : register(u0);\n"
                   "[numthreads(64, 1, 1)]\n"
                   "void main(uint i : SV_DispatchThreadID)\n"
                   "{\n"
                   "    uint64_t wide = i;\n"
                   "    Out.Store(0, (uint)(wide >> 1));\n"
                   "}\n",
                   "main", TB_SHADER_STAGE_COMPUTE, "64-bit integers", 5, 21);
    /* Of the whole shader: its stage, which has no place of its own. */
    expect_refused("[shader(\"miss\")] void main(inout float4 payload) { payload = 1; }\n", "main",
                   TB_SHADER_STAGE_MISS, "the miss stage", 0, 0);
}

TB_TEST(shader_hlsl, templated_loads_of_bytes_are_loads_of_words)
{
    written out = {0};
    write_source(&out,
                 "ByteAddressBuffer In : register(t0);\n"
                 "RWByteAddressBuffer Out : register(u0);\n"
                 "[numthreads(1, 1, 1)]\n"
                 "void main()\n"
                 "{\n"
                 "    const float4 value = In.Load<float4>(16);\n"
                 "    const int2 pair = In.Load<int2>(32);\n"
                 "    Out.Store<float3>(0, value.xyz * pair.x);\n"
                 "}\n",
                 "main", TB_SHADER_STAGE_COMPUTE);
    TB_TEST_REQUIRE(out.ok);
    TB_TEST_CONTEXT("%s", out.hlsl.text);
    TB_TEST_EXPECT(has_text(&out, "asfloat(g0_In.Load4(16u))"));
    TB_TEST_EXPECT(has_text(&out, "asint(g0_In.Load2(32u))"));
    TB_TEST_EXPECT(has_text(&out, "g1_Out.Store3(0u, asuint("));
    TB_TEST_EXPECT_UINT(out.hlsl.slot_count, ==, 0);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_hlsl, logic_loops_and_switches_are_written_as_shader_model_5_reads_them)
{
    /* select() and and() are ?: and && on vectors; a loop's continuing
     * block runs at the top of each pass but the first, so that `continue`
     * runs it; a case that falls through into one that does something
     * repeats what that one does. */
    written out = {0};
    write_source(&out,
                 "float4 main(float4 a : A, nointerpolation int k : K) : SV_Target\n"
                 "{\n"
                 "    float4 sum = select(a > 0, a, -a);\n"
                 "    for (int i = 0; i < k; i++) { if (i == 2) { continue; } sum += 1; }\n"
                 "    switch (k) { case 1: sum *= 2; case 2: sum += 3; break; default: break; }\n"
                 "    return select(and(sum > 1, a < 2), sum, a);\n"
                 "}\n",
                 "main", TB_SHADER_STAGE_PIXEL);
    TB_TEST_REQUIRE(out.ok);
    TB_TEST_CONTEXT("%s", out.hlsl.text);
    TB_TEST_EXPECT(has_text(&out, " ? "));
    TB_TEST_EXPECT(has_text(&out, " && "));
    TB_TEST_EXPECT(has_text(&out, "while (true)"));
    TB_TEST_EXPECT(has_text(&out, "_first = false;"));
    TB_TEST_EXPECT(has_text(&out, "case 1:"));
    TB_TEST_EXPECT(has_text(&out, "default:"));
    TB_TEST_EXPECT(!has_text(&out, "select("));
    TB_TEST_EXPECT(!has_text(&out, "and("));
    tb_arena_destroy(out.arena);
}

/* -------------------------------------------------------------------------
 * D3DCompiler's messages, and the shader the D3D11 backend reads.
 * ------------------------------------------------------------------------- */

/* The first line of the text written for line `line` of the source. */
static size_t line_written_for(const tb_shader_hlsl *hlsl, uint32_t line)
{
    for (size_t index = 0; index < hlsl->line_count; index++)
    {
        if (hlsl->lines[index].line == line)
        {
            return index + 1;
        }
    }
    return 0;
}

TB_TEST(shader_hlsl, d3dcompiler_messages_are_placed_in_the_source)
{
    written out = {0};
    write_source(&out, heap_source, "main", TB_SHADER_STAGE_PIXEL);
    TB_TEST_REQUIRE(out.ok);
    const size_t written_line = line_written_for(&out.hlsl, 13);
    TB_TEST_REQUIRE_UINT(written_line, >, 0);
    const tb_allocator allocator = tb_arena_allocator(out.arena);
    /* As D3DCompiler writes them: after the directory it ran in, or as
     * given; one past the text's end; one with no place; one of another
     * file; and a summary that is no message. */
    const char *const messages = tb_fmt_string(
        &allocator,
        "C:\\game\\Engine\\Private\\Main.hlsl(%zu,5-9): error X3004: undeclared identifier 'x'\n"
        "Engine/Private/Main.hlsl(%zu,1): warning X3206: implicit truncation of vector type\n"
        "Engine/Private/Main.hlsl(99999,1): error X3000: syntax error\n"
        "error X3501: 'main': entrypoint not found\n"
        "Other.hlsl(3,4): warning X1000: elsewhere\n"
        "compilation failed; no code produced\n",
        written_line, written_line);
    tb_array *const diagnostics = TB_ARRAY_CREATE(&allocator, tb_shader_diagnostic);
    const size_t errors = tb_shader_d3dcompiler_diagnostics(
        messages, tb_str_length(messages), "Engine/Private/Main.hlsl", &out.hlsl, out.unit.files,
        out.unit.file_count, out.arena, diagnostics);
    TB_TEST_EXPECT_UINT(errors, ==, 3);
    TB_TEST_REQUIRE_UINT(tb_array_length(diagnostics), ==, 5);
    const tb_shader_diagnostic *const read = TB_ARRAY_DATA(diagnostics, tb_shader_diagnostic);
    TB_TEST_EXPECT_STRING(read[0].path, ==, MAIN_PATH);
    TB_TEST_EXPECT_UINT(read[0].line, ==, 13);
    TB_TEST_EXPECT_STRING(read[0].message, ==, "undeclared identifier 'x'");
    TB_TEST_EXPECT_UINT(read[1].severity, ==, TB_SHADER_WARNING);
    TB_TEST_EXPECT_UINT(read[1].line, ==, 13);
    TB_TEST_EXPECT_STRING(read[2].path, ==, "");
    TB_TEST_EXPECT_UINT(read[2].line, ==, 0);
    TB_TEST_EXPECT_STRING(read[3].path, ==, "");
    TB_TEST_EXPECT_STRING(read[3].message, ==, "'main': entrypoint not found");
    TB_TEST_EXPECT_STRING(read[4].path, ==, "Other.hlsl");
    TB_TEST_EXPECT_UINT(read[4].line, ==, 3);
    TB_TEST_EXPECT_UINT(read[4].column, ==, 4);
    tb_arena_destroy(out.arena);
}

/* A compiler that gives back a few bytes for any source, as a test's own. */
static const unsigned char fake_code[] = {'D', 'X', 'B', 'C', 1, 2, 3, 4};

static tb_d3dcompiler_result fake_compile(tb_d3dcompiler *compiler,
                                          const tb_d3dcompiler_request *request,
                                          tb_d3dcompiler_output *output,
                                          tb_d3dcompiler_error *error)
{
    (void)compiler;
    (void)error;
    TB_TEST_EXPECT_STRING(request->profile, ==, "ps_5_0");
    TB_TEST_EXPECT_STRING(request->name, ==, "Engine/Private/Main.hlsl");
    output->code = fake_code;
    output->code_size = sizeof(fake_code);
    output->messages = "";
    return TB_D3DCOMPILER_OK;
}

static tb_d3dcompiler_result fake_refuse(tb_d3dcompiler *compiler,
                                         const tb_d3dcompiler_request *request,
                                         tb_d3dcompiler_output *output, tb_d3dcompiler_error *error)
{
    (void)compiler;
    (void)request;
    output->messages = "";
    (void)tb_fmt_buffer(error->message, sizeof(error->message), "out of registers");
    return TB_D3DCOMPILER_FAILED;
}

static void fake_release(tb_d3dcompiler_output *output)
{
    const tb_d3dcompiler_output empty = {0};
    *output = empty;
}

TB_TEST(shader_hlsl, the_d3d11_shader_holds_its_slots_then_its_dxbc)
{
    written out = {0};
    write_source(&out, heap_source, "main", TB_SHADER_STAGE_PIXEL);
    TB_TEST_REQUIRE(out.ok);
    tb_shader_compile_request request = {0};
    request.source.path = MAIN_PATH;
    request.entry_point = "main";
    request.stage = TB_SHADER_STAGE_PIXEL;
    request.target = TB_SHADER_TARGET_DXBC;
    const tb_shader_d3dcompiler_functions compiler = {fake_compile, fake_release};
    const tb_allocator allocator = tb_arena_allocator(out.arena);
    tb_array *const diagnostics = TB_ARRAY_CREATE(&allocator, tb_shader_diagnostic);
    tb_shader_dxbc dxbc = {0};
    TB_TEST_REQUIRE(tb_shader_write_dxbc(&out.hlsl, &request, out.unit.files, out.unit.file_count,
                                         &compiler, out.arena, diagnostics, &dxbc));
    TB_TEST_EXPECT_UINT(dxbc.error_count, ==, 0);
    tb_shader_dxbc_shader read = {0};
    TB_TEST_REQUIRE(tb_shader_dxbc_read(dxbc.code, dxbc.code_size, &read));
    TB_TEST_EXPECT_UINT(read.slot_count, ==, out.hlsl.slot_count);
    TB_TEST_EXPECT_BYTES(read.slots, out.hlsl.slots, sizeof(tb_shader_dxbc_slot) * read.slot_count);
    TB_TEST_EXPECT_BYTES(read.constant_bytes, out.hlsl.constant_bytes, sizeof(read.constant_bytes));
    TB_TEST_REQUIRE_UINT(read.code_size, ==, sizeof(fake_code));
    TB_TEST_EXPECT_BYTES(read.code, fake_code, sizeof(fake_code));

    /* Anything short, damaged, or of another version is no shader. */
    unsigned char *const copy = tb_arena_alloc(out.arena, dxbc.code_size, 4);
    memcpy(copy, dxbc.code, dxbc.code_size);
    TB_TEST_EXPECT(!tb_shader_dxbc_read(copy, dxbc.code_size - 1, &read));
    TB_TEST_EXPECT(!tb_shader_dxbc_read(copy, 16, &read));
    TB_TEST_EXPECT(!tb_shader_dxbc_read(NULL, 0, &read));
    copy[4] = 2;
    TB_TEST_EXPECT(!tb_shader_dxbc_read(copy, dxbc.code_size, &read));
    copy[4] = 1;
    copy[TB_SHADER_DXBC_HEADER_BYTES + 1] = 'x';
    TB_TEST_EXPECT(!tb_shader_dxbc_read(copy, dxbc.code_size, &read));
    copy[TB_SHADER_DXBC_HEADER_BYTES + 1] = 't';
    copy[8] = 99;
    TB_TEST_EXPECT(!tb_shader_dxbc_read(copy, dxbc.code_size, &read));
    copy[8] = (unsigned char)out.hlsl.slot_count;
    TB_TEST_EXPECT(tb_shader_dxbc_read(copy, dxbc.code_size, &read));

    /* A failure says why, though the compiler's messages do not. */
    const tb_shader_d3dcompiler_functions refusing = {fake_refuse, fake_release};
    TB_TEST_EXPECT(!tb_shader_write_dxbc(&out.hlsl, &request, out.unit.files, out.unit.file_count,
                                         &refusing, out.arena, diagnostics, &dxbc));
    TB_TEST_EXPECT_UINT(dxbc.error_count, ==, 1);
    TB_TEST_EXPECT_POINTER(dxbc.code, ==, NULL);
    TB_TEST_EXPECT_STRING(TB_ARRAY_AT(diagnostics, tb_shader_diagnostic, 0)->message, ==,
                          "d3dcompiler: out of registers");

    /* Shader Model 5's stages have profiles; the others none. */
    char profile[TB_SHADER_DXBC_PROFILE_CAPACITY] = {0};
    TB_TEST_EXPECT(tb_shader_dxbc_profile(TB_SHADER_STAGE_COMPUTE, profile));
    TB_TEST_EXPECT_STRING(profile, ==, "cs_5_0");
    TB_TEST_EXPECT(!tb_shader_dxbc_profile(TB_SHADER_STAGE_MESH, profile));
    tb_arena_destroy(out.arena);
}
