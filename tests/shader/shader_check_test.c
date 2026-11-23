/* Checking shaders: the type each expression has, the conversions HLSL
 * makes and the warnings they earn, overloads, names and scopes, constants,
 * and the errors DXC reports for what HLSL does not allow. Trees are
 * compared as tb_shader_syntax_print_typed() writes them. */
#include "badger/arena.h"
#include "badger/fmt.h"
#include "badger/shader.h"
#include "badger/str.h"
#include "shader/shader_check.h"
#include "shader/shader_check_internal.h"
#include "tb_test.h"

#define MAIN_PATH "/Engine/Private/Main.hlsl"
#define AT(line, column) MAIN_PATH "(" #line "," #column "): "

typedef struct checked_source
{
    tb_arena *arena;
    tb_shader_result result;
    tb_shader_syntax syntax;
    tb_shader_checked checked;
    const char *tree;
} checked_source;

static void check_source(checked_source *out, const char *source)
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
    TB_TEST_EXPECT_UINT(tb_shader_preprocess_unit(&request, out->arena, &unit), ==, TB_SHADER_OK);
    const tb_shader_result parsed = tb_shader_parse(&unit, out->arena, &out->syntax);
    if (parsed != TB_SHADER_OK && out->syntax.diagnostic_count > 0)
    {
        TB_TEST_CONTEXT("parse error: %s", out->syntax.diagnostics[0].message);
    }
    TB_TEST_EXPECT_UINT(parsed, ==, TB_SHADER_OK);
    out->result = tb_shader_check(&out->syntax, out->arena, &out->checked);
    out->tree = tb_shader_syntax_print_typed(&out->syntax, out->arena);
}

static void finish(checked_source *out)
{
    tb_arena_destroy(out->arena);
}

static void diagnostic_text(const checked_source *out, size_t index, char *buffer, size_t size)
{
    TB_TEST_REQUIRE_UINT(out->checked.diagnostic_count, >, index);
    (void)tb_shader_diagnostic_format(&out->checked.diagnostics[index], buffer, size);
}

/* A source that checks with no diagnostics into `expected`. */
static void expect_typed(const char *source, const char *expected)
{
    checked_source out = {0};
    check_source(&out, source);
    TB_TEST_CONTEXT("source: %s", source);
    if (out.checked.diagnostic_count > 0)
    {
        char message[256] = {0};
        diagnostic_text(&out, 0, message, sizeof(message));
        TB_TEST_CONTEXT("first diagnostic: %s", message);
    }
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    TB_TEST_EXPECT_UINT(out.checked.diagnostic_count, ==, 0);
    TB_TEST_EXPECT_STRING(out.tree, ==, expected);
    finish(&out);
}

/* A source whose first diagnostic is `expected`; FAILED when it is an error. */
static void expect_diagnostic(const char *source, const char *expected)
{
    checked_source out = {0};
    check_source(&out, source);
    TB_TEST_CONTEXT("source: %s", source);
    char message[256] = {0};
    diagnostic_text(&out, 0, message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==, expected);
    const bool is_error = out.checked.diagnostics[0].severity == TB_SHADER_ERROR;
    TB_TEST_EXPECT_UINT(out.result, ==, is_error ? TB_SHADER_FAILED : TB_SHADER_OK);
    finish(&out);
}

/* A source with no diagnostics. */
static void expect_clean(const char *source)
{
    checked_source out = {0};
    check_source(&out, source);
    TB_TEST_CONTEXT("source: %s", source);
    if (out.checked.diagnostic_count > 0)
    {
        char message[256] = {0};
        diagnostic_text(&out, 0, message, sizeof(message));
        TB_TEST_CONTEXT("first diagnostic: %s", message);
    }
    TB_TEST_EXPECT_UINT(out.checked.diagnostic_count, ==, 0);
    finish(&out);
}

/* -------------------------------------------------------------------------
 * Literals and conversions.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_check, literals_take_the_type_their_use_asks_for)
{
    /* As in DXC, an unsuffixed literal is a "literal" type until used. */
    expect_typed("static const float a = 1;",
                 "(variable static const float a = (convert 1:literal int):float)\n");
    expect_typed(
        "static const uint b = 2u; static const half c = 1.5h; static const double d = 1.0l;",
        "(variable static const uint b = 2u:uint)\n"
        "(variable static const half c = 1.5h:half)\n"
        "(variable static const double d = 1.0l:double)\n");
    expect_typed("static const int64_t e = 1l; static const uint64_t f = 1ul;",
                 "(variable static const int64_t e = 1l:int64_t)\n"
                 "(variable static const uint64_t f = 1ul:uint64_t)\n");
}

TB_TEST(shader_check, a_lost_fraction_is_a_warning)
{
    expect_diagnostic("static const int a = 1.5;",
                      AT(1, 22) "warning: converting 1.5 to 'int' drops its fraction");
}

TB_TEST(shader_check, arithmetic_converts_to_the_common_type)
{
    expect_typed("float3 f(float3 a, float b) { return a * b + 1; }",
                 "(function float3 f ((parameter float3 a) (parameter float b)) (block (return "
                 "(+ (* a:float3 (convert b:float):float3):float3 (convert 1:literal "
                 "int):float3):float3)))\n");
    expect_typed("uint f(int a, uint b) { return a + b; }",
                 "(function uint f ((parameter int a) (parameter uint b)) (block (return "
                 "(+ (convert a:int):uint b:uint):uint)))\n");
    expect_typed("float f(int a) { return a + 0.5; }",
                 "(function float f ((parameter int a)) (block (return "
                 "(+ (convert a:int):float (convert 0.5:literal float):float):float)))\n");
    expect_typed("int f(bool a) { return a + a; }",
                 "(function int f ((parameter bool a)) (block (return "
                 "(+ (convert a:bool):int (convert a:bool):int):int)))\n");
}

TB_TEST(shader_check, comparisons_give_bools_of_the_operands_shape)
{
    expect_typed("bool2 f(float2 a, int2 b) { return a < b; }",
                 "(function bool2 f ((parameter float2 a) (parameter int2 b)) (block (return "
                 "(< a:float2 (convert b:int2):float2):bool2)))\n");
}

TB_TEST(shader_check, vectors_truncate_with_a_warning_and_never_extend)
{
    expect_diagnostic("float2 f(float4 v) { return v; }",
                      AT(1, 29) "warning: 'float4' is truncated to 'float2'");
    expect_diagnostic(
        "float f(float2 a, float3 b) { return (a + b).x; }",
        AT(1, 41) "warning: 'float2' and 'float3' are combined as their first 2 components");
    expect_diagnostic("float3 f(float2 v) { return v; }",
                      AT(1, 29) "error: cannot convert from 'float2' to 'float3'");
    expect_diagnostic("float3 f(float3x3 m, float3 v) { return m + v; }",
                      AT(1, 43) "error: cannot combine 'float3x3' and 'float3'");
    /* A vector and a matrix of as many components convert to each other. */
    expect_clean("float4 f(float2x2 m) { float4 v = m; return v; }");
    expect_clean("float2x2 f(float4 v) { return (float2x2)v; }");
}

TB_TEST(shader_check, hlsl_2021_logic_takes_scalars)
{
    expect_diagnostic(
        "bool2 f(bool2 a, bool2 b) { return a && b; }",
        AT(1, 36) "error: '&&' and '||' take scalars, not 'bool2'; use and() or or() for vectors");
    expect_diagnostic("float2 f(bool2 c, float2 x) { return c ? x : 2; }",
                      AT(1, 38) "error: the condition of '?:' must be a scalar, not 'bool2'; use "
                                "select() for vectors");
    expect_diagnostic(
        "float f(float a) { if (float2(a, a)) return 1; return 0; }",
        AT(1, 24) "error: the condition of an if statement must be a scalar, not 'float2'");
    expect_diagnostic("float f(float a) { return a & 2; }",
                      AT(1, 29) "error: '&' takes integers, not 'float'");
    /* `%` takes floats in HLSL. */
    expect_clean("float f(float a) { return a % 2; }");
}

/* -------------------------------------------------------------------------
 * Swizzles, members and indexing.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_check, swizzles_select_components)
{
    expect_typed(
        "float3 f(float4 v) { return v.bgr; }",
        "(function float3 f ((parameter float4 v)) (block (return (. v:float4 bgr):float3)))\n");
    expect_typed(
        "float3 f(float s) { return s.xxx; }",
        "(function float3 f ((parameter float s)) (block (return (. s:float xxx):float3)))\n");
    expect_typed("float2 f(float2x2 m) { return m._m00_m11 + m._12_21 + m[1]; }",
                 "(function float2 f ((parameter float2x2 m)) (block (return (+ (+ (. m:float2x2 "
                 "_m00_m11):float2 (. m:float2x2 _12_21):float2):float2 ([] m:float2x2 (convert "
                 "1:literal int):uint):float2):float2)))\n");
    expect_diagnostic("float f(float2 a) { return a.xy.z; }",
                      AT(1, 32) "error: 'z' is out of range for 'float2'");
    expect_diagnostic("float2 f(float4 a) { return a.xg; }",
                      AT(1, 30) "error: a swizzle cannot mix xyzw and rgba");
    expect_diagnostic("void f(inout float2 a) { a.xx = 1; }",
                      AT(1, 27) "error: a swizzle that repeats a component cannot be assigned");
    expect_diagnostic("float f(float2x2 m) { return m._33; }",
                      AT(1, 31) "error: '_33' is out of range for 'float2x2'");
    expect_diagnostic("float f(float2 a) { return a.q; }",
                      AT(1, 29) "error: 'float2' has no member 'q'");
}

TB_TEST(shader_check, structs_have_fields_methods_and_bases)
{
    expect_typed(
        "struct Light { float3 color; float scale; float3 Scaled() { return color * scale; } };\n"
        "float3 f(Light l) { return l.Scaled() + l.color; }",
        "(struct Light (variable float3 color) (variable float scale) (function float3 Scaled () "
        "(block (return (* color:float3 (convert scale:float):float3):float3))))\n"
        "(function float3 f ((parameter Light l)) (block (return (+ (call (. l:Light "
        "Scaled)):float3 "
        "(. l:Light color):float3):float3)))\n");
    expect_clean("struct Base { float a; }; struct Derived : Base { float b; };\n"
                 "float f(Derived d) { return d.a + d.b; }");
    expect_clean("struct S { float k; float Get(); float Twice() { return this.k * 2; } };\n"
                 "float S::Get() { return k; }\n"
                 "float f(S s) { return s.Get() + s.Twice(); }");
    expect_diagnostic("struct S { float a; }; float f(S s) { return s.b; }",
                      AT(1, 47) "error: 'S' has no member 'b'");
    expect_diagnostic("struct S { float a; float a; };", AT(1, 27) "error: duplicate member 'a'");
    expect_diagnostic("struct S { S inner; };",
                      AT(1, 14) "error: 'S' is used before its declaration is complete");
    expect_diagnostic("struct S { float a = 1; };",
                      AT(1, 22) "error: a struct's member cannot have an initializer");
    expect_diagnostic("struct S { float a; }; float f(S s) { return s.a(); }",
                      AT(1, 47) "error: 'S' has no method 'a'");
    /* A static method is named by its struct; a method with an object is not. */
    expect_clean("struct S { static int twice(int x) { return 2 * x; } }; int f() { return "
                 "S::twice(3); }");
    expect_diagnostic("struct S { int n; int get() { return n; } }; int f() { return S::get(); }",
                      AT(1, 63) "error: 'S::get' is not declared");
}

TB_TEST(shader_check, indexing_reads_elements)
{
    expect_typed("float f(float a[3], int i) { return a[i]; }",
                 "(function float f ((parameter float[3] a) (parameter int i)) (block (return "
                 "([] a:float[3] (convert i:int):uint):float)))\n");
    expect_diagnostic("float f(float a[3]) { return a[3]; }",
                      AT(1, 32) "error: index 3 is past the end of 'float[3]'");
    expect_diagnostic("float f(float a) { return a[0]; }",
                      AT(1, 28) "error: 'float' cannot be indexed");
    expect_diagnostic("float f(float a[3]) { return a[1.5]; }",
                      AT(1, 32) "warning: converting 1.5 to 'uint' drops its fraction");
    expect_diagnostic("float f(float a[3]) { return a[int2(0, 1)]; }",
                      AT(1, 32) "error: an index must be an integer, not 'int2'");
    /* DXC takes a bool as an index. */
    expect_clean("float f(float a[3]) { return a[true]; }");
}

TB_TEST(shader_check, resources_read_and_write_by_index)
{
    expect_typed("Texture2D<float4> T; RWTexture2D<float> O;\nvoid f(uint2 p) { O[p] = T[p].x; }",
                 "(variable Texture2D<float4> T)\n(variable RWTexture2D<float> O)\n"
                 "(function void f ((parameter uint2 p)) (block (expr (= ([] O:RWTexture2D<float> "
                 "p:uint2):float (. ([] T:Texture2D<float4> p:uint2):float4 x):float):float)))\n");
    expect_diagnostic("Texture2D T; void f(uint2 p) { T[p] = 1; }",
                      AT(1, 33) "error: 'Texture2D<float4>' is read-only");
    expect_clean("struct Light { float3 color; }; StructuredBuffer<Light> Lights; "
                 "ConstantBuffer<Light> One;\n"
                 "float3 f(uint i) { return Lights[i].color + One.color; }");
    expect_clean(
        "Buffer<uint> B; Texture2DMS<float4, 4> M; InputPatch<float4, 3> P; RayQuery<1, 2> Q;"
        " SamplerState S; RaytracingAccelerationStructure A;");
}

TB_TEST(shader_check, resource_templates_are_checked)
{
    expect_diagnostic(
        "struct S { float a; }; Texture2D<S> T;",
        AT(1, 24) "error: the element of 'Texture2D' must be a scalar or a vector, not 'S'");
    expect_diagnostic(
        "ConstantBuffer<float4> C;",
        AT(1, 1) "error: the element of 'ConstantBuffer' must be a struct, not 'float4'");
    expect_diagnostic("StructuredBuffer S;",
                      AT(1, 1) "error: 'StructuredBuffer' takes 1 template argument, not 0");
    expect_diagnostic("InputPatch<float4, 40> P;",
                      AT(1, 20) "error: the template argument must be from 1 to 32, not 40");
    expect_typed("vector<int, 3> v; matrix<half, 2, 3> m; vector w;",
                 "(variable int3 v)\n(variable half2x3 m)\n(variable float4 w)\n");
}

/* The first error of a source, without where it is. */
static void expect_message(const char *source, const char *expected)
{
    checked_source out = {0};
    check_source(&out, source);
    TB_TEST_CONTEXT("source: %s", source);
    TB_TEST_REQUIRE_UINT(out.checked.diagnostic_count, >, 0);
    TB_TEST_EXPECT_STRING(out.checked.diagnostics[0].message, ==, expected);
    finish(&out);
}

TB_TEST(shader_check, written_types_are_checked)
{
    expect_message("vector<float2, 2> v;",
                   "the components of a vector or matrix must be scalars, not 'float2'");
    expect_message("vector<4, 4> v;", "expected a type as a template argument");
    expect_message("vector<float, float> v;", "expected a constant as a template argument");
    expect_message("vector<float, 5> v;", "the template argument must be from 1 to 4, not 5");
    expect_message("matrix<float, 2> m;", "'matrix' takes 3 template arguments, not 2");
    expect_message("Texture2D<float4, 2> t;",
                   "'Texture2D' takes from 0 to 1 template arguments, not 2");
    expect_message("Texture2DMS<float4, 200> t;",
                   "the template argument must be from 1 to 128, not 200");
    expect_message("struct S { float a; }; Texture2DMS<S> t;",
                   "the element of 'Texture2DMS' must be a scalar or a vector, not 'S'");
    expect_message("InputPatch<float4> p;", "'InputPatch' takes 2 template arguments, not 1");
    expect_message("RayQuery<float> q;", "expected a constant as a template argument");
    expect_message("RayQuery<1, float> q;", "expected a constant as a template argument");
    expect_message("StructuredBuffer<SamplerState> b;",
                   "'StructuredBuffer' cannot hold 'SamplerState'");
    expect_message("float a[2][];", "only the first size of an array may be left out");
    expect_message("void a[2];", "an array cannot hold 'void'");
    expect_message("void v;", "a variable cannot be 'void'");
    expect_typed("string s; RayDesc r; BuiltInTriangleIntersectionAttributes b; uint8_t4_packed p;"
                 " Texture2DMS<float4> m; RayQuery<5> q; AppendStructuredBuffer<float4> a;",
                 "(variable string s)\n(variable RayDesc r)\n"
                 "(variable BuiltInTriangleIntersectionAttributes b)\n(variable uint p)\n"
                 "(variable Texture2DMS<float4> m)\n(variable RayQuery<5> q)\n"
                 "(variable AppendStructuredBuffer<float4> a)\n");
    expect_typed("typedef float3 Color; typedef Color Palette[2]; Palette p;",
                 "(typedef float3 Color)\n(typedef float3[2] Palette)\n(variable float3[2] p)\n");
}

TB_TEST(shader_check, declarations_are_checked)
{
    expect_message("float N; namespace N { }", "redefinition of 'N'");
    expect_message("float a; float a;", "redefinition of 'a'");
    expect_message("struct S { uint a : 33; };",
                   "a bit field of 'uint' must be from 1 to 32 bits wide, not 33");
    expect_message("struct S { float a : 3; };", "a bit field must be an integer, not 'float'");
    expect_message("struct S { static float a; };", "static members are not supported");
    expect_message("struct S { float a[]; };", "a member's array needs a size");
    expect_message("struct D : float { };", "'float' cannot be a base");
    expect_message("float x; float x::f() { return 1; }", "'x' is not a struct");
    expect_message("struct S { float f(); }; float S::g() { return 1; }",
                   "'S' declares no method 'g' like this");
    expect_message("struct S { float f() { return 1; } }; float S::f() { return 2; }",
                   "redefinition of 'S::f'");
    expect_message("struct S { float f; float f() { return 1; } };",
                   "'f' is already a member of 'S'");
    expect_message("void f(float a[]) { }", "a parameter's array needs a size");
    expect_message("void f(void v, float a) { }", "a parameter cannot be 'void'");
    expect_message("static groupshared float g;",
                   "a variable cannot be both static and groupshared");
    expect_message("void f() { groupshared float g; }",
                   "only 'static' may set where a local variable lives");
    expect_message("cbuffer C { static float g; }",
                   "a buffer's member cannot be static, groupshared or extern");
    expect_message("void f() { void g(); }", "a function cannot be declared in a function");
    expect_message("struct S { template<typename T> T g(T x) { return x; } };",
                   "templates are not supported yet");
    expect_message("template<typename T> struct S { T a; };", "templates are not supported yet");
    expect_clean("enum E { A = 2, B, C = B + 1 }; static const int x[C] = {1, 2, 4, 8};");
    expect_clean("void f() { struct L { int a; } l; l.a = 1; enum M { P, Q } m = Q; typedef int I; "
                 "I i = l.a; }");
    expect_clean("struct S { float a; struct Inner { float b; } inner; }; float f(S s) { return "
                 "s.inner.b; }");
}

/* -------------------------------------------------------------------------
 * Constructors, casts and initializers.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_check, constructors_count_components)
{
    expect_typed(
        "float4 f(float2 v) { return float4(v, 1, 2); }",
        "(function float4 f ((parameter float2 v)) (block (return (construct float4 v:float2 "
        "(convert 1:literal int):float (convert 2:literal int):float):float4)))\n");
    expect_diagnostic("float4 f() { return float4(1); }",
                      AT(1, 21) "error: 'float4' needs 4 components, but 1 was given");
    expect_diagnostic("float2 f(float3 v) { return float2(v); }",
                      AT(1, 29) "error: 'float2' needs 2 components, but 3 were given");
}

TB_TEST(shader_check, casts_convert_component_by_component)
{
    expect_clean("struct S { float a; float b; }; S f() { return (S)0; }");
    expect_clean("struct S { float a; float b; }; S f(float2 v) { return (S)v; }");
    expect_diagnostic("struct S { float a; float b; }; S f(float2 v) { S s = v; return s; }",
                      AT(1, 55) "error: cannot convert from 'float2' to 'S'");
    expect_diagnostic("Texture2D T; float f() { return (float)T; }",
                      AT(1, 33) "error: cannot cast from 'Texture2D<float4>' to 'float'");
}

TB_TEST(shader_check, initializer_lists_flatten)
{
    expect_typed("static const float a[] = {1, 2, 3};",
                 "(variable static const float[3] a = (list 1:literal int 2:literal int 3:literal "
                 "int):float[3])\n");
    expect_clean(
        "void f(float a) { float2 b[2] = {1, 2, {a, a}}; float4 c = {a, a, float2(a, a)}; }");
    expect_diagnostic("static const float a[3] = {1, 2};",
                      AT(1, 27) "error: 'float[3]' needs 3 values, but 2 were given");
    expect_diagnostic("static const float2 a[] = {1, 2, 3};",
                      AT(1, 27) "error: 3 values do not make whole elements of 'float2'");
}

/* -------------------------------------------------------------------------
 * Calls.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_check, overloads_are_chosen_as_dxc_chooses)
{
    /* A uint prefers an int to a float. */
    expect_typed("float g(float a) { return a; } float g(int a) { return a; }\n"
                 "float f() { return g(1u); }",
                 "(function float g ((parameter float a)) (block (return a:float)))\n"
                 "(function float g ((parameter int a)) (block (return (convert a:int):float)))\n"
                 "(function float f () (block (return (call g (convert 1u:uint):int):float)))\n");
    /* A scalar prefers a scalar to a vector. */
    expect_clean("float g(float2 a) { return a.x; } float g(float a) { return a; } float f() { "
                 "return g(1); }");
    /* A literal is as far from float as from double. */
    expect_diagnostic("float g(float a) { return a; } float g(double a) { return (float)a; }\n"
                      "float f() { return g(1); }",
                      AT(2, 20) "error: the call to 'g' with (literal int) is ambiguous");
    expect_diagnostic("float g(float4 a) { return a.x; } float f(float2 v) { return g(v); }",
                      AT(1, 62) "error: no 'g' takes (float2)");
    expect_clean(
        "float g(float a, float b = 2) { return a + b; } float f() { return g(1) + g(1, 3); }");
}

TB_TEST(shader_check, outputs_must_be_assignable)
{
    expect_diagnostic("void f(out float a) { a = 1; } void g() { f(1); }",
                      AT(1, 45) "error: argument 1 of 'f' is an output, so it must be assignable");
    expect_clean("void f(out float a, inout float2 b) { a = b.x; } void g() { float x; float2 y = "
                 "0; f(x, y); }");
}

TB_TEST(shader_check, functions_are_declared_once)
{
    expect_clean("float f(float a); float f(float a) { return a; } float g() { return f(1); }");
    expect_diagnostic("float f() { return 1; } float f() { return 2; }",
                      AT(1, 31) "error: redefinition of 'f'");
    expect_diagnostic("float f(); int f();",
                      AT(1, 16) "error: 'f' is declared again returning 'int', not 'float'");
    expect_diagnostic("float f() { return g(); }", AT(1, 20) "error: 'g' is not declared");
    expect_diagnostic("float x; float f() { return x(); }",
                      AT(1, 29) "error: 'x' is not a function");
}

/* -------------------------------------------------------------------------
 * Names.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_check, names_are_scoped)
{
    expect_clean("float f(float a) { { float a = 2; } float b = a; { float b = 1; } return b; }");
    expect_diagnostic("void f() { int x; int x; }", AT(1, 23) "error: redefinition of 'x'");
    expect_diagnostic("float f() { return y; }", AT(1, 20) "error: 'y' is not declared");
    expect_diagnostic("float g() { return 1; } float f() { return g; }",
                      AT(1, 44) "error: 'g' is a function, and must be called");
    expect_diagnostic("float f() { return this; }",
                      AT(1, 20) "error: 'this' is used outside a method");
}

TB_TEST(shader_check, shader_inputs_and_constants_cannot_be_assigned)
{
    expect_diagnostic("float g; void f() { g = 1; }",
                      AT(1, 21) "error: 'g' is a shader input, and cannot be assigned");
    expect_diagnostic("cbuffer C { float4 tint; } void f() { tint.x = 1; }",
                      AT(1, 39) "error: 'tint' is a shader input, and cannot be assigned");
    expect_diagnostic("void f() { const int k = 1; k = 2; }",
                      AT(1, 29) "error: 'k' is const, and cannot be assigned");
    expect_diagnostic("void f() { 1 = 2; }", AT(1, 12) "error: this expression cannot be assigned");
    expect_clean("static float g; groupshared float s[4]; void f() { g = 1; s[0] = g; }");
}

TB_TEST(shader_check, namespaces_and_enums)
{
    expect_typed("namespace N { enum class Quality { Low, High = 4 }; }\n"
                 "static const N::Quality q = N::Quality::High;",
                 "(namespace N (enum class Quality (Low) (High = 4:literal int)))\n"
                 "(variable static const N::Quality q = N::Quality::High:N::Quality)\n");
    expect_clean("enum Mode { Off, On }; int f() { return On + 1; }");
    expect_diagnostic("enum class Mode { Off, On }; int f() { return Mode::On; }",
                      AT(1, 47) "error: cannot convert from 'Mode' to 'int'");
    expect_diagnostic("enum E : float { A };",
                      AT(1, 10) "error: an enum's type must be an integer, not 'float'");
}

/* -------------------------------------------------------------------------
 * Constants.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_check, constants_fold)
{
    expect_typed("static const int N = 2 + 1; float a[N * 2]; float b[sizeof(float4)]; float c[(1 "
                 "<< 33) >> 32];",
                 "(variable static const int N = (convert (+ 2:literal int 1:literal int):literal "
                 "int):int)\n"
                 "(variable float[6] a)\n(variable float[16] b)\n(variable float[2] c)\n");
    /* Integer literals have 64 bits, as in DXC. */
    expect_diagnostic(
        "float c[1 << 33];",
        AT(1, 11) "error: the size of an array must be from 1 to 2147483647, not 8589934592");
}

TB_TEST(shader_check, constant_folding_follows_hlsl)
{
    /* Each array's size is a constant expression, folded as HLSL folds it:
     * integers wrap at their width, casts truncate toward zero. */
    expect_typed(
        "static const int N = 3;\nfloat a0[-(-3)];\nfloat a1[~(-4)];\nfloat a2[!0 + 1];\nfloat "
        "a3[7 / 2];\nfloat a4[7 % 4];\nfloat a5[-7 / -1];\nfloat a6[-8 % -1 + 1];\nfloat a7[1 << "
        "2];\nfloat a8[16 >> 2];\nfloat a9[(-16 >> 2) + 8];\nfloat a10[6 & 3];\nfloat a11[4 | "
        "1];\nfloat a12[6 ^ 3];\nfloat a13[(2 > 1) + (1 >= 1) + (1 < 2) + (1 <= 0) + (1 == 1) + (1 "
        "!= 1)];\nfloat a14[(1 && 0) + (1 || 0) + 1];\nfloat a15[1 ? 3 : 4];\nfloat a16[0 ? 3 : "
        "4];\nfloat a17[(int)2.9];\nfloat a18[int(3.5)];\nfloat a19[uint(-1) >> 30];\nfloat "
        "a20[(int)(1.5 + 1.5)];\nfloat a21[(int)(2.0 * 2.0 / 4.0 - 1.0 + 3)];\nfloat a22[(1.5 > "
        "1.0) + 1];\nfloat a23[N.x];\nfloat a24[(uint)0xFFFFFFFF + 3];\nfloat a25[true + "
        "true];\nfloat a26[(1, 2)];\nfloat a27[(uint)3e9 >> 30];\nfloat a28[(uint64_t)1e19 >> "
        "62];\nfloat a29[(int64_t)1e19 >> 62];\nfloat a30[(int)-2.5 + 4];\nfloat a31[(int)(-1.5 < "
        "-0.5) + 1];\nfloat a32[(1.0 == 1.0) + (1.0 != 2.0)];\nfloat a33[+3];\nfloat "
        "a34[(int16_t)65537 + 1];\nfloat a35[(uint16_t)65537 + 1];\nfloat a36[(int)true + "
        "1];\nfloat a37[(bool)2.5 + 1];\nfloat a38[(float)3 > 2.5 ? 2 : 1];\nfloat "
        "a39[(int)(double)3];\n",
        "(variable static const int N = (convert 3:literal int):int)\n"
        "(variable float[3] a0)\n"
        "(variable float[3] a1)\n"
        "(variable float[2] a2)\n"
        "(variable float[3] a3)\n"
        "(variable float[3] a4)\n"
        "(variable float[7] a5)\n"
        "(variable float[1] a6)\n"
        "(variable float[4] a7)\n"
        "(variable float[4] a8)\n"
        "(variable float[4] a9)\n"
        "(variable float[2] a10)\n"
        "(variable float[5] a11)\n"
        "(variable float[5] a12)\n"
        "(variable float[4] a13)\n"
        "(variable float[2] a14)\n"
        "(variable float[3] a15)\n"
        "(variable float[4] a16)\n"
        "(variable float[2] a17)\n"
        "(variable float[3] a18)\n"
        "(variable float[3] a19)\n"
        "(variable float[3] a20)\n"
        "(variable float[3] a21)\n"
        "(variable float[2] a22)\n"
        "(variable float[3] a23)\n"
        "(variable float[2] a24)\n"
        "(variable float[2] a25)\n"
        "(variable float[2] a26)\n"
        "(variable float[2] a27)\n"
        "(variable float[2] a28)\n"
        "(variable float[1] a29)\n"
        "(variable float[2] a30)\n"
        "(variable float[2] a31)\n"
        "(variable float[2] a32)\n"
        "(variable float[3] a33)\n"
        "(variable float[2] a34)\n"
        "(variable float[2] a35)\n"
        "(variable float[2] a36)\n"
        "(variable float[2] a37)\n"
        "(variable float[2] a38)\n"
        "(variable float[3] a39)\n");
    /* Division by zero is not a constant. */
    expect_diagnostic("float a[1 / 0];", AT(1, 11) "error: expected a constant integer");
    expect_diagnostic("float a[1 % 0];", AT(1, 11) "error: expected a constant integer");
    expect_diagnostic("float a[(int)(1.0 / 0.0)];", AT(1, 9) "error: expected a constant integer");
}

TB_TEST(shader_check, sizes_must_be_positive_constants)
{
    expect_diagnostic("float f(int n) { float a[n]; return 0; }",
                      AT(1, 26) "error: expected a constant integer");
    expect_diagnostic("float a[-1];",
                      AT(1, 9) "error: the size of an array must be from 1 to 2147483647, not -1");
    expect_diagnostic("float a[];",
                      AT(1, 7) "error: the array 'a' needs a size, or an initializer to count");
    expect_clean("Texture2D Textures[] : register(t0, space1);");
}

/* -------------------------------------------------------------------------
 * Statements.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_check, jumps_and_labels_are_where_they_belong)
{
    expect_diagnostic("void f() { break; }",
                      AT(1, 12) "error: 'break' is not in a loop or a switch");
    expect_diagnostic("void f() { switch (1) { default: continue; } }",
                      AT(1, 34) "error: 'continue' is not in a loop");
    expect_diagnostic(
        "int f(int x) { switch (x) { case 1: return 1; case 1: return 2; } return 0; }",
        AT(1, 47) "error: duplicate case value 1");
    expect_diagnostic("void f(int x) { switch (x) { default: break; default: break; } }",
                      AT(1, 46) "error: a switch has one 'default'");
    /* A default after cases, and a case after a default, are no duplicates. */
    expect_clean("int f(int x) { switch (x) { case 1: x = 2; default: x = 3; case 4: break; } "
                 "return x; }");
    expect_diagnostic("void f(float x) { switch (x) { default: break; } }",
                      AT(1, 27) "error: a switch needs an integer, not 'float'");
    expect_clean("void f(int x) { for (int i = 0; i < x; i++) { if (i == 2) continue; if (i == 3) "
                 "break; } }");
}

TB_TEST(shader_check, functions_return_what_they_declare)
{
    expect_diagnostic("float f(int a) { if (a) return 1; }",
                      AT(1, 7) "error: 'f' may reach its end without returning a value");
    expect_diagnostic("void f() { return 1; }", AT(1, 19) "error: 'f' returns no value");
    expect_diagnostic("float f() { return; }", AT(1, 13) "error: 'f' must return a 'float'");
    expect_clean("float f(int a) { if (a) return 1; else return 2; }");
    expect_clean("float f(int a) { while (true) { if (a) return 1; } }");
    expect_clean("float f(int a) { for (;;) { return 1; } }");
    expect_clean("float f(int a) { if (a) { return 1; } discard; }");
}

/* -------------------------------------------------------------------------
 * Intrinsic functions.
 * ------------------------------------------------------------------------- */

/* The type of `call`, made in a function after `declarations`, which may
 * earn warnings but no error. */
static void expect_call_type(const char *declarations, const char *call, const char *expected)
{
    char source[1024] = {0};
    (void)tb_fmt_buffer(source, sizeof(source), "%s\nvoid g() { %s; }", declarations, call);
    checked_source out = {0};
    check_source(&out, source);
    TB_TEST_CONTEXT("source: %s", source);
    if (out.checked.error_count > 0)
    {
        char message[256] = {0};
        diagnostic_text(&out, 0, message, sizeof(message));
        TB_TEST_CONTEXT("first diagnostic: %s", message);
    }
    TB_TEST_EXPECT_UINT(out.checked.error_count, ==, 0);
    char ending[128] = {0};
    (void)tb_fmt_buffer(ending, sizeof(ending), "):%s)))\n", expected);
    TB_TEST_CONTEXT("tree: %s", out.tree);
    TB_TEST_EXPECT_UINT(tb_str_ends_with(out.tree, ending), ==, true);
    finish(&out);
}

#define VALUES                                                                                     \
    "static int i; static uint u; static half h; static float f; static bool b; "                  \
    "static double d; static float3 f3; static float4 f4; static int3 i3; static bool3 b3; "       \
    "static float2x2 m2; static float3x3 m3; static float3x4 m34; static float4x2 m42; "           \
    "static uint64_t u64; static min16float mf;"

TB_TEST(shader_check, every_intrinsic_signature_reads)
{
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_types *const types = tb_shader_types_create(arena);
    for (size_t index = 1; index < (size_t)TB_SHADER_INTRINSIC_COUNT; index++)
    {
        const tb_shader_intrinsic_info *const info =
            tb_shader_intrinsic_info_of((tb_shader_intrinsic)index);
        TB_TEST_REQUIRE_POINTER(info, !=, NULL);
        TB_TEST_CONTEXT("intrinsic: %s: %s", info->name, info->signatures);
        TB_TEST_EXPECT_UINT(tb_shader_check_intrinsic_is_valid(types, info->intrinsic), ==, true);
        /* One entry for each name, so an entry holds all its overloads. */
        for (size_t other = 1; other < index; other++)
        {
            const char *const name = tb_shader_intrinsic_info_of((tb_shader_intrinsic)other)->name;
            TB_TEST_EXPECT_UINT(tb_str_equal(name, info->name), ==, false);
        }
    }
    TB_TEST_EXPECT_POINTER(tb_shader_intrinsic_info_of(TB_SHADER_INTRINSIC_NONE), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_shader_intrinsic_info_of(TB_SHADER_INTRINSIC_COUNT), ==, NULL);
    tb_arena_destroy(arena);
}

TB_TEST(shader_check, intrinsic_calls_are_typed_and_converted)
{
    /* The argument is converted to the type the template takes. */
    expect_typed("float f(int i) { return sin(i); }",
                 "(function float f ((parameter int i)) (block (return (call sin (convert "
                 "i:int):float):float)))\n");
    expect_typed("int f(int i) { return abs(i); }",
                 "(function int f ((parameter int i)) (block (return (call abs i:int):int)))\n");
}

TB_TEST(shader_check, intrinsic_templates_are_decided_as_dxc_decides_them)
{
    /* Components combine as an operator's operands do. */
    expect_call_type(VALUES, "max(i, u)", "uint");
    expect_call_type(VALUES, "min(h, f)", "float");
    expect_call_type(VALUES, "clamp(i, u, f)", "float");
    expect_call_type(VALUES, "max(i3, f)", "float3");
    expect_call_type(VALUES, "dot(i3, f3)", "float");
    /* A literal keeps its literal type where the class takes it. */
    expect_call_type(VALUES, "max(1, 2)", "literal int");
    expect_call_type(VALUES, "max(1, 2.0)", "literal float");
    expect_call_type(VALUES, "sin(1.5)", "literal float");
    /* What is not in a class becomes its fallback. */
    expect_call_type(VALUES, "sin(i)", "float");
    expect_call_type(VALUES, "sin(1)", "float");
    expect_call_type(VALUES, "sqrt(d)", "float");
    expect_call_type(VALUES, "rcp(d)", "double");
    expect_call_type(VALUES, "abs(b)", "float");
    expect_call_type(VALUES, "countbits(f)", "uint");
    expect_call_type(VALUES, "sqrt(mf)", "min16float");
    /* Shapes: a scalar takes any shape, and a larger one is truncated. */
    expect_call_type(VALUES, "lerp(f3, f3, f)", "float3");
    expect_call_type(VALUES, "max(f3, f4)", "float3");
    expect_call_type(VALUES, "max(m3, m2)", "float2x2");
    expect_call_type(VALUES, "sign(f3)", "int3");
    expect_call_type(VALUES, "isnan(f4)", "bool4");
    expect_call_type(VALUES, "select(b3, f3, f)", "float3");
    expect_call_type(VALUES, "dot(f, f)", "float");
    expect_call_type(VALUES, "dot(f3, f)", "float");
    expect_call_type(VALUES, "cross(f3, f4)", "float3");
    expect_call_type(VALUES, "transpose(m34)", "float4x3");
    expect_call_type(VALUES, "determinant(m3)", "float");
    expect_call_type(VALUES, "asuint(f3)", "uint3");
    expect_diagnostic(VALUES "\nfloat3 g() { return max(f3, f4); }",
                      AT(2, 29) "warning: 'float4' is truncated to 'float3'");
}

TB_TEST(shader_check, mul_takes_each_of_its_forms)
{
    expect_call_type(VALUES, "mul(f, f)", "float");
    expect_call_type(VALUES, "mul(f, f3)", "float3");
    expect_call_type(VALUES, "mul(m3, f)", "float3x3");
    expect_call_type(VALUES, "mul(f3, f3)", "float");
    expect_call_type(VALUES, "mul(f3, m34)", "float4");
    expect_call_type(VALUES, "mul(m34, f4)", "float3");
    expect_call_type(VALUES, "mul(m34, m42)", "float3x2");
    /* An inner size that differs takes the smaller, as DXC does. */
    expect_call_type(VALUES, "mul(m2, f4)", "float2");
}

TB_TEST(shader_check, intrinsics_refuse_what_dxc_refuses)
{
    expect_diagnostic(VALUES "\nvoid g() { max(m2, f4); }",
                      AT(2, 12) "error: no 'max' takes (float2x2, float4)");
    expect_diagnostic(VALUES "\nvoid g() { dot(m2, m2); }",
                      AT(2, 12) "error: no 'dot' takes (float2x2, float2x2)");
    expect_diagnostic(VALUES "\nvoid g() { asfloat(u64); }",
                      AT(2, 12) "error: no 'asfloat' takes (uint64_t)");
    expect_diagnostic(VALUES "\nvoid g() { sin(); }", AT(2, 12) "error: no 'sin' takes ()");
    /* A template written as a scalar takes only a scalar. */
    expect_diagnostic(VALUES "\nvoid g() { refract(f3, f3, f3); }",
                      AT(2, 12) "error: no 'refract' takes (float3, float3, float3)");
    expect_diagnostic(VALUES "\nvoid g() { InterlockedAdd(f, 1); }",
                      AT(2, 12) "error: no 'InterlockedAdd' takes (float, literal int)");
    expect_diagnostic("float f(float x) { sincos(x, 1, 2); return x; }",
                      AT(1, 30) "error: argument 2 of 'sincos' is an output, so it must be "
                                "assignable");
    expect_diagnostic("void f() { frobnicate(1); }",
                      AT(1, 12) "error: 'frobnicate' is not declared");
}

TB_TEST(shader_check, intrinsics_with_outputs_and_atomics)
{
    expect_clean("float f(float3 x) { float3 s, c; sincos(x, s, c); float3 w; return modf(x, "
                 "w).x + s.y + c.z; }");
    expect_clean("void f(double d) { uint low, high; asuint(d, low, high); }");
    expect_clean("groupshared uint counter; RWBuffer<uint> totals;\n"
                 "void f() { uint before; InterlockedAdd(counter, 1, before); "
                 "InterlockedMax(totals[0], before); GroupMemoryBarrierWithGroupSync(); }");
}

TB_TEST(shader_check, intrinsics_of_waves_ray_tracing_and_debugging)
{
    expect_clean("float f(float3 v) { if (WaveIsFirstLane()) { return WaveActiveSum(v).x; } "
                 "return WaveReadLaneAt(v, 0).y + QuadReadAcrossX(v.z); }");
    expect_clean("struct Payload { float4 color; };\n"
                 "RaytracingAccelerationStructure scene;\n"
                 "void f() { RayDesc ray; ray.Origin = WorldRayOrigin(); ray.Direction = "
                 "float3(0, 0, 1); ray.TMin = 0; ray.TMax = 1000; Payload p; "
                 "TraceRay(scene, 0, 0xff, 0, 1, 0, ray, p); }");
    expect_clean("struct Hit { float2 barycentrics; };\n"
                 "void f() { Hit h; h.barycentrics = 0; ReportHit(RayTCurrent(), 0, h); }");
    expect_clean("void f(int x) { printf(\"x is %d\", x); }");
    expect_diagnostic("void f() { float3 v = 1; TraceRay(v); }",
                      AT(1, 26) "error: no 'TraceRay' takes (float3)");
}

TB_TEST(shader_check, functions_of_the_shader_hide_intrinsics)
{
    expect_diagnostic("struct S { float a; };\n"
                      "float length(S s) { return s.a; }\n"
                      "float f() { return length(float3(1, 2, 3)); }",
                      AT(3, 20) "error: no 'length' takes (float3)");
    expect_diagnostic("float f() { float sin = 1; return sin(sin); }",
                      AT(1, 35) "error: 'sin' is not a function");
}

/* -------------------------------------------------------------------------
 * Methods of the built-in objects.
 * ------------------------------------------------------------------------- */

#define OBJECTS                                                                                    \
    "Texture1D t1; Texture2D t2; Texture2DArray t2a; Texture3D t3; TextureCube tc; "               \
    "Texture2DMS<float4> ms; Texture2D<uint2> tu; Texture2D<float> tf; RWTexture2D<float> rw2; "   \
    "Buffer<float3> buf; RWBuffer<int> rb; StructuredBuffer<float2> sb; "                          \
    "RWStructuredBuffer<float> rsb; ByteAddressBuffer bab; RWByteAddressBuffer rbab; "             \
    "AppendStructuredBuffer<float> asb; ConsumeStructuredBuffer<float> csb; SamplerState s; "      \
    "SamplerComparisonState sc; "

TB_TEST(shader_check, every_method_signature_reads)
{
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_types *const types = tb_shader_types_create(arena);
    for (size_t index = 1; index < (size_t)TB_SHADER_METHOD_COUNT; index++)
    {
        const tb_shader_method_info *const info = tb_shader_method_info_of((tb_shader_method)index);
        TB_TEST_REQUIRE_POINTER(info, !=, NULL);
        TB_TEST_CONTEXT("method: %s: %s", info->name, info->signatures);
        TB_TEST_EXPECT_UINT(tb_shader_check_method_is_valid(types, info->method), ==, true);
        /* A row without traits would be every object's, a sampler's too. */
        TB_TEST_EXPECT_UINT(info->traits, !=, 0);
    }
    TB_TEST_EXPECT_POINTER(tb_shader_method_info_of(TB_SHADER_METHOD_NONE), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_shader_method_info_of(TB_SHADER_METHOD_COUNT), ==, NULL);
    TB_TEST_EXPECT_UINT(tb_shader_object_methods_of(TB_SHADER_OBJECT_SAMPLER).traits, ==, 0);
    tb_arena_destroy(arena);
}

TB_TEST(shader_check, texture_methods_return_what_dxc_returns)
{
    /* What a texture holds, or a component of it. */
    expect_call_type(OBJECTS VALUES, "t1.Sample(s, 0.5)", "float4");
    expect_call_type(OBJECTS VALUES, "tf.Sample(s, f3.xy)", "float");
    expect_call_type(OBJECTS VALUES, "t2.SampleCmp(sc, f3.xy, 0.5)", "float");
    expect_call_type(OBJECTS VALUES, "tu.Gather(s, f3.xy)", "uint4");
    expect_call_type(OBJECTS VALUES, "tc.GatherCmp(sc, f3, 0.5)", "float4");
    expect_call_type(OBJECTS VALUES, "t2a.CalculateLevelOfDetailUnclamped(s, f3.xy)", "float");
    expect_call_type(OBJECTS VALUES, "ms.GetSamplePosition(0)", "float2");
    /* Offsets, clamps and statuses, each where its texture takes it. */
    expect_call_type(OBJECTS VALUES, "t2.Sample(s, f3.xy, int2(1, 1), 0.5, u)", "float4");
    expect_call_type(OBJECTS VALUES, "tc.Sample(s, f3, 1.0, u)", "float4");
    expect_call_type(OBJECTS VALUES, "tc.SampleGrad(s, f3, f3, f3)", "float4");
    expect_call_type(OBJECTS VALUES, "t2a.SampleLevel(s, f3, 0, int2(0, 0), u)", "float4");
    expect_call_type(OBJECTS VALUES, "t2.SampleCmpLevelZero(sc, f3.xy, 0.5, int2(0, 0))", "float");
    expect_call_type(OBJECTS VALUES, "tu.GatherRed(s, f3.xy, i3.xy, i3.xy, i3.xy, i3.xy)", "uint4");
    expect_call_type(OBJECTS VALUES, "tc.Gather(s, f3, u)", "float4");
    /* Loads at a location with a mip level, with a sample, or alone. */
    expect_call_type(OBJECTS VALUES, "t2.Load(i3, int2(0, 0), u)", "float4");
    expect_call_type(OBJECTS VALUES, "t3.Load(int4(i3, 0))", "float4");
    expect_call_type(OBJECTS VALUES, "ms.Load(i3.xy, 1, int2(0, 0), u)", "float4");
    expect_call_type(OBJECTS VALUES, "rw2.Load(i3.xy, u)", "float");
}

TB_TEST(shader_check, buffer_methods_return_what_dxc_returns)
{
    expect_call_type(OBJECTS VALUES, "buf.Load(1, u)", "float3");
    expect_call_type(OBJECTS VALUES, "rb.Load(1)", "int");
    expect_call_type(OBJECTS VALUES, "sb.Load(1, u)", "float2");
    expect_call_type(OBJECTS VALUES, "rsb.IncrementCounter()", "uint");
    expect_call_type(OBJECTS VALUES, "csb.Consume()", "float");
    /* A byte-address buffer loads uints, or the type the call names. */
    expect_call_type(OBJECTS VALUES, "bab.Load(0, u)", "uint");
    expect_call_type(OBJECTS VALUES, "bab.Load3(0)", "uint3");
    expect_call_type(OBJECTS VALUES, "bab.Load<float4>(0)", "float4");
    expect_call_type("struct S { float a; int b; };\n" OBJECTS, "bab.Load<S>(0)", "S");
}

TB_TEST(shader_check, object_methods_take_what_dxc_takes)
{
    expect_clean(OBJECTS VALUES
                 "\nvoid g() {\n"
                 "  t2.GetDimensions(u, u); t2.GetDimensions(0, u, u, u);\n"
                 "  t2.GetDimensions(0, f, f, f); t2.GetDimensions(i, i);\n"
                 "  ms.GetDimensions(u, u, u); tc.GetDimensions(0, f, f, f);\n"
                 "  buf.GetDimensions(u); sb.GetDimensions(u, u); "
                 "rw2.GetDimensions(u, u);\n"
                 "  asb.Append(1.0); rbab.Store(0, 1u); rbab.Store2(0, uint2(1, 2));\n"
                 "  rbab.Store(0, f); rbab.Store<float2>(0, float2(1, 1));\n"
                 "  rbab.InterlockedAdd(0, 1); rbab.InterlockedAdd(0, 1, u);\n"
                 "  rbab.InterlockedMin(0, i); rbab.InterlockedMax(0, 1, u);\n"
                 "  rbab.InterlockedCompareExchange(0, 1, 2, u);\n"
                 "  rbab.InterlockedCompareStore(0, 1, 2); "
                 "rbab.InterlockedAdd64(0, 1);\n"
                 "  rbab.InterlockedMax64(0, u64, u64);\n"
                 "}");
}

TB_TEST(shader_check, a_stored_value_takes_its_own_type)
{
    /* `Store` stores what it is given, a literal as a uint or a float; an
     * atomic of int or uint takes a literal as a uint. */
    expect_typed("RWByteAddressBuffer B;\nvoid f(int i) { B.Store(0, 1); B.Store(4, 2.5); "
                 "B.InterlockedMin(8, 1); B.InterlockedMin(8, i); }",
                 "(variable RWByteAddressBuffer B)\n"
                 "(function void f ((parameter int i)) (block "
                 "(expr (call (. B:RWByteAddressBuffer Store) (convert 0:literal int):uint "
                 "(convert 1:literal int):uint):void) "
                 "(expr (call (. B:RWByteAddressBuffer Store) (convert 4:literal int):uint "
                 "(convert 2.5:literal float):float):void) "
                 "(expr (call (. B:RWByteAddressBuffer InterlockedMin) (convert 8:literal "
                 "int):uint (convert 1:literal int):uint):void) "
                 "(expr (call (. B:RWByteAddressBuffer InterlockedMin) (convert 8:literal "
                 "int):uint i:int):void)))\n");
}

TB_TEST(shader_check, object_methods_refuse_what_dxc_refuses)
{
    /* A method's errors are reported where its object is left: at the `.`. */
    expect_diagnostic(OBJECTS "\nvoid g(float2 p) { t2.Sample(sc, p); }",
                      AT(2, 22) "error: no 'Sample' takes (SamplerComparisonState, float2)");
    expect_diagnostic(OBJECTS "\nvoid g() { t2.Sample(s); }",
                      AT(2, 14) "error: no 'Sample' takes (SamplerState)");
    /* A cube takes no offset, and a volume no comparison sampler. */
    expect_diagnostic(OBJECTS "\nvoid g(float3 p) { tc.Sample(s, p, int3(0, 0, 0)); }",
                      AT(2, 22) "error: no 'Sample' takes (SamplerState, float3, int3)");
    expect_diagnostic(OBJECTS "\nvoid g(float3 p) { t3.CalculateLevelOfDetail(sc, p); }",
                      AT(2, 22) "error: no 'CalculateLevelOfDetail' takes "
                                "(SamplerComparisonState, float3)");
    expect_diagnostic(OBJECTS "\nvoid g(int4 p) { tc.Load(p); }",
                      AT(2, 20) "error: 'TextureCube<float4>' has no method 'Load'");
    expect_diagnostic(OBJECTS "\nvoid g() { rsb.Append(1.0); }",
                      AT(2, 15) "error: 'RWStructuredBuffer<float>' has no method 'Append'");
    expect_diagnostic(OBJECTS "\nvoid g() { s.Sample(); }",
                      AT(2, 13) "error: 'SamplerState' has no method 'Sample'");
    expect_diagnostic(OBJECTS "\nvoid g() { bab.Load2<float>(0); }",
                      AT(2, 15) "error: 'Load2' takes no template arguments");
    expect_diagnostic(OBJECTS "\nvoid g() { bab.Load<float, int>(0); }",
                      AT(2, 15) "error: 'Load' takes one type to name");
    /* A location larger than the texture's is truncated, as DXC warns. */
    expect_diagnostic(OBJECTS "\nfloat4 g(float4 p) { return t2.Sample(s, p); }",
                      AT(2, 42) "warning: 'float4' is truncated to 'float2'");
}

TB_TEST(shader_check, scalar_parameters_take_only_scalars)
{
    /* DXC passes no vector, not even a `float1`, where an intrinsic or a
     * method takes a scalar; a vector parameter takes a scalar or a vector,
     * truncated, but no matrix. */
    expect_diagnostic(OBJECTS "\nfloat4 g(float2 p) { return t2.SampleLevel(s, p, float2(0, 0)); }",
                      AT(2, 31) "error: no 'SampleLevel' takes (SamplerState, float2, float2)");
    expect_diagnostic(OBJECTS "\nfloat4 g(float2 p) { return t2.SampleLevel(s, p, float1(0)); }",
                      AT(2, 31) "error: no 'SampleLevel' takes (SamplerState, float2, float1)");
    expect_diagnostic("float g(float f) { return WaveReadLaneAt(f, uint2(0, 0)); }",
                      AT(1, 27) "error: no 'WaveReadLaneAt' takes (float, uint2)");
    expect_diagnostic(OBJECTS "\nfloat4 g(float2x2 p) { return t2.Sample(s, p); }",
                      AT(2, 33) "error: no 'Sample' takes (SamplerState, float2x2)");
    expect_clean(OBJECTS "\nfloat4 g(float p) { rbab.Store2(0, 1); return t2.Sample(s, p); }");
}

TB_TEST(shader_check, streams_ray_queries_and_feedback_have_methods)
{
    expect_clean("struct V { float4 p : SV_Position; };\n"
                 "[maxvertexcount(3)] void gs(triangle V input[3], inout TriangleStream<V> "
                 "stream) { stream.Append(input[0]); stream.RestartStrip(); }");
    expect_clean("RaytracingAccelerationStructure scene;\n"
                 "float f(RayDesc ray) { RayQuery<0> q; q.TraceRayInline(scene, 0, 0xff, ray);\n"
                 "  while (q.Proceed()) { if (q.CandidateType() == 0) { "
                 "q.CommitNonOpaqueTriangleHit(); } }\n"
                 "  float3x4 m = q.CommittedObjectToWorld3x4(); "
                 "return q.CommittedRayT() + m[0][0]; }");
    expect_clean("FeedbackTexture2D feedback; Texture2D t; SamplerState s;\n"
                 "void f(float2 p) { feedback.WriteSamplerFeedbackLevel(t, s, p, 0); }");
}

#define MEMBERS                                                                                    \
    "Texture2D<float> tf; Texture1D t1; Texture2DArray t2a; Texture3D t3; "                        \
    "Texture2DMS<float4> ms; Texture2DMSArray<uint2> msa; static uint m; static uint2 p; "         \
    "static uint3 q; static int2 ip; static float2 fp; static uint1 p1;"

TB_TEST(shader_check, mips_and_samples_read_texels_as_dxc_reads_them)
{
    /* A level or a sample, then the texel at a location. */
    expect_call_type(MEMBERS, "tf.mips[m][p]", "float");
    expect_call_type(MEMBERS, "t1.mips[m][p.x]", "float4");
    expect_call_type(MEMBERS, "t1.mips[m][p1]", "float4");
    expect_call_type(MEMBERS, "t2a.mips[m][q]", "float4");
    expect_call_type(MEMBERS, "t3.mips[m][q]", "float4");
    expect_call_type(MEMBERS, "ms.sample[m][p]", "float4");
    expect_call_type(MEMBERS, "msa.sample[m][q]", "uint2");
    /* A location of the right size converts, whatever its component. */
    expect_call_type(MEMBERS, "tf.mips[m][ip]", "float");
    expect_call_type(MEMBERS, "tf.mips[m][fp]", "float");
    /* The members and their slices are types of their own, named as DXC names them. */
    expect_call_type(MEMBERS, "tf.mips", "Texture2D<float>::mips_type");
    expect_call_type(MEMBERS, "tf.mips[m]", "Texture2D<float>::mips_slice_type");
    expect_call_type(MEMBERS, "ms.sample[m]", "Texture2DMS<float4>::sample_slice_type");
    /* A writable texture's samples are written. */
    expect_clean("RWTexture2DMS<float4> rw;\nvoid g(uint2 p) { rw.sample[0][p] = 1; }");
}

TB_TEST(shader_check, mips_and_samples_refuse_what_dxc_refuses)
{
    expect_diagnostic("TextureCube tc;\nfloat4 g(uint3 p) { return tc.mips[0][p]; }",
                      AT(2, 30) "error: 'TextureCube<float4>' has no member 'mips'");
    expect_diagnostic("Texture2D t;\nfloat4 g(uint2 p) { return t.sample[0][p]; }",
                      AT(2, 29) "error: 'Texture2D<float4>' has no member 'sample'");
    /* A location takes exactly the texture's components: none dropped, none spread. */
    expect_diagnostic("Texture2D<float> t;\nfloat g(uint3 p) { return t.mips[0][p]; }",
                      AT(2, 37) "error: 'Texture2D<float>::mips_slice_type' takes a 'uint2' "
                                "location, not 'uint3'");
    expect_diagnostic("Texture2D<float> t;\nfloat g(uint p) { return t.mips[0][p]; }",
                      AT(2, 36) "error: 'Texture2D<float>::mips_slice_type' takes a 'uint2' "
                                "location, not 'uint'");
    expect_diagnostic("Texture2D<float> t;\nfloat g(uint2 p) { return t.mips[p][p]; }",
                      AT(2, 34) "error: an index must be an integer, not 'uint2'");
    expect_diagnostic("Texture2D t;\nvoid g(uint2 p) { t.mips[0][p] = 1; }",
                      AT(2, 28) "error: 'Texture2D<float4>' is read-only");
}

TB_TEST(shader_check, templates_are_not_supported_yet)
{
    expect_diagnostic("template<typename T> T Square(T x) { return x * x; }",
                      AT(1, 24) "error: templates are not supported yet");
}

TB_TEST(shader_check, errors_of_the_syntax_come_first)
{
    checked_source out = {0};
    out.arena = tb_arena_create(0);
    tb_shader_file file = {0};
    file.path = MAIN_PATH;
    file.text = "float a = ;\nfloat b = c;";
    file.length = tb_str_length(file.text);
    tb_shader_preprocess_request request = {0};
    request.path = MAIN_PATH;
    request.files = &file;
    request.file_count = 1;
    tb_shader_unit unit = {0};
    TB_TEST_EXPECT_UINT(tb_shader_preprocess_unit(&request, out.arena, &unit), ==, TB_SHADER_OK);
    TB_TEST_EXPECT_UINT(tb_shader_parse(&unit, out.arena, &out.syntax), ==, TB_SHADER_FAILED);
    TB_TEST_EXPECT_UINT(tb_shader_check(&out.syntax, out.arena, &out.checked), ==,
                        TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.checked.diagnostic_count, ==, 2);
    TB_TEST_EXPECT_UINT(out.checked.error_count, ==, 2);
    char message[256] = {0};
    diagnostic_text(&out, 1, message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==, AT(2, 11) "error: 'c' is not declared");
    finish(&out);
}
