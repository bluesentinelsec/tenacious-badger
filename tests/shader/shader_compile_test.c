/* Compiling a shader's entry point, and its reflection: the stage, the
 * thread group, the values it takes and gives by their semantics, and the
 * bindings, with registers assigned as DXC assigns them. The expected
 * bindings and signatures were read from DXC 1.8.2502's disassembly
 * (`dxc -T ps_6_6 -Fc`) of the same shaders. */
#include "badger/arena.h"
#include "badger/array.h"
#include "badger/fmt.h"
#include "badger/shader.h"
#include "badger/sort.h"
#include "badger/str.h"
#include "tb_test.h"
#include "test_data.h"

#include <string.h>

extern const tb_test_file tb_shader_reflection_files[];
extern const size_t tb_shader_reflection_files_count;

#define MAIN_PATH "/Engine/Private/Main.hlsl"

typedef struct compiled
{
    tb_arena *arena;
    tb_shader_result result;
    tb_shader_compiled output;
} compiled;

static void compile_text(compiled *out, const char *text, const char *entry_point,
                         tb_shader_stage stage)
{
    tb_shader_file file = {0};
    file.path = MAIN_PATH;
    file.text = text;
    file.length = tb_str_length(text);
    tb_shader_compile_request request = {0};
    request.source.path = MAIN_PATH;
    request.source.files = &file;
    request.source.file_count = 1;
    request.entry_point = entry_point;
    request.stage = stage;
    out->arena = tb_arena_create(0);
    out->result = tb_shader_compile(&request, out->arena, &out->output);
    if (out->output.diagnostic_count > 0)
    {
        TB_TEST_CONTEXT("first diagnostic: %s", out->output.diagnostics[0].message);
    }
}

static void expect_binding(const tb_shader_binding *binding, const char *name,
                           tb_shader_binding_kind kind, unsigned space, unsigned index,
                           unsigned count)
{
    TB_TEST_CONTEXT("binding %s", name);
    TB_TEST_EXPECT_STRING(binding->name, ==, name);
    TB_TEST_EXPECT_UINT(binding->kind, ==, kind);
    TB_TEST_EXPECT_UINT(binding->space, ==, space);
    TB_TEST_EXPECT_UINT(binding->register_index, ==, index);
    TB_TEST_EXPECT_UINT(binding->count, ==, count);
}

static void expect_element(const tb_shader_signature_element *element, const char *semantic,
                           unsigned index, tb_shader_component component, unsigned columns,
                           tb_shader_interpolation interpolation)
{
    TB_TEST_CONTEXT("element %s%u", semantic, index);
    TB_TEST_EXPECT_STRING(element->semantic, ==, semantic);
    TB_TEST_EXPECT_UINT(element->semantic_index, ==, index);
    TB_TEST_EXPECT_UINT(element->component, ==, component);
    TB_TEST_EXPECT_UINT(element->columns, ==, columns);
    TB_TEST_EXPECT_UINT(element->interpolation, ==, interpolation);
}

/* The shader DXC was asked about: what it reports, this reports. */
static const char *const pixel_source =
    "Texture2D unused_tex;\n"
    "Texture2D albedo;\n"
    "Texture2D normals : register(t0);\n"
    "Texture2D extra;\n"
    "SamplerState s;\n"
    "SamplerComparisonState shadow : register(s1); Texture2D<float> depth;\n"
    "RWTexture2D<float4> output;\n"
    "StructuredBuffer<float4> data : register(t2, space1);\n"
    "Texture2D<float> arr[4];\n"
    "cbuffer Frame { float4 tint; float time; };\n"
    "float4 loose;\n"
    "struct PSIn { float4 position : SV_Position; float2 uv : TEXCOORD0; "
    "nointerpolation uint id : ID; float3 normal[2] : NORMAL; };\n"
    "struct PSOut { float4 color : SV_Target0; float4 other : SV_Target2; };\n"
    "PSOut main(PSIn i, bool front : SV_IsFrontFace) {\n"
    "  PSOut o;\n"
    "  o.color = albedo.Sample(s, i.uv) * tint + normals.Sample(s, i.uv) + "
    "extra.Sample(s, i.uv) * time + loose;\n"
    "  o.color += data[i.id] + arr[i.id % 4].Sample(s, i.uv) + depth.SampleCmp(shadow, i.uv, "
    "0.5);\n"
    "  o.other = float4(i.normal[1], front ? 1 : 0);\n"
    "  output[uint2(0, 0)] = o.color;\n"
    "  return o;\n"
    "}\n";

TB_TEST(shader_compile, registers_are_assigned_as_dxc_assigns_them)
{
    compiled out = {0};
    compile_text(&out, pixel_source, "main", TB_SHADER_STAGE_PIXEL);
    TB_TEST_REQUIRE_UINT(out.result, ==, TB_SHADER_OK);
    const tb_shader_reflection *const reflection = &out.output.reflection;
    TB_TEST_EXPECT_UINT(reflection->stage, ==, TB_SHADER_STAGE_PIXEL);
    /* What the entry point does not use is not bound; `$Globals` is first,
     * and what has no register takes the lowest free one, in order. */
    TB_TEST_REQUIRE_UINT(reflection->binding_count, ==, 11);
    const tb_shader_binding *const b = reflection->bindings;
    expect_binding(&b[0], "$Globals", TB_SHADER_BINDING_CONSTANT_BUFFER, 0, 0, 1);
    expect_binding(&b[1], "Frame", TB_SHADER_BINDING_CONSTANT_BUFFER, 0, 1, 1);
    expect_binding(&b[2], "s", TB_SHADER_BINDING_SAMPLER, 0, 0, 1);
    expect_binding(&b[3], "shadow", TB_SHADER_BINDING_SAMPLER, 0, 1, 1);
    expect_binding(&b[4], "albedo", TB_SHADER_BINDING_READ, 0, 1, 1);
    expect_binding(&b[5], "normals", TB_SHADER_BINDING_READ, 0, 0, 1);
    expect_binding(&b[6], "extra", TB_SHADER_BINDING_READ, 0, 2, 1);
    expect_binding(&b[7], "depth", TB_SHADER_BINDING_READ, 0, 3, 1);
    expect_binding(&b[8], "data", TB_SHADER_BINDING_READ, 1, 2, 1);
    expect_binding(&b[9], "arr", TB_SHADER_BINDING_READ, 0, 4, 4);
    expect_binding(&b[10], "output", TB_SHADER_BINDING_READ_WRITE, 0, 0, 1);
    TB_TEST_EXPECT_UINT(b[4].shape, ==, TB_SHADER_SHAPE_TEXTURE2D);
    TB_TEST_EXPECT_UINT(b[4].element, ==, TB_SHADER_COMPONENT_FLOAT32);
    TB_TEST_EXPECT_UINT(b[4].element_columns, ==, 4);
    TB_TEST_EXPECT_UINT(b[7].element_columns, ==, 1);
    TB_TEST_EXPECT_UINT(b[8].shape, ==, TB_SHADER_SHAPE_STRUCTURED_BUFFER);
    TB_TEST_EXPECT_UINT(b[0].shape, ==, TB_SHADER_SHAPE_NONE);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_compile, signatures_pass_each_semantic_as_dxc_does)
{
    compiled out = {0};
    compile_text(&out, pixel_source, "main", TB_SHADER_STAGE_PIXEL);
    TB_TEST_REQUIRE_UINT(out.result, ==, TB_SHADER_OK);
    const tb_shader_reflection *const reflection = &out.output.reflection;
    /* An array passes one element for each of its elements; a bool passes
     * as a uint; floats interpolate linearly, SV_Position without
     * perspective, and integers not at all. */
    TB_TEST_REQUIRE_UINT(reflection->input_count, ==, 6);
    const tb_shader_signature_element *const in = reflection->inputs;
    expect_element(&in[0], "SV_Position", 0, TB_SHADER_COMPONENT_FLOAT32, 4,
                   TB_SHADER_INTERPOLATION_NOPERSPECTIVE);
    expect_element(&in[1], "TEXCOORD", 0, TB_SHADER_COMPONENT_FLOAT32, 2,
                   TB_SHADER_INTERPOLATION_LINEAR);
    expect_element(&in[2], "ID", 0, TB_SHADER_COMPONENT_UINT32, 1,
                   TB_SHADER_INTERPOLATION_CONSTANT);
    expect_element(&in[3], "NORMAL", 0, TB_SHADER_COMPONENT_FLOAT32, 3,
                   TB_SHADER_INTERPOLATION_LINEAR);
    expect_element(&in[4], "NORMAL", 1, TB_SHADER_COMPONENT_FLOAT32, 3,
                   TB_SHADER_INTERPOLATION_LINEAR);
    expect_element(&in[5], "SV_IsFrontFace", 0, TB_SHADER_COMPONENT_UINT32, 1,
                   TB_SHADER_INTERPOLATION_CONSTANT);
    TB_TEST_REQUIRE_UINT(reflection->output_count, ==, 2);
    expect_element(&reflection->outputs[0], "SV_Target", 0, TB_SHADER_COMPONENT_FLOAT32, 4,
                   TB_SHADER_INTERPOLATION_NONE);
    expect_element(&reflection->outputs[1], "SV_Target", 2, TB_SHADER_COMPONENT_FLOAT32, 4,
                   TB_SHADER_INTERPOLATION_NONE);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_compile, a_compute_shader_has_a_thread_group_and_no_signature)
{
    compiled out = {0};
    compile_text(&out,
                 "RWBuffer<uint> outputs;\n"
                 "[numthreads(8, 4, 2)] void cs(uint3 id : SV_DispatchThreadID) { outputs[id.x] = "
                 "id.y; }",
                 "cs", TB_SHADER_STAGE_COMPUTE);
    TB_TEST_REQUIRE_UINT(out.result, ==, TB_SHADER_OK);
    const tb_shader_reflection *const reflection = &out.output.reflection;
    TB_TEST_EXPECT_UINT(reflection->thread_group[0], ==, 8);
    TB_TEST_EXPECT_UINT(reflection->thread_group[1], ==, 4);
    TB_TEST_EXPECT_UINT(reflection->thread_group[2], ==, 2);
    TB_TEST_EXPECT_UINT(reflection->input_count, ==, 0);
    TB_TEST_EXPECT_UINT(reflection->output_count, ==, 0);
    TB_TEST_REQUIRE_UINT(reflection->binding_count, ==, 1);
    expect_binding(&reflection->bindings[0], "outputs", TB_SHADER_BINDING_READ_WRITE, 0, 0, 1);
    TB_TEST_EXPECT_UINT(reflection->bindings[0].shape, ==, TB_SHADER_SHAPE_BUFFER);
    TB_TEST_EXPECT_UINT(reflection->bindings[0].element, ==, TB_SHADER_COMPONENT_UINT32);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_compile, vertex_and_geometry_signatures)
{
    compiled out = {0};
    /* A vertex shader's inputs take a register each. A matrix passes a
     * column for each semantic index, as HLSL's are column-major, each in a
     * register of its own. */
    compile_text(&out,
                 "struct VSOut { float4 position : SV_Position; float3x4 frame : FRAME2; };\n"
                 "VSOut vs(float3 p : POSITION, uint id : SV_VertexID) { VSOut o; "
                 "o.position = float4(p, id); o.frame = 0; return o; }",
                 "vs", TB_SHADER_STAGE_VERTEX);
    TB_TEST_REQUIRE_UINT(out.result, ==, TB_SHADER_OK);
    const tb_shader_reflection *reflection = &out.output.reflection;
    TB_TEST_REQUIRE_UINT(reflection->input_count, ==, 2);
    expect_element(&reflection->inputs[0], "POSITION", 0, TB_SHADER_COMPONENT_FLOAT32, 3,
                   TB_SHADER_INTERPOLATION_NONE);
    expect_element(&reflection->inputs[1], "SV_VertexID", 0, TB_SHADER_COMPONENT_UINT32, 1,
                   TB_SHADER_INTERPOLATION_NONE);
    TB_TEST_EXPECT_UINT(reflection->inputs[1].register_index, ==, 1);
    TB_TEST_REQUIRE_UINT(reflection->output_count, ==, 5);
    expect_element(&reflection->outputs[1], "FRAME", 2, TB_SHADER_COMPONENT_FLOAT32, 3,
                   TB_SHADER_INTERPOLATION_NONE);
    expect_element(&reflection->outputs[4], "FRAME", 5, TB_SHADER_COMPONENT_FLOAT32, 3,
                   TB_SHADER_INTERPOLATION_NONE);
    TB_TEST_EXPECT_UINT(reflection->outputs[4].register_index, ==, 4);
    TB_TEST_EXPECT_UINT(reflection->outputs[4].first_column, ==, 0);
    tb_arena_destroy(out.arena);

    /* A geometry shader takes its primitive's vertex once and gives its stream's. */
    compile_text(
        &out,
        "struct V { float4 p : SV_Position; float2 uv : TEXCOORD; };\n"
        "[maxvertexcount(3)] void gs(triangle V input[3], inout TriangleStream<V> "
        "stream, uint primitive : SV_PrimitiveID) { stream.Append(input[primitive % 3]); }",
        "gs", TB_SHADER_STAGE_GEOMETRY);
    TB_TEST_REQUIRE_UINT(out.result, ==, TB_SHADER_OK);
    reflection = &out.output.reflection;
    TB_TEST_REQUIRE_UINT(reflection->input_count, ==, 3);
    expect_element(&reflection->inputs[1], "TEXCOORD", 0, TB_SHADER_COMPONENT_FLOAT32, 2,
                   TB_SHADER_INTERPOLATION_NONE);
    expect_element(&reflection->inputs[2], "SV_PrimitiveID", 0, TB_SHADER_COMPONENT_UINT32, 1,
                   TB_SHADER_INTERPOLATION_NONE);
    /* A geometry shader's primitive is passed in no register. */
    TB_TEST_EXPECT_UINT(reflection->inputs[2].register_index, ==, TB_SHADER_NO_REGISTER);
    TB_TEST_EXPECT_UINT(reflection->output_count, ==, 2);
    tb_arena_destroy(out.arena);
}

/* A variable named `name` among a buffer's variables. */
static const tb_shader_variable *find_variable(const tb_shader_constant_buffer *buffer,
                                               const char *name)
{
    for (size_t index = 0; index < buffer->variable_count; index++)
    {
        if (tb_str_equal(buffer->variables[index].name, name))
        {
            return &buffer->variables[index];
        }
    }
    return NULL;
}

static void expect_at(const tb_shader_constant_buffer *buffer, const char *name, unsigned offset,
                      unsigned size)
{
    TB_TEST_CONTEXT("variable %s", name);
    const tb_shader_variable *const variable = find_variable(buffer, name);
    TB_TEST_REQUIRE_POINTER(variable, !=, NULL);
    TB_TEST_EXPECT_UINT(variable->offset, ==, offset);
    TB_TEST_EXPECT_UINT(variable->size, ==, size);
}

TB_TEST(shader_compile, constant_buffers_are_packed_as_dxc_packs_them)
{
    compiled out = {0};
    compile_text(&out,
                 "struct Inner { float a; float2 b; };\n"
                 "struct Light { float3 color; float range; Inner inner; float tail; };\n"
                 "cbuffer Layout { float x; float3 v3; float2 v2; float2 w2; float s1; "
                 "float3 after_s1; float arr[3]; float tail1; Light light; float tail2; "
                 "float4x3 m43; row_major float4x3 rm43; float2x2 m22; float tail3; double d; "
                 "float tail4; min16float mf; bool flag; half h; Light lights[2]; "
                 "float3x2 m32arr[2]; float tail5; };\n"
                 "cbuffer Packed { float p0 : packoffset(c1.y); float4 p1 : packoffset(c0); "
                 "float p2 : packoffset(c2); };\n"
                 "float4 main() : SV_Target { return x + v3.x + v2.x + w2.x + s1 + after_s1.x + "
                 "arr[2] + tail1 + light.inner.b.y + tail2 + m43[0][0] + rm43[0][0] + m22[0][0] + "
                 "tail3 + (float)d + tail4 + mf + flag + h + lights[1].tail + m32arr[1][0][0] + "
                 "tail5 + p0 + p1.x + p2; }",
                 "main", TB_SHADER_STAGE_PIXEL);
    TB_TEST_REQUIRE_UINT(out.result, ==, TB_SHADER_OK);
    const tb_shader_reflection *const reflection = &out.output.reflection;
    TB_TEST_REQUIRE_UINT(reflection->buffer_count, ==, 2);
    const tb_shader_constant_buffer *const layout = &reflection->buffers[0];
    TB_TEST_EXPECT_STRING(layout->name, ==, "Layout");
    TB_TEST_EXPECT_UINT(layout->size, ==, 464);
    TB_TEST_EXPECT_UINT(layout->top_count, ==, 22);
    /* No vector crosses a register; an array's elements each start one. */
    expect_at(layout, "x", 0, 4);
    expect_at(layout, "v3", 4, 12);
    expect_at(layout, "v2", 16, 8);
    expect_at(layout, "w2", 24, 8);
    expect_at(layout, "s1", 32, 4);
    expect_at(layout, "after_s1", 36, 12);
    expect_at(layout, "arr", 48, 36);
    expect_at(layout, "tail1", 84, 4);
    /* A struct starts a register, and what follows packs into its last. */
    expect_at(layout, "light", 96, 32);
    expect_at(layout, "inner", 112, 12);
    expect_at(layout, "b", 116, 8);
    expect_at(layout, "tail2", 128, 4);
    /* A column-major matrix has a register for each column, a row-major one
     * for each row; the last is not padded. */
    expect_at(layout, "m43", 144, 48);
    expect_at(layout, "rm43", 192, 60);
    expect_at(layout, "m22", 256, 24);
    expect_at(layout, "tail3", 280, 4);
    expect_at(layout, "d", 288, 8);
    expect_at(layout, "tail4", 296, 4);
    /* A minimum-precision value takes a register of its own. */
    expect_at(layout, "mf", 304, 4);
    expect_at(layout, "flag", 320, 4);
    expect_at(layout, "h", 324, 4);
    expect_at(layout, "lights", 336, 64);
    expect_at(layout, "m32arr", 400, 60);
    expect_at(layout, "tail5", 460, 4);
    const tb_shader_variable *const lights = find_variable(layout, "lights");
    TB_TEST_REQUIRE_POINTER(lights, !=, NULL);
    TB_TEST_EXPECT_UINT(lights->variable_class, ==, TB_SHADER_VARIABLE_STRUCT);
    TB_TEST_EXPECT_UINT(lights->elements, ==, 2);
    TB_TEST_EXPECT_UINT(lights->stride, ==, 32);
    TB_TEST_EXPECT_UINT(lights->member_count, ==, 4);
    TB_TEST_EXPECT_UINT(layout->variables[lights->first_member].offset, ==, 336);
    const tb_shader_variable *const rm43 = find_variable(layout, "rm43");
    TB_TEST_REQUIRE_POINTER(rm43, !=, NULL);
    TB_TEST_EXPECT_UINT(rm43->variable_class, ==, TB_SHADER_VARIABLE_MATRIX_ROWS);
    TB_TEST_EXPECT_UINT(rm43->rows, ==, 4);
    TB_TEST_EXPECT_UINT(rm43->columns, ==, 3);
    TB_TEST_EXPECT_UINT(find_variable(layout, "m43")->variable_class, ==,
                        TB_SHADER_VARIABLE_MATRIX_COLUMNS);
    TB_TEST_EXPECT_UINT(find_variable(layout, "d")->component, ==, TB_SHADER_COMPONENT_FLOAT64);
    /* packoffset places a variable where it says. */
    const tb_shader_constant_buffer *const packed = &reflection->buffers[1];
    TB_TEST_EXPECT_STRING(packed->name, ==, "Packed");
    TB_TEST_EXPECT_UINT(packed->size, ==, 36);
    expect_at(packed, "p0", 20, 4);
    expect_at(packed, "p1", 0, 16);
    expect_at(packed, "p2", 32, 4);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_compile, arrays_and_minimum_precision_are_packed_as_dxc_packs_them)
{
    compiled out = {0};
    compile_text(&out,
                 "struct S3 { float3 v; };\n"
                 "cbuffer M { float a; min16float b; float c; min16float2 d; float e; min16int f; "
                 "float g; S3 arr[2]; float h; float2 i[2]; float j; };\n"
                 "float4 main() : SV_Target { return a + b + c + d.x + e + f + g + arr[1].v.x + h "
                 "+ i[1].x + j; }",
                 "main", TB_SHADER_STAGE_PIXEL);
    TB_TEST_REQUIRE_UINT(out.result, ==, TB_SHADER_OK);
    TB_TEST_REQUIRE_UINT(out.output.reflection.buffer_count, ==, 1);
    const tb_shader_constant_buffer *const buffer = &out.output.reflection.buffers[0];
    TB_TEST_EXPECT_UINT(buffer->size, ==, 172);
    expect_at(buffer, "a", 0, 4);
    expect_at(buffer, "b", 16, 4);
    expect_at(buffer, "c", 32, 4);
    expect_at(buffer, "d", 48, 8);
    expect_at(buffer, "e", 64, 4);
    expect_at(buffer, "f", 80, 4);
    expect_at(buffer, "g", 96, 4);
    expect_at(buffer, "arr", 112, 28);
    expect_at(buffer, "h", 140, 4);
    expect_at(buffer, "i", 144, 24);
    expect_at(buffer, "j", 168, 4);
    TB_TEST_EXPECT_UINT(find_variable(buffer, "i")->stride, ==, 16);
    tb_arena_destroy(out.arena);
}

/* `#pragma pack_matrix` lays out the matrices declared after it, struct
 * members included, unless a declaration says otherwise; a later pragma
 * changes it back. The corpus case pack_matrix.hlsl checks the offsets
 * against DXC; this checks the layout each matrix is reported with. */
TB_TEST(shader_compile, pack_matrix_lays_out_the_matrices_after_it)
{
    compiled out = {0};
    compile_text(&out,
                 "float2x4 before;\n"
                 "#pragma pack_matrix(row_major)\n"
                 "struct S { float2x4 m; };\n"
                 "cbuffer P { float2x4 a; column_major float2x4 c; S s; };\n"
                 "#pragma pack_matrix( column_major )\n"
                 "cbuffer Q { float2x4 e; row_major float2x4 k; };\n"
                 "#pragma pack_matrix(something_else)\n"
                 "cbuffer R { float2x4 u; };\n"
                 "float4 main() : SV_Target { return before[0] + a[0] + c[0] + s.m[0] + e[0] + "
                 "k[0] + u[0]; }",
                 "main", TB_SHADER_STAGE_PIXEL);
    TB_TEST_REQUIRE_UINT(out.result, ==, TB_SHADER_OK);
    const tb_shader_reflection *const reflection = &out.output.reflection;
    TB_TEST_REQUIRE_UINT(reflection->buffer_count, ==, 4);
    const struct
    {
        size_t buffer;
        const char *name;
        tb_shader_variable_class variable_class;
    } expected[] = {
        {0, "before", TB_SHADER_VARIABLE_MATRIX_COLUMNS},
        {1, "a", TB_SHADER_VARIABLE_MATRIX_ROWS},
        {1, "c", TB_SHADER_VARIABLE_MATRIX_COLUMNS},
        {1, "m", TB_SHADER_VARIABLE_MATRIX_ROWS},
        {2, "e", TB_SHADER_VARIABLE_MATRIX_COLUMNS},
        {2, "k", TB_SHADER_VARIABLE_MATRIX_ROWS},
        /* A pragma naming no layout changes nothing. */
        {3, "u", TB_SHADER_VARIABLE_MATRIX_COLUMNS},
    };
    for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); index++)
    {
        TB_TEST_CONTEXT("variable %s", expected[index].name);
        const tb_shader_variable *const variable =
            find_variable(&reflection->buffers[expected[index].buffer], expected[index].name);
        TB_TEST_REQUIRE_POINTER(variable, !=, NULL);
        TB_TEST_EXPECT_UINT(variable->variable_class, ==, expected[index].variable_class);
    }
    tb_arena_destroy(out.arena);
}

static unsigned features_of(const char *text, const char *entry_point, tb_shader_stage stage)
{
    compiled out = {0};
    compile_text(&out, text, entry_point, stage);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    const unsigned features = out.output.reflection.features;
    tb_arena_destroy(out.arena);
    return features;
}

TB_TEST(shader_compile, a_shader_records_the_features_it_needs)
{
    /* DXC reports this one needs doubles, wave operations, 64-bit integers
     * and inline ray tracing (ray tracing tier 1.1); its array of no size
     * is bindless, and its atomic is of 64 bits. */
    const unsigned compute =
        features_of("RWByteAddressBuffer buf; RWStructuredBuffer<uint64_t> counters;\n"
                    "RaytracingAccelerationStructure scene;\n"
                    "Texture2D textures[] : register(t0, space1); SamplerState s;\n"
                    "cbuffer C { double scale; };\n"
                    "[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
                    "  uint total = WaveActiveSum(id.x);\n"
                    "  uint64_t wide = counters[id.x] + 1;\n"
                    "  InterlockedAdd(counters[0], wide);\n"
                    "  RayQuery<0> q; RayDesc ray = (RayDesc)0;\n"
                    "  q.TraceRayInline(scene, 0, 0xff, ray); q.Proceed();\n"
                    "  float4 c = textures[NonUniformResourceIndex(id.x)].SampleLevel(s, float2(0, "
                    "0), 0);\n"
                    "  buf.Store(0, total + (uint)(scale * c.x) + (uint)wide);\n"
                    "}",
                    "main", TB_SHADER_STAGE_COMPUTE);
    TB_TEST_EXPECT_UINT(compute, ==,
                        TB_SHADER_FEATURE_WAVE_OPERATIONS | TB_SHADER_FEATURE_BINDLESS |
                            TB_SHADER_FEATURE_FLOAT64 | TB_SHADER_FEATURE_INT64 |
                            TB_SHADER_FEATURE_ATOMIC64 | TB_SHADER_FEATURE_RAY_QUERY);
    /* What an entry point does not reach needs nothing. */
    TB_TEST_EXPECT_UINT(features_of("cbuffer C { double unused; };\n"
                                    "float helper() { return WaveActiveSum(1.0); }\n"
                                    "float4 main() : SV_Target { return 1; }",
                                    "main", TB_SHADER_STAGE_PIXEL),
                        ==, 0);
    TB_TEST_EXPECT_UINT(features_of("RWByteAddressBuffer b;\n"
                                    "[numthreads(1, 1, 1)] void main() { b.InterlockedAdd64(0, "
                                    "1); }",
                                    "main", TB_SHADER_STAGE_COMPUTE),
                        ==, TB_SHADER_FEATURE_ATOMIC64 | TB_SHADER_FEATURE_INT64);
    TB_TEST_EXPECT_UINT(features_of("FeedbackTexture2D feedback; Texture2D t; SamplerState s;\n"
                                    "float4 main(float2 uv : UV) : SV_Target { "
                                    "feedback.WriteSamplerFeedback(t, s, uv); return 1; }",
                                    "main", TB_SHADER_STAGE_PIXEL),
                        ==, TB_SHADER_FEATURE_SAMPLER_FEEDBACK);
    TB_TEST_EXPECT_UINT(features_of("void rg() {}", "rg", TB_SHADER_STAGE_RAY_GENERATION), ==,
                        TB_SHADER_FEATURE_RAY_TRACING);
    TB_TEST_EXPECT_UINT(
        features_of("[numthreads(1, 1, 1)] void as() {}", "as", TB_SHADER_STAGE_AMPLIFICATION), ==,
        TB_SHADER_FEATURE_MESH_SHADER);
}

TB_TEST(shader_compile, features_are_named_for_the_console)
{
    TB_TEST_EXPECT_STRING(tb_shader_feature_name(TB_SHADER_FEATURE_WAVE_OPERATIONS), ==,
                          "wave operations");
    TB_TEST_EXPECT_STRING(tb_shader_feature_name(TB_SHADER_FEATURE_BINDLESS), ==,
                          "bindless resources");
    TB_TEST_EXPECT_STRING(tb_shader_feature_name(TB_SHADER_FEATURE_FLOAT64), ==, "64-bit floats");
    TB_TEST_EXPECT_STRING(tb_shader_feature_name(TB_SHADER_FEATURE_INT64), ==, "64-bit integers");
    TB_TEST_EXPECT_STRING(tb_shader_feature_name(TB_SHADER_FEATURE_ATOMIC64), ==, "64-bit atomics");
    TB_TEST_EXPECT_STRING(tb_shader_feature_name(TB_SHADER_FEATURE_RAY_QUERY), ==, "ray queries");
    TB_TEST_EXPECT_STRING(tb_shader_feature_name(TB_SHADER_FEATURE_RAY_TRACING), ==,
                          "ray-tracing pipelines");
    TB_TEST_EXPECT_STRING(tb_shader_feature_name(TB_SHADER_FEATURE_MESH_SHADER), ==,
                          "mesh shaders");
    TB_TEST_EXPECT_STRING(tb_shader_feature_name(TB_SHADER_FEATURE_SAMPLER_FEEDBACK), ==,
                          "sampler feedback");
    TB_TEST_EXPECT_STRING(tb_shader_feature_name((tb_shader_feature)0), ==, "unknown feature");
}

/* -------------------------------------------------------------------------
 * The corpus: reflection as DXC reports it (tools/shader_reflection_goldens.py).
 * ------------------------------------------------------------------------- */

typedef struct report
{
    tb_arena *arena;
    tb_allocator allocator;
    tb_array *lines; /* const char * */
} report;

static void add_line(report *out, const char *format, ...) TB_TEST_PRINTF_FORMAT(2, 3);

static void add_line(report *out, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const char *const line = tb_fmt_vstring(&out->allocator, format, arguments);
    va_end(arguments);
    (void)tb_array_push(out->lines, &line);
}

static const char *format_name(tb_shader_component component)
{
    if (component == TB_SHADER_COMPONENT_INT32)
    {
        return "int";
    }
    return component == TB_SHADER_COMPONENT_UINT32 ? "uint" : "float";
}

/* DXC's names of the interpolations the corpus uses. */
/* DXC's name of an interpolation mode. */
static const char *interpolation_name(tb_shader_interpolation interpolation)
{
    static const char *const names[] = {
        [TB_SHADER_INTERPOLATION_NONE] = "",
        [TB_SHADER_INTERPOLATION_LINEAR] = "linear",
        [TB_SHADER_INTERPOLATION_CONSTANT] = "nointerpolation",
        [TB_SHADER_INTERPOLATION_NOPERSPECTIVE] = "noperspective",
        [TB_SHADER_INTERPOLATION_CENTROID] = "centroid",
        [TB_SHADER_INTERPOLATION_NOPERSPECTIVE_CENTROID] = "noperspective centroid",
        [TB_SHADER_INTERPOLATION_SAMPLE] = "sample",
        [TB_SHADER_INTERPOLATION_NOPERSPECTIVE_SAMPLE] = "noperspective sample",
    };
    if ((size_t)interpolation >= sizeof(names) / sizeof(names[0]))
    {
        return "?";
    }
    return names[interpolation];
}

static const char *binding_type(tb_shader_binding_kind kind)
{
    switch (kind)
    {
    case TB_SHADER_BINDING_CONSTANT_BUFFER:
        return "cbuffer";
    case TB_SHADER_BINDING_TEXTURE_BUFFER:
        return "tbuffer";
    case TB_SHADER_BINDING_READ:
        return "texture";
    case TB_SHADER_BINDING_READ_WRITE:
        return "UAV";
    case TB_SHADER_BINDING_SAMPLER:
        return "sampler";
    }
    return "?";
}

static void report_bindings(report *out, const tb_shader_reflection *reflection)
{
    static const char *const prefixes[] = {"cb", "t", "t", "u", "s"};
    for (size_t index = 0; index < reflection->binding_count; index++)
    {
        const tb_shader_binding *const binding = &reflection->bindings[index];
        char bind[64] = {0};
        (void)tb_fmt_buffer(bind, sizeof(bind), "%s%u", prefixes[binding->kind],
                            binding->register_index);
        if (binding->space != 0)
        {
            char spaced[64] = {0};
            (void)tb_fmt_buffer(spaced, sizeof(spaced), "%s,space%u", bind, binding->space);
            memcpy(bind, spaced, sizeof(bind));
        }
        if (binding->count == 0)
        {
            add_line(out, "binding %s %s %s unbounded", binding->name, binding_type(binding->kind),
                     bind);
        }
        else
        {
            add_line(out, "binding %s %s %s %u", binding->name, binding_type(binding->kind), bind,
                     binding->count);
        }
    }
}

static void report_signature(report *out, const char *kind,
                             const tb_shader_signature_element *elements, size_t count,
                             bool interpolates)
{
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_signature_element *const element = &elements[index];
        /* DXC's report gives an element in no register no components, so
         * its width is not compared. */
        char where[32] = "-";
        char width[16] = "-";
        if (element->register_index != TB_SHADER_NO_REGISTER)
        {
            (void)tb_fmt_buffer(where, sizeof(where), "r%u.%c", element->register_index,
                                "xyzw"[element->first_column & 3u]);
            (void)tb_fmt_buffer(width, sizeof(width), "%u", element->columns);
        }
        if (interpolates)
        {
            add_line(out, "%s %s %u %s %s %s %s", kind, element->semantic, element->semantic_index,
                     format_name(element->component), width, where,
                     interpolation_name(element->interpolation));
        }
        else
        {
            add_line(out, "%s %s %u %s %s %s", kind, element->semantic, element->semantic_index,
                     format_name(element->component), width, where);
        }
    }
}

/* A variable and, for a struct, its members, each by its path. */
static void report_variable(report *out, const tb_shader_constant_buffer *buffer,
                            const tb_shader_variable *variable, const char *prefix)
{
    const char *const path = tb_fmt_string(&out->allocator, "%s.%s", prefix, variable->name);
    add_line(out, "variable %s %u", path, variable->offset);
    for (unsigned at = 0; at < variable->member_count; at++)
    {
        report_variable(out, buffer, &buffer->variables[variable->first_member + at], path);
    }
}

static void report_buffers(report *out, const tb_shader_reflection *reflection)
{
    for (size_t index = 0; index < reflection->buffer_count; index++)
    {
        const tb_shader_constant_buffer *const buffer = &reflection->buffers[index];
        add_line(out, "buffer %s %u", buffer->name, buffer->size);
        for (size_t at = 0; at < buffer->top_count; at++)
        {
            report_variable(out, buffer, &buffer->variables[at], buffer->name);
        }
    }
}

/* The features DXC names: of those the engine records, the ones it shares. */
static void report_features(report *out, unsigned features)
{
    static const struct
    {
        tb_shader_feature feature;
        const char *name;
    } named[] = {
        {TB_SHADER_FEATURE_FLOAT64, "float64"},
        {TB_SHADER_FEATURE_INT64, "int64"},
        {TB_SHADER_FEATURE_WAVE_OPERATIONS, "wave"},
        {TB_SHADER_FEATURE_RAY_QUERY, "rayquery"},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(named); index++)
    {
        if ((features & (unsigned)named[index].feature) != 0)
        {
            add_line(out, "feature %s", named[index].name);
        }
    }
}

static int compare_lines(const void *left, const void *right, void *context)
{
    (void)context;
    return tb_str_compare(*(const char *const *)left, *(const char *const *)right);
}

/* The reflection as tools/shader_reflection_goldens.py writes DXC's: sorted lines. */
static const char *render(report *out, const tb_shader_reflection *reflection)
{
    report_bindings(out, reflection);
    report_signature(out, "input", reflection->inputs, reflection->input_count,
                     reflection->stage == TB_SHADER_STAGE_PIXEL);
    report_signature(out, "output", reflection->outputs, reflection->output_count, false);
    report_buffers(out, reflection);
    report_features(out, reflection->features);
    const size_t count = tb_array_length(out->lines);
    tb_sort(tb_array_data(out->lines), count, sizeof(const char *), compare_lines, NULL);
    tb_array *const text = TB_ARRAY_CREATE(&out->allocator, char);
    for (size_t index = 0; index < count; index++)
    {
        const char *const line = *TB_ARRAY_AT(out->lines, const char *, index);
        tb_array_append(text, line, tb_str_length(line));
        tb_array_append(text, "\n", 1);
    }
    const char nul = '\0';
    (void)tb_array_push(text, &nul);
    return tb_array_data(text);
}

/* `// dxc: -T ps_6_6 -E main`: the stage and the entry point a shader is compiled for. */
static bool read_arguments(const char *text, tb_shader_stage *stage, char *entry, size_t size)
{
    static const struct
    {
        const char *prefix;
        tb_shader_stage stage;
    } profiles[] = {
        {"vs_", TB_SHADER_STAGE_VERTEX},        {"ps_", TB_SHADER_STAGE_PIXEL},
        {"gs_", TB_SHADER_STAGE_GEOMETRY},      {"hs_", TB_SHADER_STAGE_HULL},
        {"ds_", TB_SHADER_STAGE_DOMAIN},        {"cs_", TB_SHADER_STAGE_COMPUTE},
        {"as_", TB_SHADER_STAGE_AMPLIFICATION}, {"ms_", TB_SHADER_STAGE_MESH},
    };
    const size_t profile = tb_str_find(text, 0, tb_str_length(text), "-T ");
    const size_t name = tb_str_find(text, 0, tb_str_length(text), "-E ");
    const size_t line_end = tb_str_find_char(text, 0, tb_str_length(text), '\n');
    if (profile >= line_end || name >= line_end)
    {
        return false;
    }
    bool found = false;
    for (size_t index = 0; index < TB_TEST_COUNT_OF(profiles); index++)
    {
        if (tb_str_matches_at(text, profile + 3, profiles[index].prefix))
        {
            *stage = profiles[index].stage;
            found = true;
        }
    }
    const size_t start = name + 3;
    size_t end = start;
    while (end < line_end && text[end] != ' ' && text[end] != '\r' && end - start + 1 < size)
    {
        end++;
    }
    memcpy(entry, text + start, end - start);
    entry[end - start] = '\0';
    return found;
}

static const tb_test_file *golden_of(const tb_test_file *shader)
{
    const size_t stem = tb_str_length(shader->name) - tb_str_length(".hlsl");
    for (size_t index = 0; index < tb_shader_reflection_files_count; index++)
    {
        const tb_test_file *const file = &tb_shader_reflection_files[index];
        if (tb_str_length(file->name) == stem + tb_str_length(".dxc") &&
            memcmp(file->name, shader->name, stem) == 0 && tb_str_ends_with(file->name, ".dxc"))
        {
            return file;
        }
    }
    return NULL;
}

TB_TEST(shader_compile, reflection_matches_dxc)
{
    size_t shaders = 0;
    for (size_t index = 0; index < tb_shader_reflection_files_count; index++)
    {
        const tb_test_file *const shader = &tb_shader_reflection_files[index];
        if (!tb_str_ends_with(shader->name, ".hlsl"))
        {
            continue;
        }
        shaders++;
        TB_TEST_CONTEXT("shader %s", shader->name);
        const char *const text = (const char *)shader->bytes;
        const tb_test_file *const golden = golden_of(shader);
        TB_TEST_REQUIRE_POINTER(golden, !=, NULL);
        tb_shader_stage stage = TB_SHADER_STAGE_PIXEL;
        char entry[64] = {0};
        TB_TEST_REQUIRE_UINT(read_arguments(text, &stage, entry, sizeof(entry)), ==, true);
        compiled out = {0};
        compile_text(&out, text, entry, stage);
        TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
        report lines = {0};
        lines.arena = out.arena;
        lines.allocator = tb_arena_allocator(out.arena);
        lines.lines = TB_ARRAY_CREATE(&lines.allocator, const char *);
        TB_TEST_EXPECT_STRING(render(&lines, &out.output.reflection), ==,
                              (const char *)golden->bytes);
        tb_arena_destroy(out.arena);
    }
    TB_TEST_EXPECT_UINT(shaders, >, 0);
}

TB_TEST(shader_compile, what_cannot_be_compiled_says_why)
{
    compiled out = {0};
    compile_text(&out, "float4 main() : SV_Target { return 1; }", "nope", TB_SHADER_STAGE_PIXEL);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.output.diagnostic_count, ==, 1);
    TB_TEST_EXPECT_STRING(out.output.diagnostics[0].message, ==,
                          "there is no function 'nope' to run");
    TB_TEST_EXPECT_STRING(out.output.diagnostics[0].path, ==, "");
    tb_arena_destroy(out.arena);

    compile_text(&out, "float4 main(float4 x) : SV_Target { return x; }", "main",
                 TB_SHADER_STAGE_PIXEL);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.output.diagnostic_count, ==, 1);
    TB_TEST_EXPECT_STRING(out.output.diagnostics[0].message, ==,
                          "entry point 'main': 'x' is passed between stages without a semantic");
    tb_arena_destroy(out.arena);

    compile_text(&out,
                 "Texture2D t[]; SamplerState s;\n"
                 "float4 main(float2 uv : UV) : SV_Target { return t[0].Sample(s, uv); }",
                 "main", TB_SHADER_STAGE_PIXEL);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.output.diagnostic_count, ==, 1);
    TB_TEST_EXPECT_STRING(out.output.diagnostics[0].message, ==,
                          "entry point 'main': 't', an array of resources of no size, needs a "
                          "register");
    tb_arena_destroy(out.arena);

    /* The source's own errors come first, with where they are. */
    compile_text(&out, "float4 main() : SV_Target { return missing; }", "main",
                 TB_SHADER_STAGE_PIXEL);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.output.diagnostic_count, >, 0);
    TB_TEST_EXPECT_STRING(out.output.diagnostics[0].path, ==, MAIN_PATH);
    tb_arena_destroy(out.arena);

    /* A request that names no entry point or no stage is invalid. */
    compile_text(&out, "void main() {}", "", TB_SHADER_STAGE_PIXEL);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_INVALID);
    tb_arena_destroy(out.arena);
    compile_text(&out, "void main() {}", "main", (tb_shader_stage)99);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_INVALID);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_compile, stages_are_named_as_hlsl_names_them)
{
    TB_TEST_EXPECT_STRING(tb_shader_stage_name(TB_SHADER_STAGE_PIXEL), ==, "pixel");
    TB_TEST_EXPECT_STRING(tb_shader_stage_name(TB_SHADER_STAGE_RAY_GENERATION), ==,
                          "raygeneration");
    TB_TEST_EXPECT_STRING(tb_shader_stage_name((tb_shader_stage)99), ==, "unknown stage");
}
