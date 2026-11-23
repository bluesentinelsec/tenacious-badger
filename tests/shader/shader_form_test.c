/* The compiler's own form: what lowering makes of a checked shader, as
 * tb_shader_form_print() writes it, and the rules tb_shader_form_verify()
 * holds every form to. Each lowered form is verified before it is compared,
 * so a test that passes shows lowering keeps the form's rules. */
#include "badger/arena.h"
#include "badger/shader.h"
#include "badger/str.h"
#include "shader/shader_check.h"
#include "shader/shader_form.h"
#include "shader/shader_intrinsics.h"
#include "shader/shader_lower.h"
#include "tb_test.h"

#define MAIN_PATH "/Engine/Private/Main.hlsl"

typedef struct lowered_source
{
    tb_arena *arena;
    tb_shader_result result;
    tb_shader_lowered lowered;
    tb_shader_form form;
    const char *text;
} lowered_source;

/* Check and lower a source, which must check cleanly, then verify the form. */
static void lower_source(lowered_source *out, const char *source)
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
    tb_shader_syntax syntax = {0};
    TB_TEST_EXPECT_UINT(tb_shader_parse(&unit, out->arena, &syntax), ==, TB_SHADER_OK);
    tb_shader_checked checked = {0};
    const tb_shader_result result = tb_shader_check(&syntax, out->arena, &checked);
    if (checked.diagnostic_count > 0 && checked.error_count > 0)
    {
        TB_TEST_CONTEXT("check error: %s", checked.diagnostics[0].message);
    }
    TB_TEST_REQUIRE_UINT(result, ==, TB_SHADER_OK);
    out->result = tb_shader_lower(&syntax, &checked, out->arena, &out->form, &out->lowered);
    char why[256] = {0};
    const bool verified = tb_shader_form_verify(&out->form, why, sizeof(why));
    out->text = tb_shader_form_print(&out->form, out->arena);
    TB_TEST_CONTEXT("verifier: %s", why);
    TB_TEST_EXPECT_UINT(verified, ==, true);
}

/* A source whose form is `expected`. */
static void expect_form(const char *source, const char *expected)
{
    lowered_source out = {0};
    lower_source(&out, source);
    TB_TEST_CONTEXT("source: %s", source);
    if (out.lowered.diagnostic_count > 0)
    {
        TB_TEST_CONTEXT("lowering: %s", out.lowered.diagnostics[0].message);
    }
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    TB_TEST_EXPECT_STRING(out.text, ==, expected);
    tb_arena_destroy(out.arena);
}

/* A source that checks, but uses what the form does not hold yet: lowering
 * says so, and what it made still keeps the form's rules. */
static void expect_unsupported(const char *source, const char *expected)
{
    lowered_source out = {0};
    lower_source(&out, source);
    TB_TEST_CONTEXT("source: %s", source);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.lowered.diagnostic_count, >, 0);
    TB_TEST_EXPECT_STRING(out.lowered.diagnostics[0].message, ==, expected);
    tb_arena_destroy(out.arena);
}

/* -------------------------------------------------------------------------
 * Lowering.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_form, parameters_become_locals_and_conversions_are_written_out)
{
    expect_form("float f(int i, float x) { return x * i + 1; }",
                "function float f(in int i, in float x)\n"
                "  local0 int i\n"
                "  local1 float x\n"
                "  {\n"
                "    store local0 arg0\n"
                "    store local1 arg1\n"
                "    %5:float = load local1\n"
                "    %7:int = load local0\n"
                "    %8:float = convert %7\n"
                "    %9:float = multiply %5 %8\n"
                "    %11:float = add %9 1.0:float\n"
                "    return %11\n"
                "  }\n");
}

TB_TEST(shader_form, conversions_splat_truncate_or_rebuild)
{
    expect_form("float4 f(float x, float3x3 m, float4 v) { float2x2 n = (float2x2)m; "
                "float2x2 w = (float2x2)v; int2 i = (int2)v; return x + n[0].x + w[1].y + i.x; }",
                "function float4 f(in float x, in float3x3 m, in float4 v)\n"
                "  local0 float x\n"
                "  local1 float3x3 m\n"
                "  local2 float4 v\n"
                "  local3 float2x2 n\n"
                "  local4 float2x2 w\n"
                "  local5 int2 i\n"
                "  {\n"
                "    store local0 arg0\n"
                "    store local1 arg1\n"
                "    store local2 arg2\n"
                "    %7:float3x3 = load local1\n"
                "    %8:float3 = access_index %7 0\n"
                "    %9:float2 = swizzle %8 xy\n"
                "    %10:float3 = access_index %7 1\n"
                "    %11:float2 = swizzle %10 xy\n"
                "    %12:float2x2 = compose %9 %11\n"
                "    store local3 %12\n"
                "    %15:float4 = load local2\n"
                "    %16:float = access_index %15 0\n"
                "    %17:float = access_index %15 1\n"
                "    %18:float = access_index %15 2\n"
                "    %19:float = access_index %15 3\n"
                "    %20:float2 = compose %16 %17\n"
                "    %21:float2 = compose %18 %19\n"
                "    %22:float2x2 = compose %20 %21\n"
                "    store local4 %22\n"
                "    %25:float4 = load local2\n"
                "    %26:float2 = swizzle %25 xy\n"
                "    %27:int2 = convert %26\n"
                "    store local5 %27\n"
                "    %30:float = load local0\n"
                "    %32:float2* function = access_index local3 0\n"
                "    %33:float2 = load %32\n"
                "    %34:float = swizzle %33 x\n"
                "    %35:float = add %30 %34\n"
                "    %37:float2* function = access_index local4 1\n"
                "    %38:float2 = load %37\n"
                "    %39:float = swizzle %38 y\n"
                "    %40:float = add %35 %39\n"
                "    %42:int2 = load local5\n"
                "    %43:int = swizzle %42 x\n"
                "    %44:float = convert %43\n"
                "    %45:float = add %40 %44\n"
                "    %46:float4 = splat %45\n"
                "    return %46\n"
                "  }\n");
}

TB_TEST(shader_form, compound_assignments_steps_and_swizzled_stores)
{
    expect_form("void f(inout float4 v, inout float2x2 m) { v.xz += 1; v.y++; m._m01_m10 = v.xy; }",
                "function void f(inout float4 v, inout float2x2 m)\n"
                "  {\n"
                "    %1:float4 = load arg0\n"
                "    %2:float2 = swizzle %1 xz\n"
                "    %5:float2 = splat 1.0:float\n"
                "    %6:float2 = add %2 %5\n"
                "    %8:float* function = access_index arg0 0\n"
                "    %9:float = access_index %6 0\n"
                "    store %8 %9\n"
                "    %10:float* function = access_index arg0 2\n"
                "    %11:float = access_index %6 1\n"
                "    store %10 %11\n"
                "    %13:float4 = load arg0\n"
                "    %14:float = swizzle %13 y\n"
                "    %16:float = add %14 1.0:float\n"
                "    %18:float* function = access_index arg0 1\n"
                "    store %18 %16\n"
                "    %20:float4 = load arg0\n"
                "    %21:float2 = swizzle %20 xy\n"
                "    %23:float2* function = access_index arg1 0\n"
                "    %24:float* function = access_index %23 1\n"
                "    %25:float = access_index %21 0\n"
                "    store %24 %25\n"
                "    %26:float2* function = access_index arg1 1\n"
                "    %27:float* function = access_index %26 0\n"
                "    %28:float = access_index %21 1\n"
                "    store %27 %28\n"
                "  }\n");
}

TB_TEST(shader_form, loops_test_their_condition_where_hlsl_does)
{
    expect_form("int f(int n) { int total = 0; for (int i = 0; i < n; i++) { if (i == 3) continue; "
                "if (i > 8) break; total += i; } return total; }",
                "function int f(in int n)\n"
                "  local0 int n\n"
                "  local1 int total\n"
                "  local2 int i\n"
                "  {\n"
                "    store local0 arg0\n"
                "    store local1 0:int\n"
                "    store local2 0:int\n"
                "    loop\n"
                "    {\n"
                "      %7:int = load local2\n"
                "      %9:int = load local0\n"
                "      %10:bool = less %7 %9\n"
                "      %11:bool = not %10\n"
                "      if %11\n"
                "      {\n"
                "        break\n"
                "      }\n"
                "      %13:int = load local2\n"
                "      %15:bool = equal %13 3:int\n"
                "      if %15\n"
                "      {\n"
                "        continue\n"
                "      }\n"
                "      %17:int = load local2\n"
                "      %19:bool = greater %17 8:int\n"
                "      if %19\n"
                "      {\n"
                "        break\n"
                "      }\n"
                "      %21:int = load local1\n"
                "      %23:int = load local2\n"
                "      %24:int = add %21 %23\n"
                "      store local1 %24\n"
                "    }\n"
                "    continuing\n"
                "    {\n"
                "      %26:int = load local2\n"
                "      %28:int = add %26 1:int\n"
                "      store local2 %28\n"
                "    }\n"
                "    %30:int = load local1\n"
                "    return %30\n"
                "  }\n");
    expect_form("int f(int n) { do { n--; } while (n > 0); while (n < 4) n++; return n; }",
                "function int f(in int n)\n"
                "  local0 int n\n"
                "  {\n"
                "    store local0 arg0\n"
                "    loop\n"
                "    {\n"
                "      %3:int = load local0\n"
                "      %5:int = subtract %3 1:int\n"
                "      store local0 %5\n"
                "    }\n"
                "    continuing\n"
                "    {\n"
                "      %7:int = load local0\n"
                "      %9:bool = greater %7 0:int\n"
                "      %10:bool = not %9\n"
                "    }\n"
                "    break if %10\n"
                "    loop\n"
                "    {\n"
                "      %12:int = load local0\n"
                "      %14:bool = less %12 4:int\n"
                "      %15:bool = not %14\n"
                "      if %15\n"
                "      {\n"
                "        break\n"
                "      }\n"
                "      %17:int = load local0\n"
                "      %19:int = add %17 1:int\n"
                "      store local0 %19\n"
                "    }\n"
                "    continuing\n"
                "    {\n"
                "    }\n"
                "    %21:int = load local0\n"
                "    return %21\n"
                "  }\n");
}

TB_TEST(shader_form, switch_cases_say_whether_they_fall_through)
{
    expect_form("int f(int n) { switch (n) { case 0: case 1: n = 2; break; case 2: n = 3; "
                "default: n = 4; } return n; }",
                "function int f(in int n)\n"
                "  local0 int n\n"
                "  {\n"
                "    store local0 arg0\n"
                "    %3:int = load local0\n"
                "    switch %3\n"
                "    case 0 falls through\n"
                "    {\n"
                "    }\n"
                "    case 1\n"
                "    {\n"
                "      store local0 2:int\n"
                "    }\n"
                "    case 2 falls through\n"
                "    {\n"
                "      store local0 3:int\n"
                "    }\n"
                "    default\n"
                "    {\n"
                "      store local0 4:int\n"
                "    }\n"
                "    %11:int = load local0\n"
                "    return %11\n"
                "  }\n");
}

TB_TEST(shader_form, logic_evaluates_only_what_it_must)
{
    /* A second operand with no effect is evaluated anyway; one with an
     * effect only when it decides, by a branch. */
    expect_form("bool g(); bool f(bool a, bool b) { return a && b || a && g(); } bool g() { return "
                "true; }",
                "function bool f(in bool a, in bool b)\n"
                "  local0 bool a\n"
                "  local1 bool b\n"
                "  local2 bool temporary\n"
                "  local3 bool temporary\n"
                "  {\n"
                "    store local0 arg0\n"
                "    store local1 arg1\n"
                "    %5:bool = load local0\n"
                "    %7:bool = load local1\n"
                "    %8:bool = and %5 %7\n"
                "    %9:bool = not %8\n"
                "    store local2 %8\n"
                "    if %9\n"
                "    {\n"
                "      %12:bool = load local0\n"
                "      store local3 %12\n"
                "      if %12\n"
                "      {\n"
                "        %14:bool = call g\n"
                "        store local3 %14\n"
                "      }\n"
                "      %17:bool = load local3\n"
                "      store local2 %17\n"
                "    }\n"
                "    %20:bool = load local2\n"
                "    return %20\n"
                "  }\n"
                "function bool g()\n"
                "  {\n"
                "    return true:bool\n"
                "  }\n");
    expect_form("int g(); int f(bool c, int x) { int y = c ? x : 2; return c ? g() : y; } "
                "int g() { return 1; }",
                "function int f(in bool c, in int x)\n"
                "  local0 bool c\n"
                "  local1 int x\n"
                "  local2 int y\n"
                "  local3 int temporary\n"
                "  {\n"
                "    store local0 arg0\n"
                "    store local1 arg1\n"
                "    %5:bool = load local0\n"
                "    %7:int = load local1\n"
                "    %9:int = select %5 %7 2:int\n"
                "    store local2 %9\n"
                "    %12:bool = load local0\n"
                "    if %12\n"
                "    {\n"
                "      %13:int = call g\n"
                "      store local3 %13\n"
                "    }\n"
                "    else\n"
                "    {\n"
                "      %16:int = load local2\n"
                "      store local3 %16\n"
                "    }\n"
                "    %19:int = load local3\n"
                "    return %19\n"
                "  }\n"
                "function int g()\n"
                "  {\n"
                "    return 1:int\n"
                "  }\n");
}

TB_TEST(shader_form, outputs_are_copied_back_and_defaults_passed)
{
    expect_form("void h(out float r, inout int k, float s = 2) { r = s; k++; }\n"
                "float f() { float r; int k = 1; h(r, k); return r + k; }",
                "function void h(out float r, inout int k, in float s)\n"
                "  local0 float s\n"
                "  {\n"
                "    store local0 arg2\n"
                "    %3:float = load local0\n"
                "    store arg0 %3\n"
                "    %6:int = load arg1\n"
                "    %8:int = add %6 1:int\n"
                "    store arg1 %8\n"
                "  }\n"
                "function float f()\n"
                "  local0 float r\n"
                "  local1 int k\n"
                "  local2 float temporary\n"
                "  local3 int temporary\n"
                "  {\n"
                "    store local1 1:int\n"
                "    %4:int = load local1\n"
                "    store local3 %4\n"
                "    call h local2 local3 2.0:float\n"
                "    %9:float = load local2\n"
                "    store local0 %9\n"
                "    %12:int = load local3\n"
                "    store local1 %12\n"
                "    %15:float = load local0\n"
                "    %17:int = load local1\n"
                "    %18:float = convert %17\n"
                "    %19:float = add %15 %18\n"
                "    return %19\n"
                "  }\n");
}

TB_TEST(shader_form, constructors_casts_and_lists_build_values)
{
    expect_form(
        "struct S { float a; int b[2]; };\n"
        "float4 f(float2 v, float4 w) { S s = {1, 2, 3}; float2x2 m = float2x2(1, 2, 3, 4); "
        "float a[3] = {1, 2, 3}; S z = (S)0; return float4(v, (float2)w) + m[1].x + "
        "s.b[1] + z.a + a[2]; }",
        "struct S { float a, int[2] b }\n"
        "function float4 f(in float2 v, in float4 w)\n"
        "  local0 float2 v\n"
        "  local1 float4 w\n"
        "  local2 S s\n"
        "  local3 float2x2 m\n"
        "  local4 float[3] a\n"
        "  local5 S z\n"
        "  {\n"
        "    store local0 arg0\n"
        "    store local1 arg1\n"
        "    %8:int[2] = compose 2:int 3:int\n"
        "    %9:S = compose 1.0:float %8\n"
        "    store local2 %9\n"
        "    %15:float2 = compose 1.0:float 2.0:float\n"
        "    %16:float2 = compose 3.0:float 4.0:float\n"
        "    %17:float2x2 = compose %15 %16\n"
        "    store local3 %17\n"
        "    %25:float[3] = compose 1.0:float 2.0:float 3.0:float\n"
        "    store local4 %25\n"
        "    %29:int[2] = compose 0:int 0:int\n"
        "    %30:S = compose 0.0:float %29\n"
        "    store local5 %30\n"
        "    %33:float2 = load local0\n"
        "    %35:float4 = load local1\n"
        "    %36:float2 = swizzle %35 xy\n"
        "    %37:float4 = compose %33 %36\n"
        "    %39:float2* function = access_index local3 1\n"
        "    %40:float2 = load %39\n"
        "    %41:float = swizzle %40 x\n"
        "    %42:float4 = splat %41\n"
        "    %43:float4 = add %37 %42\n"
        "    %45:int[2]* function = access_index local2 1\n"
        "    %46:int* function = access_index %45 1\n"
        "    %47:int = load %46\n"
        "    %48:float = convert %47\n"
        "    %49:float4 = splat %48\n"
        "    %50:float4 = add %43 %49\n"
        "    %52:float* function = access_index local5 0\n"
        "    %53:float = load %52\n"
        "    %54:float4 = splat %53\n"
        "    %55:float4 = add %50 %54\n"
        "    %57:float* function = access_index local4 2\n"
        "    %58:float = load %57\n"
        "    %59:float4 = splat %58\n"
        "    %60:float4 = add %55 %59\n"
        "    return %60\n"
        "  }\n");
}

TB_TEST(shader_form, intrinsics_and_globals)
{
    expect_form("static float g; groupshared uint counter;\n"
                "float f(float3 x) { float3 s, c; sincos(x, s, c); InterlockedAdd(counter, 1); g = "
                "length(s); return g + c.x; }",
                "global0 private float g\n"
                "global1 groupshared uint counter\n"
                "function float f(in float3 x)\n"
                "  local0 float3 x\n"
                "  local1 float3 s\n"
                "  local2 float3 c\n"
                "  {\n"
                "    store local0 arg0\n"
                "    %3:float3 = load local0\n"
                "    call sincos %3 local1 local2\n"
                "    call InterlockedAdd global1 1:uint\n"
                "    %9:float3 = load local1\n"
                "    %10:float = length %9\n"
                "    store global0 %10\n"
                "    %13:float = load global0\n"
                "    %15:float3 = load local2\n"
                "    %16:float = swizzle %15 x\n"
                "    %17:float = add %13 %16\n"
                "    return %17\n"
                "  }\n");
}

TB_TEST(shader_form, methods_take_a_pointer_to_their_object)
{
    /* A method changes its object through `this`; a static method has none. */
    expect_form("struct Counter { int n; void add(int k) { n += k; } int get() { return this.n; } "
                "static int twice(int x) { return 2 * x; } };\n"
                "int f() { Counter c; c.n = 0; c.add(3); return c.get() + Counter::twice(c.n); }",
                "struct Counter { int n }\n"
                "function void Counter::add(inout Counter this, in int k)\n"
                "  local0 int k\n"
                "  {\n"
                "    store local0 arg1\n"
                "    %3:int* function = access_index arg0 0\n"
                "    %4:int = load %3\n"
                "    %6:int = load local0\n"
                "    %7:int = add %4 %6\n"
                "    store %3 %7\n"
                "  }\n"
                "function int Counter::get(inout Counter this)\n"
                "  {\n"
                "    %1:int* function = access_index arg0 0\n"
                "    %2:int = load %1\n"
                "    return %2\n"
                "  }\n"
                "function int Counter::twice(in int x)\n"
                "  local0 int x\n"
                "  {\n"
                "    store local0 arg0\n"
                "    %4:int = load local0\n"
                "    %5:int = multiply 2:int %4\n"
                "    return %5\n"
                "  }\n"
                "function int f()\n"
                "  local0 Counter c\n"
                "  {\n"
                "    %2:int* function = access_index local0 0\n"
                "    store %2 0:int\n"
                "    call Counter::add local0 3:int\n"
                "    %6:int = call Counter::get local0\n"
                "    %8:int* function = access_index local0 0\n"
                "    %9:int = load %8\n"
                "    %10:int = call Counter::twice %9\n"
                "    %11:int = add %6 %10\n"
                "    return %11\n"
                "  }\n");
    /* Defined outside its struct, and called on an object that is no local. */
    expect_form("struct Light { float r; float area(); };\n"
                "float Light::area() { return r * r; }\n"
                "static Light sun;\n"
                "float f(Light l) { return l.area() + sun.area(); }",
                "struct Light { float r }\n"
                "global0 private Light sun\n"
                "function float Light::area(inout Light this)\n"
                "  {\n"
                "    %1:float* function = access_index arg0 0\n"
                "    %2:float = load %1\n"
                "    %4:float* function = access_index arg0 0\n"
                "    %5:float = load %4\n"
                "    %6:float = multiply %2 %5\n"
                "    return %6\n"
                "  }\n"
                "function float f(in Light l)\n"
                "  local0 Light l\n"
                "  local1 Light temporary\n"
                "  {\n"
                "    store local0 arg0\n"
                "    %3:float = call Light::area local0\n"
                "    %6:Light = load global0\n"
                "    store local1 %6\n"
                "    %9:float = call Light::area local1\n"
                "    %11:Light = load local1\n"
                "    store global0 %11\n"
                "    %13:float = add %3 %9\n"
                "    return %13\n"
                "  }\n");
}

TB_TEST(shader_form, static_variables_get_their_first_values_from_the_initializer)
{
    expect_form("static float g = 1; static const float3 axis = float3(0, 0, 1);\n"
                "float f() { static int count = 2; count++; return g + axis.z + count; }",
                "global0 private float g\n"
                "global1 private float3 axis\n"
                "global2 private int f::count\n"
                "initializer static initializer\n"
                "function float f()\n"
                "  {\n"
                "    %1:int = load global2\n"
                "    %3:int = add %1 1:int\n"
                "    store global2 %3\n"
                "    %5:float = load global0\n"
                "    %7:float3 = load global1\n"
                "    %8:float = swizzle %7 z\n"
                "    %9:float = add %5 %8\n"
                "    %11:int = load global2\n"
                "    %12:float = convert %11\n"
                "    %13:float = add %9 %12\n"
                "    return %13\n"
                "  }\n"
                "function void static initializer()\n"
                "  {\n"
                "    store global0 1.0:float\n"
                "    %5:float3 = compose 0.0:float 0.0:float 1.0:float\n"
                "    store global1 %5\n"
                "    store global2 2:int\n"
                "  }\n");
}

TB_TEST(shader_form, printf_takes_its_format_as_a_string)
{
    expect_form("void f(int x) { printf(\"x is %d\", x); }", "function void f(in int x)\n"
                                                             "  local0 int x\n"
                                                             "  {\n"
                                                             "    store local0 arg0\n"
                                                             "    %4:int = load local0\n"
                                                             "    call printf \"x is %d\" %4\n"
                                                             "  }\n");
}

TB_TEST(shader_form, constant_buffers_are_uniform_structs)
{
    /* A cbuffer's variables, a ConstantBuffer<S>'s members, and a loose
     * uniform, gathered into `$Globals` as DXC gathers them. */
    expect_form("cbuffer Frame : register(b1) { float time; float3 tint; };\n"
                "struct Light { float3 color; };\n"
                "ConstantBuffer<Light> light : register(b2, space1);\n"
                "float4 ambient;\n"
                "float3 f() { return tint * time + light.color + ambient.xyz; }",
                "struct Frame { float time, float3 tint }\n"
                "struct Light { float3 color }\n"
                "struct $Globals { float4 ambient }\n"
                "global0 uniform Frame Frame register(b1, space0)\n"
                "global1 uniform Light light register(b2, space1)\n"
                "global2 uniform $Globals $Globals register(b)\n"
                "function float3 f()\n"
                "  {\n"
                "    %1:float3* uniform = access_index global0 1\n"
                "    %2:float3 = load %1\n"
                "    %4:float* uniform = access_index global0 0\n"
                "    %5:float = load %4\n"
                "    %6:float3 = splat %5\n"
                "    %7:float3 = multiply %2 %6\n"
                "    %9:float3* uniform = access_index global1 0\n"
                "    %10:float3 = load %9\n"
                "    %11:float3 = add %7 %10\n"
                "    %13:float4* uniform = access_index global2 0\n"
                "    %14:float4 = load %13\n"
                "    %15:float3 = swizzle %14 xyz\n"
                "    %16:float3 = add %11 %15\n"
                "    return %16\n"
                "  }\n");
}

TB_TEST(shader_form, resources_are_handles_and_their_elements_texels)
{
    expect_form(
        "struct Light { float3 color; };\n"
        "Texture2D<float4> albedo : register(t0); SamplerState linear_sampler : register(s0);\n"
        "RWTexture2D<float> output : register(u0); StructuredBuffer<Light> lights;\n"
        "RWByteAddressBuffer counters; RWTexture2D<uint> hits : register(u1);\n"
        "float4 f(float2 uv, uint2 p) { output[p] = albedo.Sample(linear_sampler, uv).x; "
        "uint before; counters.InterlockedAdd(0, 1, before); "
        "InterlockedMax(hits[p], 2u); "
        "return albedo.Load(int3(p, 0)) + lights[1].color.x + albedo.mips[1][p]; }",
        "struct Light { float3 color }\n"
        "global0 handle Texture2D<float4> albedo register(t0, space0)\n"
        "global1 handle SamplerState linear_sampler register(s0, space0)\n"
        "global2 handle RWTexture2D<float> output register(u0, space0)\n"
        "global3 handle StructuredBuffer<Light> lights register(t)\n"
        "global4 handle RWByteAddressBuffer counters register(u)\n"
        "global5 handle RWTexture2D<uint> hits register(u1, space0)\n"
        "function float4 f(in float2 uv, in uint2 p)\n"
        "  local0 float2 uv\n"
        "  local1 uint2 p\n"
        "  local2 uint before\n"
        "  {\n"
        "    store local0 arg0\n"
        "    store local1 arg1\n"
        "    %5:Texture2D<float4> = load global0\n"
        "    %7:SamplerState = load global1\n"
        "    %9:float2 = load local0\n"
        "    %10:float4 = method Sample %5 %7 %9\n"
        "    %11:float = swizzle %10 x\n"
        "    %13:RWTexture2D<float> = load global2\n"
        "    %15:uint2 = load local1\n"
        "    %16:float* element = texel %13 %15\n"
        "    store %16 %11\n"
        "    %18:RWByteAddressBuffer = load global4\n"
        "    call method InterlockedAdd %18 0:uint 1:uint local2\n"
        "    %23:RWTexture2D<uint> = load global5\n"
        "    %25:uint2 = load local1\n"
        "    %26:uint* element = texel %23 %25\n"
        "    call InterlockedMax %26 2:uint\n"
        "    %29:Texture2D<float4> = load global0\n"
        "    %31:uint2 = load local1\n"
        "    %32:int2 = convert %31\n"
        "    %34:int3 = compose %32 0:int\n"
        "    %35:float4 = method Load %29 %34\n"
        "    %37:StructuredBuffer<Light> = load global3\n"
        "    %39:Light* element = texel %37 1:uint\n"
        "    %40:float3* element = access_index %39 0\n"
        "    %41:float3 = load %40\n"
        "    %42:float = swizzle %41 x\n"
        "    %43:float4 = splat %42\n"
        "    %44:float4 = add %35 %43\n"
        "    %47:Texture2D<float4> = load global0\n"
        "    %49:uint2 = load local1\n"
        "    %50:float4* element = texel %47 %49 mip 1:uint\n"
        "    %51:float4 = load %50\n"
        "    %52:float4 = add %44 %51\n"
        "    return %52\n"
        "  }\n");
    /* An array of resources of no size, indexed, and a sampler passed in. */
    expect_form("Texture2D textures[] : register(t0, space2);\n"
                "float4 f(uint i, float2 uv, SamplerState s) { "
                "return textures[NonUniformResourceIndex(i)].Sample(s, uv); }",
                "global0 handle Texture2D<float4>[] textures register(t0, space2)\n"
                "function float4 f(in uint i, in float2 uv, in SamplerState s)\n"
                "  local0 uint i\n"
                "  local1 float2 uv\n"
                "  local2 SamplerState s\n"
                "  {\n"
                "    store local0 arg0\n"
                "    store local1 arg1\n"
                "    store local2 arg2\n"
                "    %8:uint = load local0\n"
                "    %9:uint = NonUniformResourceIndex %8\n"
                "    %10:Texture2D<float4>* handle = access global0 %9\n"
                "    %11:Texture2D<float4> = load %10\n"
                "    %13:SamplerState = load local2\n"
                "    %15:float2 = load local1\n"
                "    %16:float4 = method Sample %11 %13 %15\n"
                "    return %16\n"
                "  }\n");
}

TB_TEST(shader_form, a_ray_query_is_changed_through_a_pointer)
{
    expect_form("RaytracingAccelerationStructure scene;\n"
                "float f(RayDesc ray) { RayQuery<0> q; q.TraceRayInline(scene, 0, 0xff, ray); "
                "q.Proceed(); return q.CommittedRayT(); }",
                "struct RayDesc { float3 Origin, float TMin, float3 Direction, float TMax }\n"
                "global0 handle RaytracingAccelerationStructure scene register(t)\n"
                "function float f(in RayDesc ray)\n"
                "  local0 RayDesc ray\n"
                "  local1 RayQuery<0> q\n"
                "  {\n"
                "    store local0 arg0\n"
                "    %4:RaytracingAccelerationStructure = load global0\n"
                "    %8:RayDesc = load local0\n"
                "    call method TraceRayInline local1 %4 0:uint 255:uint %8\n"
                "    %10:bool = call method Proceed local1\n"
                "    %12:float = call method CommittedRayT local1\n"
                "    return %12\n"
                "  }\n");
}

TB_TEST(shader_form, entry_points_pass_values_by_their_semantics)
{
    expect_form(
        "struct VSOut { float4 position : SV_Position; nointerpolation float3 color : COLOR0; "
        "};\n"
        "[shader(\"vertex\")] VSOut vs(float3 p : POSITION, uint id : SV_VertexID) { "
        "VSOut o; o.position = float4(p, 1); o.color = id; return o; }\n"
        "[shader(\"pixel\")] float4 ps(VSOut i) : SV_Target { return float4(i.color, 1); }",
        "struct VSOut { float4 position : SV_Position, nointerpolation float3 color : COLOR0 }\n"
        "entry vertex vs\n"
        "entry pixel ps\n"
        "[shader(\"vertex\")]\n"
        "function VSOut vs(in float3 p : POSITION, in uint id : SV_VertexID)\n"
        "  local0 float3 p\n"
        "  local1 uint id\n"
        "  local2 VSOut o\n"
        "  {\n"
        "    store local0 arg0\n"
        "    store local1 arg1\n"
        "    %5:float3 = load local0\n"
        "    %7:float4 = compose %5 1.0:float\n"
        "    %9:float4* function = access_index local2 0\n"
        "    store %9 %7\n"
        "    %11:uint = load local1\n"
        "    %12:float = convert %11\n"
        "    %13:float3 = splat %12\n"
        "    %15:float3* function = access_index local2 1\n"
        "    store %15 %13\n"
        "    %17:VSOut = load local2\n"
        "    return %17\n"
        "  }\n"
        "[shader(\"pixel\")]\n"
        "function float4 ps(in VSOut i) : SV_Target\n"
        "  local0 VSOut i\n"
        "  {\n"
        "    store local0 arg0\n"
        "    %3:float3* function = access_index local0 1\n"
        "    %4:float3 = load %3\n"
        "    %6:float4 = compose %4 1.0:float\n"
        "    return %6\n"
        "  }\n");
    expect_form(
        "RWBuffer<uint> outputs;\n"
        "[shader(\"compute\")] [numthreads(8, 4, 1)] void cs(uint3 id : SV_DispatchThreadID) "
        "{ outputs[id.x] = id.y; }",
        "global0 handle RWBuffer<uint> outputs register(u)\n"
        "entry compute cs\n"
        "[shader(\"compute\")]\n"
        "[numthreads(8, 4, 1)]\n"
        "function void cs(in uint3 id : SV_DispatchThreadID)\n"
        "  local0 uint3 id\n"
        "  {\n"
        "    store local0 arg0\n"
        "    %3:uint3 = load local0\n"
        "    %4:uint = swizzle %3 y\n"
        "    %6:RWBuffer<uint> = load global0\n"
        "    %8:uint3 = load local0\n"
        "    %9:uint = swizzle %8 x\n"
        "    %10:uint* element = texel %6 %9\n"
        "    store %10 %4\n"
        "  }\n");
}

TB_TEST(shader_form, geometry_streams_and_tessellation_patches)
{
    expect_form(
        "struct V { float4 p : SV_Position; };\n"
        "[shader(\"geometry\")] [maxvertexcount(3)] void gs(triangle V input[3], inout "
        "TriangleStream<V> stream) { for (uint i = 0; i < 3; i++) { stream.Append(input[i]); "
        "} stream.RestartStrip(); }",
        "struct V { float4 p : SV_Position }\n"
        "entry geometry gs\n"
        "[shader(\"geometry\")]\n"
        "[maxvertexcount(3)]\n"
        "function void gs(in triangle V[3] input, inout TriangleStream<V> stream)\n"
        "  local0 V[3] input\n"
        "  local1 uint i\n"
        "  {\n"
        "    store local0 arg0\n"
        "    store local1 0:uint\n"
        "    loop\n"
        "    {\n"
        "      %5:uint = load local1\n"
        "      %7:bool = less %5 3:uint\n"
        "      %8:bool = not %7\n"
        "      if %8\n"
        "      {\n"
        "        break\n"
        "      }\n"
        "      %12:uint = load local1\n"
        "      %13:V* function = access local0 %12\n"
        "      %14:V = load %13\n"
        "      call method Append arg1 %14\n"
        "    }\n"
        "    continuing\n"
        "    {\n"
        "      %16:uint = load local1\n"
        "      %18:uint = add %16 1:uint\n"
        "      store local1 %18\n"
        "    }\n"
        "    call method RestartStrip arg1\n"
        "  }\n");
    expect_form(
        "struct CP { float3 p : POSITION; };\n"
        "struct PC { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };\n"
        "PC constants(InputPatch<CP, 3> patch) { PC c; c.edges[0] = patch[0].p.x; "
        "c.edges[1] = 1; c.edges[2] = 1; c.inside = 1; return c; }\n"
        "[shader(\"hull\")] [domain(\"tri\")] [partitioning(\"fractional_odd\")] "
        "[outputtopology(\"triangle_cw\")] [outputcontrolpoints(3)] "
        "[patchconstantfunc(\"constants\")] [maxtessfactor(15.5)]\n"
        "CP hs(InputPatch<CP, 3> patch, uint i : SV_OutputControlPointID) { return patch[i]; }",
        "struct PC { float[3] edges : SV_TessFactor, float inside : SV_InsideTessFactor }\n"
        "struct CP { float3 p : POSITION }\n"
        "entry hull hs\n"
        "function PC constants(in InputPatch<CP, 3> patch)\n"
        "  local0 InputPatch<CP, 3> patch\n"
        "  local1 PC c\n"
        "  {\n"
        "    store local0 arg0\n"
        "    %3:InputPatch<CP, 3> = load local0\n"
        "    %5:CP* element = texel %3 0:uint\n"
        "    %6:float3* element = access_index %5 0\n"
        "    %7:float3 = load %6\n"
        "    %8:float = swizzle %7 x\n"
        "    %10:float[3]* function = access_index local1 0\n"
        "    %11:float* function = access_index %10 0\n"
        "    store %11 %8\n"
        "    %14:float[3]* function = access_index local1 0\n"
        "    %15:float* function = access_index %14 1\n"
        "    store %15 1.0:float\n"
        "    %18:float[3]* function = access_index local1 0\n"
        "    %19:float* function = access_index %18 2\n"
        "    store %19 1.0:float\n"
        "    %22:float* function = access_index local1 1\n"
        "    store %22 1.0:float\n"
        "    %24:PC = load local1\n"
        "    return %24\n"
        "  }\n"
        "[shader(\"hull\")]\n"
        "[domain(\"tri\")]\n"
        "[partitioning(\"fractional_odd\")]\n"
        "[outputtopology(\"triangle_cw\")]\n"
        "[outputcontrolpoints(3)]\n"
        "[patchconstantfunc(\"constants\")]\n"
        "[maxtessfactor(15.5)]\n"
        "function CP hs(in InputPatch<CP, 3> patch, in uint i : SV_OutputControlPointID)\n"
        "  local0 InputPatch<CP, 3> patch\n"
        "  local1 uint i\n"
        "  {\n"
        "    store local0 arg0\n"
        "    store local1 arg1\n"
        "    %5:InputPatch<CP, 3> = load local0\n"
        "    %7:uint = load local1\n"
        "    %8:CP* element = texel %5 %7\n"
        "    %9:CP = load %8\n"
        "    return %9\n"
        "  }\n");
}

/* A source whose lowering reports `expected` first. */
static void expect_lowering_error(const char *source, const char *expected)
{
    lowered_source out = {0};
    lower_source(&out, source);
    TB_TEST_CONTEXT("source: %s", source);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.lowered.diagnostic_count, >, 0);
    TB_TEST_EXPECT_STRING(out.lowered.diagnostics[0].message, ==, expected);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_form, recursion_is_an_error)
{
    expect_lowering_error("int f(int n) { return n <= 0 ? 0 : f(n - 1); }",
                          "'f' calls itself, directly or through other functions, which HLSL "
                          "does not allow");
    expect_lowering_error(
        "int g(int n); int f(int n) { return g(n); } int g(int n) { return f(n); }",
        "'f' calls itself, directly or through other functions, which HLSL "
        "does not allow");
}

/* The form of each entry point of a source, extracted, verified and
 * printed one after the other. */
static void expect_extracted(const char *source, const char *expected)
{
    lowered_source out = {0};
    lower_source(&out, source);
    TB_TEST_CONTEXT("source: %s", source);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    tb_array *const text = TB_ARRAY_CREATE(NULL, char);
    const size_t count = tb_array_length(out.form.entry_points);
    for (size_t index = 0; index < count; index++)
    {
        tb_shader_form entry = {0};
        TB_TEST_REQUIRE_UINT(tb_shader_form_extract(&out.form, (uint32_t)index, out.arena, &entry),
                             ==, true);
        char why[256] = {0};
        TB_TEST_CONTEXT("verifier: %s", why);
        TB_TEST_EXPECT_UINT(tb_shader_form_verify(&entry, why, sizeof(why)), ==, true);
        const char *const printed = tb_shader_form_print(&entry, out.arena);
        tb_array_append(text, "----\n", 5);
        tb_array_append(text, printed, tb_str_length(printed));
    }
    const char nul = '\0';
    (void)tb_array_push(text, &nul);
    TB_TEST_EXPECT_STRING((const char *)tb_array_data(text), ==, expected);
    tb_shader_form missing = {0};
    TB_TEST_EXPECT_UINT(tb_shader_form_extract(&out.form, (uint32_t)count, out.arena, &missing), ==,
                        false);
    tb_array_destroy(text);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_form, an_entry_point_takes_only_what_it_reaches)
{
    /* Each pixel shader keeps its own texture and the sampler; only the
     * first reaches the static its helper reads, so only it keeps the
     * initializer that sets it, and neither keeps a static neither uses. */
    expect_extracted("Texture2D a : register(t0); Texture2D b : register(t1); SamplerState s;\n"
                     "static float scale = 2; static float unused;\n"
                     "float4 shade(float2 uv) { return a.Sample(s, uv) * scale; }\n"
                     "[shader(\"pixel\")] float4 ps_a(float2 uv : TEXCOORD) : SV_Target { "
                     "return shade(uv); }\n"
                     "[shader(\"pixel\")] float4 ps_b(float2 uv : TEXCOORD) : SV_Target { "
                     "unused = 1; return b.Sample(s, uv); }",
                     "----\n"
                     "global0 handle Texture2D<float4> a register(t0, space0)\n"
                     "global1 handle SamplerState s register(s)\n"
                     "global2 private float scale\n"
                     "entry pixel ps_a\n"
                     "initializer static initializer\n"
                     "function float4 shade(in float2 uv)\n"
                     "  local0 float2 uv\n"
                     "  {\n"
                     "    store local0 arg0\n"
                     "    %3:Texture2D<float4> = load global0\n"
                     "    %5:SamplerState = load global1\n"
                     "    %7:float2 = load local0\n"
                     "    %8:float4 = method Sample %3 %5 %7\n"
                     "    %10:float = load global2\n"
                     "    %11:float4 = splat %10\n"
                     "    %12:float4 = multiply %8 %11\n"
                     "    return %12\n"
                     "  }\n"
                     "[shader(\"pixel\")]\n"
                     "function float4 ps_a(in float2 uv : TEXCOORD) : SV_Target\n"
                     "  local0 float2 uv\n"
                     "  {\n"
                     "    store local0 arg0\n"
                     "    %3:float2 = load local0\n"
                     "    %4:float4 = call shade %3\n"
                     "    return %4\n"
                     "  }\n"
                     "function void static initializer()\n"
                     "  {\n"
                     "    store global2 2.0:float\n"
                     "  }\n"
                     "----\n"
                     "global0 handle Texture2D<float4> b register(t1, space0)\n"
                     "global1 handle SamplerState s register(s)\n"
                     "global2 private float unused\n"
                     "entry pixel ps_b\n"
                     "[shader(\"pixel\")]\n"
                     "function float4 ps_b(in float2 uv : TEXCOORD) : SV_Target\n"
                     "  local0 float2 uv\n"
                     "  {\n"
                     "    store local0 arg0\n"
                     "    store global2 1.0:float\n"
                     "    %5:Texture2D<float4> = load global0\n"
                     "    %7:SamplerState = load global1\n"
                     "    %9:float2 = load local0\n"
                     "    %10:float4 = method Sample %5 %7 %9\n"
                     "    return %10\n"
                     "  }\n");
}

TB_TEST(shader_form, what_the_form_does_not_hold_yet_is_reported)
{
    expect_unsupported("[shader(\"teapot\")] void f() {}",
                       "a shader attribute that names no stage is not supported by the compiler "
                       "yet");
    expect_unsupported("[numthreads(4 * 2, 1, 1)] void f() {}",
                       "an attribute's argument that is not a literal is not supported by the "
                       "compiler yet");
    expect_unsupported(
        "struct S { float a; }; ConstantBuffer<S> c[2]; float f() { return c[1].a; }",
        "an array of constant buffers is not supported by the compiler yet");
}

/* -------------------------------------------------------------------------
 * The verifier, on forms broken by hand.
 * ------------------------------------------------------------------------- */

typedef struct built
{
    tb_arena *arena;
    tb_shader_form form;
    uint32_t function;
    uint32_t f32;
    uint32_t i32;
    uint32_t boolean;
    uint32_t float2;
} built;

static void start(built *b, bool returns_float)
{
    b->arena = tb_arena_create(0);
    tb_shader_form_init(&b->form, b->arena);
    b->f32 = tb_shader_form_scalar(&b->form, TB_SHADER_SCALAR_FLOAT);
    b->i32 = tb_shader_form_scalar(&b->form, TB_SHADER_SCALAR_INT);
    b->boolean = tb_shader_form_scalar(&b->form, TB_SHADER_SCALAR_BOOL);
    b->float2 = tb_shader_form_vector(&b->form, TB_SHADER_SCALAR_FLOAT, 2);
    const uint32_t result = returns_float ? b->f32 : tb_shader_form_void(&b->form);
    b->function = tb_shader_form_add_function(&b->form, "f", result);
}

static tb_shader_form_function *function_of(built *b)
{
    return tb_shader_form_function_at(&b->form, b->function);
}

static uint32_t expression(built *b, tb_shader_form_expression_kind kind, uint32_t type,
                           uint32_t first, uint32_t second)
{
    tb_shader_form_expression made = {0};
    made.kind = kind;
    made.type = type;
    made.operands[0] = first;
    made.operands[1] = second;
    made.operands[2] = TB_SHADER_FORM_NONE;
    return tb_shader_form_add_expression(function_of(b), &made);
}

static uint32_t constant(built *b, uint32_t type, uint64_t bits)
{
    tb_shader_form_expression made = {0};
    made.kind = TB_SHADER_FORM_LITERAL;
    made.type = type;
    made.value.bits = bits;
    return tb_shader_form_add_expression(function_of(b), &made);
}

static tb_shader_form_statement statement_of(tb_shader_form_statement_kind kind)
{
    tb_shader_form_statement made = {0};
    made.kind = kind;
    made.next = TB_SHADER_FORM_NONE;
    made.operands[0] = TB_SHADER_FORM_NONE;
    made.operands[1] = TB_SHADER_FORM_NONE;
    made.first = TB_SHADER_FORM_NONE;
    made.accept = TB_SHADER_FORM_NONE;
    made.reject = TB_SHADER_FORM_NONE;
    made.body = TB_SHADER_FORM_NONE;
    made.continuing = TB_SHADER_FORM_NONE;
    made.function = TB_SHADER_FORM_NONE;
    made.result = TB_SHADER_FORM_NONE;
    return made;
}

/* Add a statement at the end of a block. */
static uint32_t add_to(built *b, uint32_t block, const tb_shader_form_statement *statement)
{
    tb_shader_form_function *const function = function_of(b);
    const uint32_t index = tb_shader_form_add_statement(function, statement);
    tb_shader_form_statement *at = tb_shader_form_statement_at(function, block);
    if (at->first == TB_SHADER_FORM_NONE)
    {
        at->first = index;
        return index;
    }
    uint32_t last = at->first;
    while (tb_shader_form_statement_at(function, last)->next != TB_SHADER_FORM_NONE)
    {
        last = tb_shader_form_statement_at(function, last)->next;
    }
    tb_shader_form_statement_at(function, last)->next = index;
    return index;
}

static uint32_t add(built *b, const tb_shader_form_statement *statement)
{
    return add_to(b, function_of(b)->body, statement);
}

static uint32_t new_block(built *b)
{
    const tb_shader_form_statement block = statement_of(TB_SHADER_FORM_BLOCK);
    return tb_shader_form_add_statement(function_of(b), &block);
}

static void emit(built *b, uint32_t block, uint32_t start, uint32_t end)
{
    tb_shader_form_statement made = statement_of(TB_SHADER_FORM_EMIT);
    made.operands[0] = start;
    made.operands[1] = end;
    (void)add_to(b, block, &made);
}

static void expect_broken(built *b, const char *message)
{
    char why[256] = {0};
    TB_TEST_EXPECT_UINT(tb_shader_form_verify(&b->form, why, sizeof(why)), ==, false);
    TB_TEST_CONTEXT("why: %s", why);
    TB_TEST_EXPECT_UINT(tb_str_ends_with(why, message), ==, true);
    tb_arena_destroy(b->arena);
}

static void expect_sound(built *b)
{
    char why[256] = {0};
    TB_TEST_CONTEXT("why: %s", why);
    TB_TEST_EXPECT_UINT(tb_shader_form_verify(&b->form, why, sizeof(why)), ==, true);
    tb_arena_destroy(b->arena);
}

TB_TEST(shader_form, the_verifier_accepts_a_sound_form)
{
    built b = {0};
    start(&b, true);
    const uint32_t local = tb_shader_form_add_local(function_of(&b), "x", b.f32);
    tb_shader_form_expression pointer = {0};
    pointer.kind = TB_SHADER_FORM_LOCAL;
    pointer.type = b.f32;
    pointer.space = TB_SHADER_FORM_FUNCTION;
    pointer.index = local;
    const uint32_t x = tb_shader_form_add_expression(function_of(&b), &pointer);
    const uint32_t one = constant(&b, b.f32, 0);
    tb_shader_form_statement store = statement_of(TB_SHADER_FORM_STORE);
    store.operands[0] = x;
    store.operands[1] = one;
    (void)add(&b, &store);
    const uint32_t loaded = expression(&b, TB_SHADER_FORM_LOAD, b.f32, x, TB_SHADER_FORM_NONE);
    emit(&b, function_of(&b)->body, loaded, loaded + 1);
    tb_shader_form_statement done = statement_of(TB_SHADER_FORM_RETURN);
    done.operands[0] = loaded;
    (void)add(&b, &done);
    expect_sound(&b);
}

TB_TEST(shader_form, the_verifier_finds_expressions_out_of_order_or_place)
{
    built b = {0};
    start(&b, false);
    const uint32_t one = constant(&b, b.f32, 0);
    (void)expression(&b, TB_SHADER_FORM_BINARY, b.f32, one, 5);
    expect_broken(&b, "an operand is not an earlier expression");

    start(&b, false);
    const uint32_t two = constant(&b, b.f32, 0);
    const uint32_t sum = expression(&b, TB_SHADER_FORM_BINARY, b.f32, two, two);
    tb_shader_form_expression_at(function_of(&b), sum)->op = TB_SHADER_FORM_OP_ADD;
    tb_shader_form_statement keep = statement_of(TB_SHADER_FORM_RETURN);
    (void)add(&b, &keep);
    expect_broken(&b, "%1 is never evaluated");

    start(&b, true);
    const uint32_t three = constant(&b, b.f32, 0);
    const uint32_t doubled = expression(&b, TB_SHADER_FORM_BINARY, b.f32, three, three);
    tb_shader_form_expression_at(function_of(&b), doubled)->op = TB_SHADER_FORM_OP_ADD;
    tb_shader_form_statement early = statement_of(TB_SHADER_FORM_RETURN);
    early.operands[0] = doubled;
    (void)add(&b, &early);
    expect_broken(&b, "%1 is used where it is not evaluated");

    start(&b, false);
    const uint32_t four = constant(&b, b.f32, 0);
    const uint32_t negated = expression(&b, TB_SHADER_FORM_UNARY, b.f32, four, TB_SHADER_FORM_NONE);
    tb_shader_form_expression_at(function_of(&b), negated)->op = TB_SHADER_FORM_OP_NEGATE;
    emit(&b, function_of(&b)->body, negated, negated + 1);
    emit(&b, function_of(&b)->body, negated, negated + 1);
    expect_broken(&b, "%1 is evaluated twice");

    /* Evaluated in a block, then used after it. */
    start(&b, true);
    const uint32_t yes = constant(&b, b.boolean, 1);
    tb_shader_form_statement test = statement_of(TB_SHADER_FORM_IF);
    test.operands[0] = yes;
    test.accept = new_block(&b);
    const uint32_t five = constant(&b, b.f32, 0);
    const uint32_t inner = expression(&b, TB_SHADER_FORM_UNARY, b.f32, five, TB_SHADER_FORM_NONE);
    tb_shader_form_expression_at(function_of(&b), inner)->op = TB_SHADER_FORM_OP_NEGATE;
    emit(&b, test.accept, inner, inner + 1);
    (void)add(&b, &test);
    tb_shader_form_statement late = statement_of(TB_SHADER_FORM_RETURN);
    late.operands[0] = inner;
    (void)add(&b, &late);
    expect_broken(&b, "%2 is used where it is not evaluated");

    start(&b, false);
    emit(&b, function_of(&b)->body, 3, 1);
    expect_broken(&b, "an emit of 3 to 1");
}

TB_TEST(shader_form, the_verifier_checks_the_types_of_expressions)
{
    built b = {0};
    start(&b, false);
    (void)constant(&b, b.float2, 0);
    expect_broken(&b, "%0: a literal is a scalar");

    start(&b, false);
    const uint32_t f = constant(&b, b.f32, 0);
    const uint32_t i = constant(&b, b.i32, 0);
    const uint32_t mixed = expression(&b, TB_SHADER_FORM_BINARY, b.f32, f, i);
    tb_shader_form_expression_at(function_of(&b), mixed)->op = TB_SHADER_FORM_OP_ADD;
    expect_broken(&b, "%2: the operands differ");

    start(&b, false);
    const uint32_t g = constant(&b, b.f32, 0);
    const uint32_t shifted = expression(&b, TB_SHADER_FORM_BINARY, b.f32, g, g);
    tb_shader_form_expression_at(function_of(&b), shifted)->op = TB_SHADER_FORM_OP_SHIFT_LEFT;
    expect_broken(&b, "%1: 'float' is not an integer");

    start(&b, false);
    const uint32_t k = constant(&b, b.i32, 0);
    const uint32_t both = expression(&b, TB_SHADER_FORM_BINARY, b.i32, k, k);
    tb_shader_form_expression_at(function_of(&b), both)->op = TB_SHADER_FORM_OP_AND;
    expect_broken(&b, "%1: a logical operator takes bools");

    start(&b, false);
    const uint32_t m = constant(&b, b.f32, 0);
    const uint32_t less = expression(&b, TB_SHADER_FORM_BINARY, b.f32, m, m);
    tb_shader_form_expression_at(function_of(&b), less)->op = TB_SHADER_FORM_OP_LESS;
    expect_broken(&b, "%1: not the operator's type");

    start(&b, false);
    const uint32_t n = constant(&b, b.f32, 0);
    (void)expression(&b, TB_SHADER_FORM_LOAD, b.f32, n, TB_SHADER_FORM_NONE);
    expect_broken(&b, "%1: a load reads a pointer of its type");

    start(&b, false);
    const uint32_t p = constant(&b, b.f32, 0);
    (void)expression(&b, TB_SHADER_FORM_SPLAT, b.f32, p, TB_SHADER_FORM_NONE);
    expect_broken(&b, "%1: a splat makes a scalar a vector or a matrix");

    start(&b, false);
    const uint32_t q = constant(&b, b.f32, 0);
    (void)expression(&b, TB_SHADER_FORM_CONVERT, b.float2, q, TB_SHADER_FORM_NONE);
    expect_broken(&b, "%1: a conversion keeps the shape");

    start(&b, false);
    const uint32_t r = constant(&b, b.f32, 0);
    tb_shader_form_expression choice = {0};
    choice.kind = TB_SHADER_FORM_SELECT;
    choice.type = b.f32;
    choice.operands[0] = r;
    choice.operands[1] = r;
    choice.operands[2] = r;
    (void)tb_shader_form_add_expression(function_of(&b), &choice);
    expect_broken(&b, "%1: a select takes a bool and two values of its type");

    start(&b, false);
    const uint32_t s = constant(&b, b.f32, 0);
    tb_shader_form_expression picked = {0};
    picked.kind = TB_SHADER_FORM_SWIZZLE;
    picked.type = b.f32;
    picked.operands[0] = s;
    picked.swizzle_count = 1;
    (void)tb_shader_form_add_expression(function_of(&b), &picked);
    expect_broken(&b, "%1: a swizzle selects from 1 to 4 of a vector's");

    start(&b, false);
    const uint32_t t = constant(&b, b.f32, 0);
    const uint32_t parts[1] = {t};
    tb_shader_form_expression made = {0};
    made.kind = TB_SHADER_FORM_COMPOSE;
    made.type = b.float2;
    made.first_operand = tb_shader_form_add_operands(function_of(&b), parts, 1);
    made.operand_count = 1;
    (void)tb_shader_form_add_expression(function_of(&b), &made);
    expect_broken(&b, "%1: 1 components make a 'float2'");

    start(&b, false);
    const uint32_t u = constant(&b, b.f32, 0);
    tb_shader_form_expression element = {0};
    element.kind = TB_SHADER_FORM_ACCESS_INDEX;
    element.type = b.f32;
    element.operands[0] = u;
    (void)tb_shader_form_add_expression(function_of(&b), &element);
    expect_broken(&b, "%1: 'float' cannot be indexed");

    start(&b, false);
    tb_shader_form_expression local = {0};
    local.kind = TB_SHADER_FORM_LOCAL;
    local.type = b.f32;
    local.space = TB_SHADER_FORM_FUNCTION;
    local.index = 4;
    (void)tb_shader_form_add_expression(function_of(&b), &local);
    expect_broken(&b, "%0: no local 4");

    start(&b, false);
    tb_shader_form_expression pointer = {0};
    pointer.kind = TB_SHADER_FORM_UNARY;
    pointer.type = b.f32;
    pointer.space = TB_SHADER_FORM_FUNCTION;
    pointer.operands[0] = constant(&b, b.f32, 0);
    (void)tb_shader_form_add_expression(function_of(&b), &pointer);
    expect_broken(&b, "%1: only a variable or an access of one is a pointer");
}

TB_TEST(shader_form, the_verifier_checks_statements)
{
    built b = {0};
    start(&b, false);
    const tb_shader_form_statement stop = statement_of(TB_SHADER_FORM_BREAK);
    (void)add(&b, &stop);
    expect_broken(&b, "a break outside a loop or switch");

    start(&b, false);
    const tb_shader_form_statement next = statement_of(TB_SHADER_FORM_CONTINUE);
    (void)add(&b, &next);
    expect_broken(&b, "a continue outside a loop");

    /* Neither jumps out of a continuing block. */
    start(&b, false);
    tb_shader_form_statement loop = statement_of(TB_SHADER_FORM_LOOP);
    loop.body = new_block(&b);
    loop.continuing = new_block(&b);
    (void)add_to(&b, loop.continuing, &stop);
    (void)add(&b, &loop);
    expect_broken(&b, "a break outside a loop or switch");

    start(&b, true);
    const tb_shader_form_statement bare = statement_of(TB_SHADER_FORM_RETURN);
    (void)add(&b, &bare);
    expect_broken(&b, "a return without the value returned");

    start(&b, true);
    tb_shader_form_statement wrong = statement_of(TB_SHADER_FORM_RETURN);
    wrong.operands[0] = constant(&b, b.i32, 0);
    (void)add(&b, &wrong);
    expect_broken(&b, "a return of another type");

    start(&b, false);
    tb_shader_form_statement into_value = statement_of(TB_SHADER_FORM_STORE);
    into_value.operands[0] = constant(&b, b.f32, 0);
    into_value.operands[1] = into_value.operands[0];
    (void)add(&b, &into_value);
    expect_broken(&b, "a store of a value of the pointer's type");

    start(&b, false);
    tb_shader_form_statement test = statement_of(TB_SHADER_FORM_IF);
    test.operands[0] = constant(&b, b.i32, 0);
    test.accept = new_block(&b);
    (void)add(&b, &test);
    expect_broken(&b, "an if tests a bool");

    start(&b, false);
    tb_shader_form_statement until = statement_of(TB_SHADER_FORM_LOOP);
    until.body = new_block(&b);
    until.continuing = new_block(&b);
    until.operands[0] = constant(&b, b.i32, 0);
    (void)add(&b, &until);
    expect_broken(&b, "a loop breaks on a bool");

    start(&b, false);
    tb_shader_form_statement by_float = statement_of(TB_SHADER_FORM_SWITCH);
    by_float.operands[0] = constant(&b, b.f32, 0);
    (void)add(&b, &by_float);
    expect_broken(&b, "a switch selects by an integer");

    start(&b, false);
    tb_shader_form_statement cases = statement_of(TB_SHADER_FORM_SWITCH);
    cases.operands[0] = constant(&b, b.i32, 0);
    tb_shader_form_statement label = statement_of(TB_SHADER_FORM_CASE);
    label.body = new_block(&b);
    label.falls_through = true;
    cases.first = tb_shader_form_add_statement(function_of(&b), &label);
    (void)add(&b, &cases);
    expect_broken(&b, "the last case falls through");

    start(&b, false);
    tb_shader_form_statement defaults = statement_of(TB_SHADER_FORM_SWITCH);
    defaults.operands[0] = constant(&b, b.i32, 0);
    tb_shader_form_statement second = statement_of(TB_SHADER_FORM_CASE);
    second.is_default = true;
    second.body = new_block(&b);
    tb_shader_form_statement first = second;
    first.body = new_block(&b);
    first.next = tb_shader_form_add_statement(function_of(&b), &second);
    defaults.first = tb_shader_form_add_statement(function_of(&b), &first);
    (void)add(&b, &defaults);
    expect_broken(&b, "a switch has two defaults");

    start(&b, false);
    tb_shader_form_statement shared = statement_of(TB_SHADER_FORM_IF);
    shared.operands[0] = constant(&b, b.boolean, 1);
    shared.accept = new_block(&b);
    shared.reject = shared.accept;
    (void)add(&b, &shared);
    expect_broken(&b, "is in two places");

    /* A constant buffer is read, never written. */
    start(&b, false);
    tb_shader_form_global input = {"Frame", b.f32, TB_SHADER_FORM_UNIFORM, {'b', false, 0, 0}};
    (void)tb_shader_form_add_global(&b.form, &input);
    tb_shader_form_expression pointer = {0};
    pointer.kind = TB_SHADER_FORM_GLOBAL;
    pointer.type = b.f32;
    pointer.space = TB_SHADER_FORM_UNIFORM;
    tb_shader_form_statement into_input = statement_of(TB_SHADER_FORM_STORE);
    into_input.operands[0] = tb_shader_form_add_expression(function_of(&b), &pointer);
    into_input.operands[1] = constant(&b, b.f32, 0);
    (void)add(&b, &into_input);
    expect_broken(&b, "a store into what cannot be written");

    start(&b, false);
    const tb_shader_form_statement stray = statement_of(TB_SHADER_FORM_CASE);
    (void)add(&b, &stray);
    expect_broken(&b, "a case outside a switch");
}

/* A texel of a resource's handle `resource`, at `location`. */
static uint32_t texel_of(built *b, uint32_t resource, uint32_t location, uint32_t type)
{
    tb_shader_form_expression made = {0};
    made.kind = TB_SHADER_FORM_TEXEL;
    made.type = type;
    made.space = TB_SHADER_FORM_ELEMENT;
    made.operands[0] = resource;
    made.operands[1] = location;
    made.operands[2] = TB_SHADER_FORM_NONE;
    return tb_shader_form_add_expression(function_of(b), &made);
}

static uint32_t handle_of(built *b, tb_shader_object object, bool global_handle)
{
    const uint32_t type = tb_shader_form_resource(&b->form, object, b->f32, 0, 0, "resource");
    tb_shader_form_global global = {"r", type, TB_SHADER_FORM_HANDLE, {'t', false, 0, 0}};
    const uint32_t index = tb_shader_form_add_global(&b->form, &global);
    tb_shader_form_expression pointer = {0};
    pointer.kind = TB_SHADER_FORM_GLOBAL;
    pointer.type = type;
    pointer.space = TB_SHADER_FORM_HANDLE;
    pointer.index = index;
    const uint32_t at = tb_shader_form_add_expression(function_of(b), &pointer);
    if (global_handle)
    {
        return at;
    }
    return expression(b, TB_SHADER_FORM_LOAD, type, at, TB_SHADER_FORM_NONE);
}

TB_TEST(shader_form, the_verifier_checks_texels)
{
    built b = {0};
    start(&b, false);
    const uint32_t scalar = constant(&b, b.f32, 0);
    (void)texel_of(&b, scalar, constant(&b, b.i32, 0), b.f32);
    expect_broken(&b, "%2: a texel is of a resource");

    start(&b, false);
    const uint32_t texture = handle_of(&b, TB_SHADER_OBJECT_TEXTURE2D, false);
    (void)texel_of(&b, texture, constant(&b, b.f32, 0), b.f32);
    expect_broken(&b, "%3: a texel's location is of integers");

    start(&b, false);
    const uint32_t other = handle_of(&b, TB_SHADER_OBJECT_TEXTURE2D, false);
    (void)texel_of(&b, other, constant(&b, b.i32, 0), b.i32);
    expect_broken(&b, "%3: not the type the resource holds");

    start(&b, false);
    const uint32_t leveled = handle_of(&b, TB_SHADER_OBJECT_TEXTURE2D, false);
    const uint32_t at_level = texel_of(&b, leveled, constant(&b, b.i32, 0), b.f32);
    tb_shader_form_expression_at(function_of(&b), at_level)->index = 1;
    tb_shader_form_expression_at(function_of(&b), at_level)->operands[2] = constant(&b, b.f32, 0);
    expect_broken(&b, "%3: a texel's level or sample is an earlier integer");

    /* A texture that is not written cannot be stored to. */
    start(&b, false);
    const uint32_t read_only = handle_of(&b, TB_SHADER_OBJECT_TEXTURE2D, false);
    const uint32_t place = texel_of(&b, read_only, constant(&b, b.i32, 0), b.f32);
    emit(&b, function_of(&b)->body, 0, place + 1);
    tb_shader_form_statement store = statement_of(TB_SHADER_FORM_STORE);
    store.operands[0] = place;
    store.operands[1] = constant(&b, b.f32, 0);
    (void)add(&b, &store);
    expect_broken(&b, "a store into what cannot be written");

    /* One that is written can. */
    start(&b, false);
    const uint32_t writable = handle_of(&b, TB_SHADER_OBJECT_RW_TEXTURE2D, false);
    const uint32_t written = texel_of(&b, writable, constant(&b, b.i32, 0), b.f32);
    emit(&b, function_of(&b)->body, 0, written + 1);
    tb_shader_form_statement into = statement_of(TB_SHADER_FORM_STORE);
    into.operands[0] = written;
    into.operands[1] = constant(&b, b.f32, 0);
    (void)add(&b, &into);
    expect_sound(&b);

    start(&b, false);
    tb_shader_form_expression method = {0};
    method.kind = TB_SHADER_FORM_INTRINSIC;
    method.type = b.f32;
    method.method = 1;
    (void)tb_shader_form_add_expression(function_of(&b), &method);
    expect_broken(&b, "%0: a method takes its object first");
}

TB_TEST(shader_form, the_verifier_checks_calls)
{
    built b = {0};
    start(&b, false);
    const tb_shader_form_statement nothing = statement_of(TB_SHADER_FORM_CALL);
    (void)add(&b, &nothing);
    expect_broken(&b, "a call of nothing");

    start(&b, false);
    tb_shader_form_statement self = statement_of(TB_SHADER_FORM_CALL);
    self.function = b.function;
    self.operand_count = 1;
    const uint32_t argument = constant(&b, b.f32, 0);
    self.first_operand = tb_shader_form_add_operands(function_of(&b), &argument, 1);
    (void)add(&b, &self);
    expect_broken(&b, "a call of 'f' with 1 arguments");

    start(&b, true);
    tb_shader_form_statement unused = statement_of(TB_SHADER_FORM_CALL);
    unused.function = b.function;
    (void)add(&b, &unused);
    expect_broken(&b, "a call that returns a value has no result");

    start(&b, true);
    tb_shader_form_statement mistyped = statement_of(TB_SHADER_FORM_CALL);
    mistyped.function = b.function;
    mistyped.result =
        expression(&b, TB_SHADER_FORM_CALL_RESULT, b.i32, TB_SHADER_FORM_NONE, TB_SHADER_FORM_NONE);
    (void)add(&b, &mistyped);
    expect_broken(&b, "a call's result is not of the type returned");

    start(&b, false);
    tb_shader_form_statement past = statement_of(TB_SHADER_FORM_CALL);
    past.intrinsic = (uint32_t)TB_SHADER_INTRINSIC_ABORT;
    past.first_operand = 40;
    past.operand_count = 2;
    (void)add(&b, &past);
    expect_broken(&b, "a call's operands are past the end");

    start(&b, false);
    tb_shader_form_parameter parameter = {"r", b.f32, TB_SHADER_FORM_OUT, NULL, 0};
    (void)tb_shader_form_add_parameter(function_of(&b), &parameter);
    tb_shader_form_statement by_value = statement_of(TB_SHADER_FORM_CALL);
    by_value.function = b.function;
    const uint32_t value = constant(&b, b.f32, 0);
    by_value.first_operand = tb_shader_form_add_operands(function_of(&b), &value, 1);
    by_value.operand_count = 1;
    (void)add(&b, &by_value);
    expect_broken(&b, "argument 1 of 'f' is not of its parameter's type");
}

TB_TEST(shader_form, the_verifier_checks_the_module)
{
    built b = {0};
    start(&b, false);
    tb_shader_form_global global = {"g", b.f32, TB_SHADER_FORM_FUNCTION, {0, false, 0, 0}};
    (void)tb_shader_form_add_global(&b.form, &global);
    expect_broken(&b, "global 0: a global is private, groupshared, uniform or a handle");

    start(&b, false);
    const tb_shader_form_member later = {"a", 99, NULL, 0};
    (void)tb_shader_form_struct(&b.form, "S", &later, 1);
    expect_broken(&b, "a member of a type made after it");

    start(&b, false);
    (void)tb_shader_form_add_local(function_of(&b), "x", 77);
    expect_broken(&b, "local 0: no such type");

    /* A function is an entry point of a stage once. */
    start(&b, false);
    TB_TEST_EXPECT_UINT(
        tb_shader_form_add_entry_point(&b.form, b.function, TB_SHADER_FORM_STAGE_COMPUTE), ==,
        true);
    TB_TEST_EXPECT_UINT(
        tb_shader_form_add_entry_point(&b.form, b.function, TB_SHADER_FORM_STAGE_COMPUTE), ==,
        false);
    TB_TEST_EXPECT_STRING(tb_shader_form_stage_name(TB_SHADER_FORM_STAGE_CLOSEST_HIT), ==,
                          "closesthit");
    TB_TEST_EXPECT_STRING(tb_shader_form_stage_name(TB_SHADER_FORM_STAGE_COUNT), ==, "?");
    (void)tb_shader_form_add_entry_point(&b.form, 99, TB_SHADER_FORM_STAGE_PIXEL);
    expect_broken(&b, "entry point 1: no such function or stage");
}

TB_TEST(shader_form, types_are_interned_and_named_as_hlsl_names_them)
{
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_form form = {0};
    tb_shader_form_init(&form, arena);
    const uint32_t matrix = tb_shader_form_matrix(&form, TB_SHADER_SCALAR_HALF, 3, 4);
    TB_TEST_EXPECT_UINT(tb_shader_form_matrix(&form, TB_SHADER_SCALAR_HALF, 3, 4), ==, matrix);
    TB_TEST_EXPECT_STRING(tb_shader_form_type_at(&form, matrix)->name, ==, "half3x4");
    const uint32_t array = tb_shader_form_array(&form, matrix, 2);
    TB_TEST_EXPECT_STRING(tb_shader_form_type_at(&form, array)->name, ==, "half3x4[2]");
    TB_TEST_EXPECT_UINT(tb_shader_form_component_count(&form, matrix), ==, 12);
    TB_TEST_EXPECT_UINT(tb_shader_form_component_count(&form, array), ==, 0);
    const uint32_t bools = tb_shader_form_shaped(&form, TB_SHADER_SCALAR_BOOL, matrix);
    TB_TEST_EXPECT_STRING(tb_shader_form_type_at(&form, bools)->name, ==, "bool3x4");
    TB_TEST_EXPECT_UINT(
        tb_shader_form_find_numeric(&form, TB_SHADER_FORM_VECTOR, TB_SHADER_SCALAR_DOUBLE, 1, 3),
        ==, TB_SHADER_FORM_NONE);
    tb_arena_destroy(arena);
}
