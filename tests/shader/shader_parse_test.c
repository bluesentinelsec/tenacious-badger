/* The shader parser: the tree each construct of HLSL makes, the errors a
 * source that is not HLSL gets and where they are reported, recovery from
 * them, and the limits that keep a source from exhausting the stack. Trees
 * are compared as tb_shader_syntax_print() writes them. */
#include "badger/arena.h"
#include "badger/shader.h"
#include "badger/str.h"
#include "shader/shader_syntax.h"
#include "tb_test.h"

#include <string.h>

#define MAIN_PATH "/Engine/Private/Main.hlsl"

typedef struct parsed
{
    tb_arena *arena;
    tb_shader_result result;
    tb_shader_syntax syntax;
    const char *tree;
} parsed;

static void parse_source(parsed *out, const char *source)
{
    out->arena = tb_arena_create(0);
    tb_shader_file file = {0};
    file.path = MAIN_PATH;
    file.text = source;
    file.length = tb_str_length(source);
    tb_shader_preprocess_request request = {0};
    request.path = MAIN_PATH;
    request.files = &file;
    request.file_count = 1;
    tb_shader_unit unit = {0};
    const tb_shader_result preprocessed = tb_shader_preprocess_unit(&request, out->arena, &unit);
    TB_TEST_EXPECT_UINT(preprocessed, ==, TB_SHADER_OK);
    out->result = tb_shader_parse(&unit, out->arena, &out->syntax);
    out->tree = tb_shader_syntax_print(&out->syntax, out->arena);
}

static void finish(parsed *out)
{
    tb_arena_destroy(out->arena);
}

static void diagnostic_text(const parsed *out, size_t index, char *buffer, size_t size)
{
    TB_TEST_REQUIRE_UINT(out->syntax.diagnostic_count, >, index);
    (void)tb_shader_diagnostic_format(&out->syntax.diagnostics[index], buffer, size);
}

/* A source that must parse with no diagnostics into `expected`. */
static void expect_tree(const char *source, const char *expected)
{
    parsed out = {0};
    parse_source(&out, source);
    TB_TEST_CONTEXT("source: %s", source);
    if (out.syntax.diagnostic_count > 0)
    {
        char message[256] = {0};
        diagnostic_text(&out, 0, message, sizeof(message));
        TB_TEST_CONTEXT("first diagnostic: %s", message);
    }
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    TB_TEST_EXPECT_UINT(out.syntax.diagnostic_count, ==, 0);
    TB_TEST_EXPECT_STRING(out.tree, ==, expected);
    finish(&out);
}

/* A source that must fail, with `expected` its first error. */
static void expect_error(const char *source, const char *expected)
{
    parsed out = {0};
    parse_source(&out, source);
    TB_TEST_CONTEXT("source: %s", source);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    char message[256] = {0};
    diagnostic_text(&out, 0, message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==, expected);
    finish(&out);
}

#define AT(line, column) MAIN_PATH "(" #line "," #column "): error: "

/* -------------------------------------------------------------------------
 * Declarations.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_parse, an_empty_source_declares_nothing)
{
    expect_tree("", "");
    expect_tree(";;", "");
}

TB_TEST(shader_parse, global_variables_have_modifiers_semantics_and_initializers)
{
    expect_tree("static const float PI = 3.14159;", "(variable static const float PI = 3.14159)\n");
    expect_tree("float a, b[2], c = 1;",
                "(variable float a)\n(variable float[2] b)\n(variable float c = 1)\n");
    expect_tree("Texture2D<float4> Albedo : register(t0, space1);",
                "(variable Texture2D<float4> Albedo :register(t0, space1))\n");
    expect_tree("static const int Table[2][3] = {{1, 2, 3}, {4, 5, 6,}};",
                "(variable static const int[2][3] Table = (list (list 1 2 3) (list 4 5 6)))\n");
    expect_tree("groupshared float Cache[];", "(variable groupshared float[] Cache)\n");
}

TB_TEST(shader_parse, functions_have_parameters_semantics_and_bodies)
{
    expect_tree("float4 main(float4 position : SV_Position, in out float2 uv) : SV_Target"
                " { return position; }",
                "(function float4 main ((parameter float4 position :SV_Position) "
                "(parameter in out float2 uv)) :SV_Target (block (return position)))\n");
    expect_tree("void f(void);", "(function void f ())\n");
    expect_tree("float g(float, float x = 2);",
                "(function float g ((parameter float) (parameter float x = 2)))\n");
}

TB_TEST(shader_parse, compute_shaders_have_attributes)
{
    expect_tree("RWTexture2D<unorm float4> Output : register(u0);\n"
                "[numthreads(8, 8, 1)]\n"
                "void main(uint3 id : SV_DispatchThreadID) { Output[id.xy] = 1; }",
                "(variable RWTexture2D<unorm float4> Output :register(u0))\n"
                "(function [numthreads(8, 8, 1)] void main ((parameter uint3 id "
                ":SV_DispatchThreadID)) (block (expr (= ([] Output (. id xy)) 1))))\n");
}

TB_TEST(shader_parse, attributes_may_be_written_doubled_with_a_namespace)
{
    expect_tree("[[vk::binding(0, 1)]] uniform Texture2D T;",
                "(variable [vk::binding(0, 1)] uniform Texture2D T)\n");
}

TB_TEST(shader_parse, structs_hold_members_methods_and_bit_fields)
{
    expect_tree(
        "struct Light { float3 position; nointerpolation uint flags : 4;"
        " float Intensity() { return 1; } };",
        "(struct Light (variable float3 position) (variable nointerpolation uint flags : 4) "
        "(function float Intensity () (block (return 1))))\n");
    expect_tree("struct VSOut { float4 position : SV_Position; } output;",
                "(struct VSOut (variable float4 position :SV_Position))\n"
                "(variable VSOut output)\n");
    expect_tree("struct { int a; } anonymous;",
                "(struct (variable int a))\n(variable <anonymous> anonymous)\n");
}

TB_TEST(shader_parse, constant_buffers_hold_variables_with_pack_offsets)
{
    expect_tree("cbuffer View : register(b0, space2) { float4x4 ViewProjection : packoffset(c0);"
                " float3 CameraPosition : packoffset(c4.x); };\n"
                "float4 f() { return CameraPosition.xyzz; }",
                "(cbuffer View :register(b0, space2) (variable float4x4 ViewProjection "
                ":packoffset(c0)) (variable float3 CameraPosition :packoffset((. c4 x))))\n"
                "(function float4 f () (block (return (. CameraPosition xyzz))))\n");
    expect_tree("tbuffer Bones { float4x3 Bone[64]; }",
                "(tbuffer Bones (variable float4x3[64] Bone))\n");
    expect_tree("struct Params { float scale; };\n"
                "ConstantBuffer<Params> Constants : register(b1);",
                "(struct Params (variable float scale))\n"
                "(variable ConstantBuffer<Params> Constants :register(b1))\n");
}

TB_TEST(shader_parse, typedefs_name_types)
{
    expect_tree("typedef float4 Color, Palette[4];\nColor c;",
                "(typedef float4 Color)\n(typedef float4[4] Palette)\n(variable Color c)\n");
    expect_tree("typedef struct { float x; } Point;\nPoint p;",
                "(struct (variable float x))\n(typedef <anonymous> Point)\n(variable Point p)\n");
    expect_tree("typedef vector<float, 3> float3_t;", "(typedef vector<float, 3> float3_t)\n");
}

TB_TEST(shader_parse, namespaces_qualify_their_names)
{
    expect_tree(
        "namespace Lighting { struct Light { float3 color; };"
        " float Falloff(float d) { return d; } }\n"
        "Lighting::Light L;\n"
        "float f() { return Lighting::Falloff(1); }",
        "(namespace Lighting (struct Light (variable float3 color)) (function float Falloff "
        "((parameter float d)) (block (return d))))\n"
        "(variable Lighting::Light L)\n"
        "(function float f () (block (return (call Lighting::Falloff 1))))\n");
    /* Inside a namespace, its names need no qualifier, even when it is reopened. */
    expect_tree("namespace N { struct S { int a; }; }\nnamespace N { S s; }\n::N::S t;",
                "(namespace N (struct S (variable int a)))\n(namespace N (variable S s))\n"
                "(variable ::N::S t)\n");
}

TB_TEST(shader_parse, enums_declare_their_enumerators)
{
    expect_tree("enum Mode { Off, On = 2, };\nint m = On;",
                "(enum Mode (Off) (On = 2))\n(variable int m = On)\n");
    expect_tree("enum class Quality : uint { Low, High };\nQuality q = Quality::High;",
                "(enum class Quality : uint (Low) (High))\n"
                "(variable Quality q = Quality::High)\n");
}

TB_TEST(shader_parse, templates_declare_structs_and_functions)
{
    expect_tree(
        "template<typename T> T Square(T x) { return x * x; }\n"
        "float f() { return Square<float>(2); }",
        "(function template((typename T)) T Square ((parameter T x)) (block (return (* x x))))\n"
        "(function float f () (block (return (call Square<float> 2))))\n");
    expect_tree("template<typename T, uint N> struct Array { T items[N]; };\nArray<float, 4> a;",
                "(struct template((typename T) (parameter uint N)) Array (variable T[N] items))\n"
                "(variable Array<float, 4> a)\n");
}

TB_TEST(shader_parse, interfaces_classes_and_operators)
{
    expect_tree("interface IShade { float3 Shade(float3 n); };\n"
                "class Lambert : IShade { float3 Shade(float3 n) { return n; } };",
                "(interface IShade (function float3 Shade ((parameter float3 n))))\n"
                "(struct Lambert : IShade (function float3 Shade ((parameter float3 n)) "
                "(block (return n))))\n");
    expect_tree("struct V { float x; V operator+(V o) { V r; r.x = x + o.x; return r; }"
                " float operator[](uint i) { return x; } };",
                "(struct V (variable float x) (function V operator+ ((parameter V o)) (block "
                "(declare (variable V r)) (expr (= (. r x) (+ x (. o x)))) (return r))) "
                "(function float operator[] ((parameter uint i)) (block (return x))))\n");
    /* A method defined outside its struct reads the struct's members by their own names. */
    expect_tree("struct S { float k; float Get(); };\nfloat S::Get() { return k; }",
                "(struct S (variable float k) (function float Get ()))\n"
                "(function float S::Get () (block (return k)))\n");
}

TB_TEST(shader_parse, pragmas_pass_through_in_order)
{
    expect_tree("#pragma pack_matrix(row_major)\nfloat4x4 M;\nvoid f() {\n#pragma warning(disable: "
                "3557)\n}",
                "(pragma pack_matrix(row_major))\n(variable float4x4 M)\n"
                "(function void f () (block (pragma warning(disable: 3557))))\n");
}

/* -------------------------------------------------------------------------
 * The stages.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_parse, geometry_and_tessellation_stages)
{
    expect_tree("struct V { float4 p : SV_Position; };\n"
                "[maxvertexcount(3)] void GS(triangle V input[3], inout TriangleStream<V> stream)"
                " { stream.Append(input[0]); stream.RestartStrip(); }",
                "(struct V (variable float4 p :SV_Position))\n"
                "(function [maxvertexcount(3)] void GS ((parameter triangle V[3] input) "
                "(parameter inout TriangleStream<V> stream)) (block (expr (call (. stream Append) "
                "([] input 0))) (expr (call (. stream RestartStrip)))))\n");
    expect_tree(
        "struct V { float4 p : SV_Position; };\n"
        "[domain(\"tri\")] [partitioning(\"fractional_odd\")] [outputcontrolpoints(3)]"
        " [patchconstantfunc(\"Constants\")]\n"
        "V HS(InputPatch<V, 3> patch, uint i : SV_OutputControlPointID) { return patch[i]; }",
        "(struct V (variable float4 p :SV_Position))\n"
        "(function [domain(\"tri\")] [partitioning(\"fractional_odd\")] "
        "[outputcontrolpoints(3)] [patchconstantfunc(\"Constants\")] V HS ((parameter "
        "InputPatch<V, 3> patch) (parameter uint i :SV_OutputControlPointID)) "
        "(block (return ([] patch i))))\n");
}

TB_TEST(shader_parse, mesh_and_amplification_stages)
{
    expect_tree(
        "struct V { float4 p : SV_Position; }; struct P { uint id; };\n"
        "[outputtopology(\"triangle\")] [numthreads(32, 1, 1)]\n"
        "void MS(in payload P p, out indices uint3 tris[64], out vertices V verts[64])"
        " { SetMeshOutputCounts(3, 1); }",
        "(struct V (variable float4 p :SV_Position))\n(struct P (variable uint id))\n"
        "(function [outputtopology(\"triangle\")] [numthreads(32, 1, 1)] void MS "
        "((parameter in payload P p) (parameter out indices uint3[64] tris) "
        "(parameter out vertices V[64] verts)) (block (expr (call SetMeshOutputCounts 3 1))))\n");
    expect_tree(
        "struct P { uint id; }; groupshared P s;\n"
        "[numthreads(1, 1, 1)] void AS() { DispatchMesh(1, 1, 1, s); }",
        "(struct P (variable uint id))\n(variable groupshared P s)\n"
        "(function [numthreads(1, 1, 1)] void AS () (block (expr (call DispatchMesh 1 1 1 s))))\n");
}

TB_TEST(shader_parse, ray_tracing_shaders_and_payloads)
{
    expect_tree(
        "struct [raypayload] Payload { float4 color : read(caller) : write(closesthit, miss); };\n"
        "RaytracingAccelerationStructure Scene : register(t0);\n"
        "[shader(\"raygeneration\")] void RayGen() { RayDesc ray; ray.TMin = 0;"
        " Payload p = (Payload)0; TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, p); }\n"
        "[shader(\"closesthit\")] void Hit(inout Payload p,"
        " BuiltInTriangleIntersectionAttributes a) { p.color = 1; }",
        "(struct [raypayload] Payload (variable float4 color :read(caller) "
        ":write(closesthit, miss)))\n"
        "(variable RaytracingAccelerationStructure Scene :register(t0))\n"
        "(function [shader(\"raygeneration\")] void RayGen () (block (declare (variable "
        "RayDesc ray)) (expr (= (. ray TMin) 0)) (declare (variable Payload p = (cast "
        "Payload 0))) (expr (call TraceRay Scene RAY_FLAG_NONE 0xFF 0 1 0 ray p))))\n"
        "(function [shader(\"closesthit\")] void Hit ((parameter inout Payload p) (parameter "
        "BuiltInTriangleIntersectionAttributes a)) (block (expr (= (. p color) 1))))\n");
    expect_tree("void f() { RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_FORCE_OPAQUE> q;"
                " while (q.Proceed()) {} dx::HitObject h; }",
                "(function void f () (block (declare (variable RayQuery<(| "
                "RAY_FLAG_CULL_NON_OPAQUE RAY_FLAG_FORCE_OPAQUE)> q)) (while (call (. q Proceed)) "
                "(block)) (declare (variable dx::HitObject h))))\n");
}

/* -------------------------------------------------------------------------
 * Statements.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_parse, branches_and_loops)
{
    expect_tree("void f(int n) { if (n) n = 1; else if (n > 2) {} else ; }",
                "(function void f ((parameter int n)) (block (if n (expr (= n 1)) (if (> n 2) "
                "(block) (empty)))))\n");
    expect_tree("void f() { [unroll(4)] for (int i = 0, j = 1; i < 4; i++, --j) { continue; } }",
                "(function void f () (block (for [unroll(4)] (declare (variable int i = 0) "
                "(variable int j = 1)) (< i 4) (, (post++ i) (pre-- j)) (block (continue)))))\n");
    expect_tree("void f() { int i; for (;;) break; for (i = 0; ; ) {} }",
                "(function void f () (block (declare (variable int i)) (for _ _ _ (break)) "
                "(for (expr (= i 0)) _ _ (block))))\n");
    expect_tree("void f(bool b) { [loop] while (b) discard; do { b = false; } while (b); }",
                "(function void f ((parameter bool b)) (block (while [loop] b (discard)) "
                "(do-while (block (expr (= b false))) b)))\n");
}

TB_TEST(shader_parse, switches_hold_labels)
{
    expect_tree(
        "int f(int m) { [branch] switch (m) { case 0: case 1 + 1: return 1; default: break; }"
        " return 0; }",
        "(function int f ((parameter int m)) (block (switch [branch] m (block (case 0) "
        "(case (+ 1 1)) (return 1) (default) (break))) (return 0)))\n");
}

TB_TEST(shader_parse, local_declarations_and_types)
{
    expect_tree("void f() { static const float k = 2; struct L { int a; } l; typedef int I; I i; }",
                "(function void f () (block (declare (variable static const float k = 2)) "
                "(declare (struct L (variable int a)) (variable L l)) (declare (typedef int I)) "
                "(declare (variable I i))))\n");
    /* Words that are modifiers only before a type are names elsewhere. */
    expect_tree("void f() { float sample = 1; sample = 2; int line; line++; }",
                "(function void f () (block (declare (variable float sample = 1)) "
                "(expr (= sample 2)) (declare (variable int line)) (expr (post++ line))))\n");
    /* A local name hides a type of the same name, as in C. */
    expect_tree("struct T { int a; };\nvoid f() { int T = 1; T * 2; }",
                "(struct T (variable int a))\n"
                "(function void f () (block (declare (variable int T = 1)) (expr (* T 2))))\n");
}

/* -------------------------------------------------------------------------
 * Expressions.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_parse, operators_bind_with_c_precedence)
{
    expect_tree("int x = 1 + 2 * 3 - 4 / 5 % 6;",
                "(variable int x = (- (+ 1 (* 2 3)) (% (/ 4 5) 6)))\n");
    expect_tree("bool b = 1 < 2 == 3 >= 4 && 5 | 6 ^ 7 & 8 || 9 << 1 > 10 >> 2 != 0;",
                "(variable bool b = (|| (&& (== (< 1 2) (>= 3 4)) (| 5 (^ 6 (& 7 8)))) "
                "(!= (> (<< 9 1) (>> 10 2)) 0)))\n");
    expect_tree(
        "void f(int a, int b) { a = b += 2; a = a ? b : b ? 1 : 2; a = -~!+b; }",
        "(function void f ((parameter int a) (parameter int b)) (block (expr (= a (+= b 2))) "
        "(expr (= a (? a b (? b 1 2)))) (expr (= a (- (~ (! (+ b))))))))\n");
    expect_tree("void f(int a) { a *= 1; a /= 1; a %= 1; a -= 1; a <<= 1; a >>= 1; a &= 1;"
                " a ^= 1; a |= 1; }",
                "(function void f ((parameter int a)) (block (expr (*= a 1)) (expr (/= a 1)) "
                "(expr (%= a 1)) (expr (-= a 1)) (expr (<<= a 1)) (expr (>>= a 1)) (expr (&= a 1)) "
                "(expr (^= a 1)) (expr (|= a 1))))\n");
}

TB_TEST(shader_parse, constructors_casts_and_members)
{
    expect_tree("float4 c = float4(1, 2, 3, 4).wzyx * (float4)0.5 + vector<float, 4>(c.xyz, 1);",
                "(variable float4 c = (+ (* (. (construct float4 1 2 3 4) wzyx) (cast float4 0.5)) "
                "(construct vector<float, 4> (. c xyz) 1)))\n");
    expect_tree(
        "ByteAddressBuffer B; float4 v = B.Load<float4>(16); uint n = B.Load(0) < 2;",
        "(variable ByteAddressBuffer B)\n(variable float4 v = (call (. B Load<float4>) 16))\n"
        "(variable uint n = (< (call (. B Load) 0) 2))\n");
    expect_tree("Buffer<vector<float, 4>> Data; StructuredBuffer<matrix<float, 4, 4>> M;",
                "(variable Buffer<vector<float, 4>> Data)\n"
                "(variable StructuredBuffer<matrix<float, 4, 4>> M)\n");
    expect_tree("int s = sizeof(float4) + sizeof s;",
                "(variable int s = (+ (sizeof float4) (sizeof s)))\n");
    expect_tree("RayQuery<(1 > 2) ? 1 : 0> q;", "(variable RayQuery<(? (> 1 2) 1 0)> q)\n");
}

TB_TEST(shader_parse, literals_keep_their_spelling)
{
    expect_tree("uint a[] = {0x1F, 017, 0b101, 42u, 7ull, 3L};",
                "(variable uint[] a = (list 0x1F 017 0b101 42u 7ull 3L))\n");
    expect_tree("float b[] = {1.5f, .5, 2., 1e3, 2.5e-3h, 1.0L, true, false};",
                "(variable float[] b = (list 1.5f .5 2. 1e3 2.5e-3h 1.0L true false))\n");
}

TB_TEST(shader_parse, literal_values)
{
    parsed out = {0};
    parse_source(&out,
                 "uint a = 0x1F; uint b = 017; uint c = 0b101; uint d = 18446744073709551615;");
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    const uint64_t expected[] = {31, 15, 5, UINT64_MAX};
    size_t index = 0;
    for (const tb_shader_node *node = out.syntax.declarations; node != NULL; node = node->next)
    {
        TB_TEST_REQUIRE_UINT(index, <, TB_TEST_COUNT_OF(expected));
        TB_TEST_REQUIRE_UINT(node->value->kind, ==, TB_SHADER_NODE_INTEGER);
        TB_TEST_EXPECT_UINT(node->value->integer, ==, expected[index]);
        index++;
    }
    TB_TEST_EXPECT_UINT(index, ==, TB_TEST_COUNT_OF(expected));
    finish(&out);
}

/* -------------------------------------------------------------------------
 * Errors.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_parse, syntax_errors_name_where_they_are)
{
    expect_error("float a = 1\nfloat b;", AT(2, 1) "expected ';', found 'float'");
    expect_error("Light l;", AT(1, 1) "unknown type 'Light'");
    expect_error("float x;\nx y;", AT(2, 1) "'x' is not a type");
    expect_error("void f() {", AT(1, 11) "expected '}' at the end of the file");
    expect_error("void f() { return 1 +; }", AT(1, 22) "expected an expression, found ';'");
    expect_error("}", AT(1, 1) "unexpected '}'");
    expect_error(")", AT(1, 1) "expected a declaration, found ')'");
    expect_error("float f(float x) : { }", AT(1, 20) "expected a semantic, found '{'");
    expect_error("struct S { 1; };", AT(1, 12) "expected a member, found '1'");
    expect_error("void f() { int x = 'a'; }", AT(1, 20) "HLSL has no character constants");
    expect_error("int x = 1 @ 2;", AT(1, 11) "expected ';', found '@'");
    expect_error("void f(struct S { int a; } s);", AT(1, 8) "a parameter cannot define a type");
    expect_error("void f() { namespace N {} }",
                 AT(1, 12) "'namespace' is allowed only at namespace scope");
    expect_error("void f() { template<typename T> T g(); }",
                 AT(1, 12) "a template cannot be declared here");
    expect_error("template<typename T> T value;",
                 AT(1, 1) "a template declares a struct or a function");
    expect_error("float;", AT(1, 6) "expected a name, found ';'");
    expect_error("[numthreads(1, 1, 1) void f();", AT(1, 22) "expected ']', found 'void'");
}

TB_TEST(shader_parse, bad_literals_are_errors)
{
    expect_error("int a = 1f;", AT(1, 9) "invalid suffix 'f' on an integer");
    expect_error("float b = 1.5q;", AT(1, 11) "invalid suffix 'q' on a floating-point number");
    expect_error("int c = 019;", AT(1, 9) "invalid digit '9' in an octal number");
    expect_error("int d = 18446744073709551616;",
                 AT(1, 9) "the integer '18446744073709551616' does not fit in 64 bits");
    expect_error("int e = 0x;", AT(1, 9) "invalid suffix 'x' on an integer");
    expect_error("int f = 1uu;", AT(1, 9) "invalid suffix 'uu' on an integer");
}

TB_TEST(shader_parse, errors_in_included_files_name_those_files)
{
    parsed out = {0};
    out.arena = tb_arena_create(0);
    tb_shader_file files[2] = {0};
    files[0].path = MAIN_PATH;
    files[0].text = "#include \"/Engine/Public/Common.hlsli\"\nfloat ok;";
    files[0].length = tb_str_length(files[0].text);
    files[1].path = "/Engine/Public/Common.hlsli";
    files[1].text = "float a = b\n";
    files[1].length = tb_str_length(files[1].text);
    tb_shader_preprocess_request request = {0};
    request.path = MAIN_PATH;
    request.files = files;
    request.file_count = 2;
    tb_shader_unit unit = {0};
    TB_TEST_EXPECT_UINT(tb_shader_preprocess_unit(&request, out.arena, &unit), ==, TB_SHADER_OK);
    TB_TEST_EXPECT_UINT(tb_shader_parse(&unit, out.arena, &out.syntax), ==, TB_SHADER_FAILED);
    char message[256] = {0};
    diagnostic_text(&out, 0, message, sizeof(message));
    /* The error is at the token that follows the missing `;`, in the main file. */
    TB_TEST_EXPECT_STRING(message, ==,
                          "/Engine/Private/Main.hlsl(2,1): error: expected ';', found 'float'");
    finish(&out);

    out.arena = tb_arena_create(0);
    files[1].text = "float a = @;\n";
    files[1].length = tb_str_length(files[1].text);
    TB_TEST_EXPECT_UINT(tb_shader_preprocess_unit(&request, out.arena, &unit), ==, TB_SHADER_OK);
    TB_TEST_EXPECT_UINT(tb_shader_parse(&unit, out.arena, &out.syntax), ==, TB_SHADER_FAILED);
    diagnostic_text(&out, 0, message, sizeof(message));
    TB_TEST_EXPECT_STRING(
        message, ==, "/Engine/Public/Common.hlsli(1,11): error: expected an expression, found '@'");
    finish(&out);
}

TB_TEST(shader_parse, the_parser_recovers_to_report_more_errors)
{
    parsed out = {0};
    parse_source(&out, "void f() { int a = ; a = 1; }\n"
                       "void g() { b c; return; }\n"
                       "Unknown u;\n"
                       "float ok;");
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.syntax.diagnostic_count, ==, 3);
    char message[256] = {0};
    diagnostic_text(&out, 0, message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==, AT(1, 20) "expected an expression, found ';'");
    diagnostic_text(&out, 1, message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==, AT(2, 14) "expected ';', found 'c'");
    diagnostic_text(&out, 2, message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==, AT(3, 1) "unknown type 'Unknown'");
    /* What follows each error is still read. */
    TB_TEST_EXPECT_STRING(out.tree, ==,
                          "(function void f () (block (declare (variable int a)) "
                          "(expr (= a 1))))\n"
                          "(function void g () (block (expr b) (return)))\n"
                          "(variable float ok)\n");
    finish(&out);
}

TB_TEST(shader_parse, a_compile_stops_after_too_many_errors)
{
    char source[4096] = {0};
    size_t length = 0;
    for (size_t index = 0; index < TB_SHADER_MAX_ERRORS + 10; index++)
    {
        memcpy(source + length, "x y;\n", 5);
        length += 5;
    }
    parsed out = {0};
    parse_source(&out, source);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_EXPECT_UINT(out.syntax.error_count, ==, TB_SHADER_MAX_ERRORS);
    TB_TEST_REQUIRE_UINT(out.syntax.diagnostic_count, ==, TB_SHADER_MAX_ERRORS + 1);
    char message[256] = {0};
    diagnostic_text(&out, TB_SHADER_MAX_ERRORS, message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==, AT(64, 1) "too many errors; stopping");
    finish(&out);
}

/* -------------------------------------------------------------------------
 * Limits.
 * ------------------------------------------------------------------------- */

/* `int x = `, `open` `count` times, a 1, and `close` as many times. */
static char *nested_source(tb_arena *arena, const char *open, const char *close, size_t count)
{
    const size_t open_length = tb_str_length(open);
    const size_t close_length = tb_str_length(close);
    char *const source = tb_arena_alloc(arena, 16 + count * (open_length + close_length), 1);
    size_t length = 0;
    memcpy(source + length, "int x = ", 8);
    length += 8;
    for (size_t index = 0; index < count; index++)
    {
        memcpy(source + length, open, open_length);
        length += open_length;
    }
    source[length++] = '1';
    for (size_t index = 0; index < count; index++)
    {
        memcpy(source + length, close, close_length);
        length += close_length;
    }
    memcpy(source + length, ";", 2);
    return source;
}

/* A source that must fail with one error, which mentions `expected`. */
static void expect_one_error(const char *source, const char *expected)
{
    parsed out = {0};
    parse_source(&out, source);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.syntax.diagnostic_count, ==, 1);
    TB_TEST_CONTEXT("message: %s", out.syntax.diagnostics[0].message);
    TB_TEST_EXPECT(tb_str_find(out.syntax.diagnostics[0].message, 0, TB_STR_END, expected) !=
                   TB_STR_NOT_FOUND);
    finish(&out);
}

TB_TEST(shader_parse, expression_nesting_is_bounded)
{
    tb_arena *const arena = tb_arena_create(0);
    /* Deep, but within the limit. */
    expect_tree(nested_source(arena, "(", ")", 100), "(variable int x = 1)\n");
    expect_one_error(nested_source(arena, "(", ")", 10000), "nesting is deeper than 256 levels");
    expect_one_error(nested_source(arena, "-", "", 10000), "nesting is deeper than 256 levels");
    expect_one_error(nested_source(arena, "{", "}", 10000), "nesting is deeper than 256 levels");
    expect_one_error(nested_source(arena, "x = ", "", 10000), "nesting is deeper than 256 levels");
    tb_arena_destroy(arena);
}

TB_TEST(shader_parse, statement_and_type_nesting_is_bounded)
{
    tb_arena *const arena = tb_arena_create(0);
    const char *const prefixes[] = {"void f() ", "void f() { ", "Buffer<", "namespace A { "};
    const char *const repeated[] = {"{", "if (1) ", "Buffer<", "namespace A { "};
    for (size_t kind = 0; kind < TB_TEST_COUNT_OF(repeated); kind++)
    {
        const size_t count = 10000;
        const size_t prefix = tb_str_length(prefixes[kind]);
        const size_t step = tb_str_length(repeated[kind]);
        char *const source = tb_arena_alloc(arena, prefix + count * step + 1, 1);
        memcpy(source, prefixes[kind], prefix);
        for (size_t index = 0; index < count; index++)
        {
            memcpy(source + prefix + index * step, repeated[kind], step);
        }
        source[prefix + count * step] = '\0';
        TB_TEST_CONTEXT("nesting: %s", repeated[kind]);
        expect_one_error(source, "nesting is deeper than 256 levels");
    }
    tb_arena_destroy(arena);
}

TB_TEST(shader_parse, expressions_are_bounded_in_height)
{
    tb_arena *const arena = tb_arena_create(0);
    /* `1 + 1 + ...` nests in the tree, not in the parser. */
    const size_t terms = TB_SHADER_MAX_EXPRESSION_HEIGHT + 10;
    char *const source = tb_arena_alloc(arena, 16 + terms * 4, 1);
    size_t length = 0;
    memcpy(source, "int x = 1", 9);
    length = 9;
    for (size_t index = 1; index < terms; index++)
    {
        memcpy(source + length, " + 1", 4);
        length += 4;
    }
    memcpy(source + length, ";", 2);
    expect_one_error(source, "the expression is more than 1024 operations deep");
    tb_arena_destroy(arena);
}
