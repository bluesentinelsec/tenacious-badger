/* The shader preprocessor: virtual paths, includes, macros, conditionals,
 * `#line` and pragmas, the errors of each and where they are reported, the
 * limits that bound what a source can make, and loading from a VFS. */
#include "badger/arena.h"
#include "badger/fmt.h"
#include "badger/package.h"
#include "badger/shader.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/vfs.h"
#include "tb_test.h"

#include <stdbool.h>
#include <string.h>

#define MAIN_PATH "/Engine/Private/Main.hlsl"

/* A shader of one or more files in memory, the first its main file. */
typedef struct sample_file
{
    const char *path;
    const char *text;
} sample_file;

typedef struct run
{
    tb_arena *arena;
    tb_shader_result result;
    tb_shader_preprocessed output;
} run;

static void run_files(run *out, const sample_file *files, size_t count,
                      const tb_shader_define *defines, size_t define_count, unsigned flags)
{
    tb_shader_file entries[16] = {0};
    TB_TEST_REQUIRE_UINT(count, <=, TB_TEST_COUNT_OF(entries));
    for (size_t index = 0; index < count; index++)
    {
        entries[index].path = files[index].path;
        entries[index].text = files[index].text;
        entries[index].length = tb_str_length(files[index].text);
    }
    tb_shader_preprocess_request request = {0};
    request.path = files[0].path;
    request.files = entries;
    request.file_count = count;
    request.defines = defines;
    request.define_count = define_count;
    request.flags = flags;
    out->arena = tb_arena_create(0);
    out->result = tb_shader_preprocess(&request, out->arena, &out->output);
}

/* Preprocess one main file. */
static void run_text(run *out, const char *text)
{
    const sample_file file = {MAIN_PATH, text};
    run_files(out, &file, 1, NULL, 0, 0);
}

static void finish(run *out)
{
    tb_arena_destroy(out->arena);
}

/* Preprocess one main file that must succeed with no diagnostics, and check its text. */
static void expect_text(const char *source, const char *expected)
{
    run out = {0};
    run_text(&out, source);
    TB_TEST_CONTEXT("source: %s", source);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    TB_TEST_EXPECT_UINT(out.output.diagnostic_count, ==, 0);
    TB_TEST_EXPECT_STRING(out.output.text, ==, expected);
    finish(&out);
}

/* The first diagnostic, formatted as editors read it. */
static void first_diagnostic(const run *out, char *buffer, size_t size)
{
    TB_TEST_REQUIRE_UINT(out->output.diagnostic_count, >, 0);
    (void)tb_shader_diagnostic_format(&out->output.diagnostics[0], buffer, size);
}

/* Preprocess one main file that must fail, and check its first error. */
static void expect_error(const char *source, const char *expected)
{
    run out = {0};
    run_text(&out, source);
    TB_TEST_CONTEXT("source: %s", source);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_EXPECT_STRING(out.output.text, ==, "");
    char message[256] = {0};
    first_diagnostic(&out, message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==, expected);
    finish(&out);
}

/* -------------------------------------------------------------------------
 * Virtual paths.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_preprocess, virtual_paths_are_checked_before_use)
{
    /* A path names a root and a file in it, the same way on every system. */
    static const char *const valid[] = {
        "/Engine/Public/Common.hlsli",
        "/Engine/Private/DeferredLighting.hlsl",
        "/Game/Water.hlsl",
        "/Plugin/Ocean/Private/Waves.hlsli",
    };
    static const char *const invalid[] = {
        "",
        "/",
        "Engine/Common.hlsli",
        "/Common.hlsli",
        "/Engine//Common.hlsli",
        "/Engine/./Common.hlsli",
        "/Engine/../Common.hlsli",
        "/Engine/Common.txt",
        "/Engine/Common.HLSL",
        "/Engine/Common.hlsl/",
        "/Engine\\Common.hlsli",
        "/C:/Common.hlsli",
        "/Engine/My Common.hlsli",
        "/Engine/Tab\tCommon.hlsli",
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(valid); index++)
    {
        TB_TEST_CONTEXT("path: %s", valid[index]);
        TB_TEST_EXPECT(tb_shader_path_check(valid[index], tb_str_length(valid[index])));
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(invalid); index++)
    {
        TB_TEST_CONTEXT("path: %s", invalid[index]);
        TB_TEST_EXPECT(!tb_shader_path_check(invalid[index], tb_str_length(invalid[index])));
    }
    TB_TEST_EXPECT(!tb_shader_path_check(NULL, 0));
}

TB_TEST(shader_preprocess, a_request_that_is_malformed_says_why)
{
    /* The main file must be a valid .hlsl path; each file and define must be well formed. */
    static const struct
    {
        const char *path;
        const char *define;
        const char *value;
        const char *file;
        const char *expected;
    } cases[] = {
        {"Engine/Main.hlsl", NULL, NULL, NULL,
         "error: 'Engine/Main.hlsl' is not a valid shader path"},
        {"/Engine/Common.hlsli", NULL, NULL, NULL,
         "error: '/Engine/Common.hlsli' is not a .hlsl file, as a shader's main file must be"},
        {MAIN_PATH, "2X", NULL, NULL, "error: define '2X' is not an identifier"},
        {MAIN_PATH, "", NULL, NULL, "error: define '' is not an identifier"},
        {MAIN_PATH, "X", "1\n2", NULL, "error: the value of define 'X' has a line break"},
        {MAIN_PATH, NULL, NULL, "/Engine/../X.hlsli",
         "error: file '/Engine/../X.hlsli' does not have a valid shader path"},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        TB_TEST_CONTEXT("case %zu", index);
        const tb_shader_define define = {cases[index].define, cases[index].value};
        const tb_shader_file file = {cases[index].file, "", 0};
        tb_shader_preprocess_request request = {0};
        request.path = cases[index].path;
        request.defines = &define;
        request.define_count = cases[index].define != NULL ? 1 : 0;
        request.files = &file;
        request.file_count = cases[index].file != NULL ? 1 : 0;
        tb_arena *const arena = tb_arena_create(0);
        tb_shader_preprocessed output = {0};
        TB_TEST_EXPECT_UINT(tb_shader_preprocess(&request, arena, &output), ==, TB_SHADER_INVALID);
        char message[256] = {0};
        TB_TEST_REQUIRE_UINT(output.diagnostic_count, ==, 1);
        (void)tb_shader_diagnostic_format(&output.diagnostics[0], message, sizeof(message));
        TB_TEST_EXPECT_STRING(message, ==, cases[index].expected);
        tb_arena_destroy(arena);
    }
}

TB_TEST(shader_preprocess, a_main_file_that_is_missing_fails)
{
    tb_shader_preprocess_request request = {0};
    request.path = MAIN_PATH;
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_preprocessed output = {0};
    TB_TEST_EXPECT_UINT(tb_shader_preprocess(&request, arena, &output), ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(output.diagnostic_count, ==, 1);
    TB_TEST_EXPECT_STRING(output.diagnostics[0].message, ==, "'" MAIN_PATH "' was not found");
    TB_TEST_EXPECT_STRING(output.diagnostics[0].path, ==, "");
    TB_TEST_EXPECT_UINT(output.dependency_count, ==, 0);
    tb_arena_destroy(arena);
}

/* -------------------------------------------------------------------------
 * Text and tokens.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_preprocess, text_keeps_each_token_on_its_line)
{
    /* Comments go, space shrinks to one, and lines stay where they were, so
     * a later compiler's line numbers match the source. */
    expect_text("float4 main() : SV_Target   // the color\n"
                "{\n"
                "    /* nothing\n"
                "       here */ return float4(1, 0, 0, 1);\n"
                "\n"
                "}\n",
                "float4 main() : SV_Target\n"
                "{\n"
                "\n"
                "return float4(1, 0, 0, 1);\n"
                "\n"
                "}\n");
    expect_text("", "");
    expect_text("// only a comment\n", "");
}

TB_TEST(shader_preprocess, lines_end_and_join_every_way)
{
    /* CRLF and CR end lines as LF does; a backslash at the end of a line
     * joins it to the next, even inside a token; a byte order mark is skipped. */
    expect_text("a\r\nb\rc\n", "a\nb\nc\n");
    /* Tokens keep the lines they are on, so the token after a joined one is on the next. */
    expect_text("fl\\\noat x;\n", "float\nx;\n");
    expect_text("#define A 1 + \\\n  2\nA\n", "\n\n1 + 2\n");
    expect_text("\xEF\xBB\xBF"
                "float x;\n",
                "float x;\n");
    expect_text("// a comment \\\n that goes on\nx\n", "\n\nx\n");
}

TB_TEST(shader_preprocess, tokens_that_would_merge_are_kept_apart)
{
    /* Macros can put tokens side by side that were never written so; the
     * text separates any that would read back as one. */
    expect_text("#define PLUS +\n+PLUS\n", "\n+ +\n");
    expect_text("#define ID(x) x\nID(1)ID(e)\n", "\n1 e\n");
    expect_text("#define ID(x) x\nID(1)ID(.5)\n", "\n1 .5\n");
    expect_text("#define SLASH /\nSLASH/x\n", "\n/ /x\n");
    expect_text("#define A a\nA.b\n", "\na.b\n");
}

TB_TEST(shader_preprocess, unterminated_tokens_are_errors_where_they_are_read)
{
    expect_error("float x = \"open;\n", MAIN_PATH "(1,11): error: unterminated string");
    expect_error("x = 'a;\n", MAIN_PATH "(1,5): error: unterminated character constant");
    expect_error("x /* never closed\n", MAIN_PATH "(1,3): error: unterminated comment");
    /* A skipped group is not read, so an apostrophe in prose there is fine. */
    expect_text("#if 0\nthis isn't code\n#endif\nx\n", "\n\n\nx\n");
}

TB_TEST(shader_preprocess, a_nul_byte_in_a_file_is_refused)
{
    static const char text[] = {'x', '\0', 'y'};
    tb_shader_file file = {MAIN_PATH, text, sizeof(text)};
    tb_shader_preprocess_request request = {0};
    request.path = MAIN_PATH;
    request.files = &file;
    request.file_count = 1;
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_preprocessed output = {0};
    TB_TEST_EXPECT_UINT(tb_shader_preprocess(&request, arena, &output), ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(output.diagnostic_count, ==, 1);
    TB_TEST_EXPECT_STRING(output.diagnostics[0].message, ==, "'" MAIN_PATH "' holds a NUL byte");
    tb_arena_destroy(arena);
}

/* -------------------------------------------------------------------------
 * Macros.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_preprocess, object_macros_expand_and_are_read_again)
{
    expect_text("#define A B\n#define B 1\nA\n", "\n\n1\n");
    expect_text("#define EMPTY\nx EMPTY y\n", "\nx y\n");
    /* A macro never expands inside its own expansion, directly or through another. */
    expect_text("#define X X + 1\nX\n", "\nX + 1\n");
    expect_text("#define F G\n#define G F\nF G\n", "\n\nF G\n");
    expect_text("#define A 1\n#undef A\nA\n", "\n\nA\n");
    /* An identical redefinition is allowed in silence. */
    expect_text("#define A 1 + 2\n#define A 1 + 2\nA\n", "\n\n1 + 2\n");
}

TB_TEST(shader_preprocess, function_macros_take_their_arguments)
{
    expect_text("#define MAX(a, b) ((a) > (b) ? (a) : (b))\nMAX(x, y + 1)\n",
                "\n((x) > (y + 1) ? (x) : (y + 1))\n");
    /* Parentheses protect commas; arguments are expanded before they go in. */
    expect_text("#define FIRST(a, b) a\nFIRST((p, q), r)\n", "\n(p, q)\n");
    expect_text("#define TWICE(a) a a\n#define ONE 1\nTWICE(ONE)\n", "\n\n1 1\n");
    expect_text("#define ID(a) a\nID(ID(ID(z)))\n", "\nz\n");
    /* A name without `(` after it is not an invocation. */
    expect_text("#define F(a) a\nF + 1\n", "\nF + 1\n");
    /* `()` gives a macro of no parameters nothing, and one of one an empty argument. */
    expect_text("#define NONE() n\n#define ONE(a) [a]\nNONE() ONE()\n", "\n\nn []\n");
    /* An invocation may span lines, and each argument stays on its own. */
    expect_text("#define ADD(a, b) a + b\nADD(1,\n2)\n", "\n1 +\n2\n");
    /* The result is read again with what follows, so a macro may build an invocation. */
    expect_text("#define F(a) a\n#define G F\nG(5)\n", "\n\n5\n");
}

TB_TEST(shader_preprocess, the_wrong_number_of_arguments_is_an_error)
{
    expect_error("#define F(a, b) a\nF(1)\n",
                 MAIN_PATH "(2,1): error: macro 'F' takes 2 arguments, but 1 was given");
    expect_error("#define F(a) a\nF(1, 2, 3)\n",
                 MAIN_PATH "(2,1): error: macro 'F' takes 1 argument, but 3 were given");
    expect_error("#define F(a) a\nF(1,\n",
                 MAIN_PATH "(2,1): error: unterminated invocation of macro 'F'");
    expect_error("#define F(a) a\nF(1\n#define G\n)\n",
                 MAIN_PATH "(2,1): error: unterminated invocation of macro 'F'");
}

TB_TEST(shader_preprocess, hash_makes_a_string_of_an_argument)
{
    expect_text("#define STR(x) #x\nSTR(a   +  b)\n", "\n\"a + b\"\n");
    expect_text("#define STR(x) #x\nSTR(\"say \\\"hi\\\"\\n\")\n",
                "\n\"\\\"say \\\\\\\"hi\\\\\\\"\\\\n\\\"\"\n");
    expect_text("#define STR(x) #x\nSTR()\n", "\n\"\"\n");
    /* A string of a macro's expansion takes a second macro. */
    expect_text("#define STR(x) #x\n#define XSTR(x) STR(x)\n#define N 4\nSTR(N) XSTR(N)\n",
                "\n\n\n\"N\" \"4\"\n");
}

TB_TEST(shader_preprocess, double_hash_pastes_two_tokens_into_one)
{
    expect_text("#define CAT(a, b) a##b\nCAT(Texture, 2D)\n", "\nTexture2D\n");
    expect_text("#define CAT(a, b) a ## b\nCAT(x, ) CAT(, y) CAT(,)\n", "\nx y\n");
    expect_text("#define CAT3(a, b, c) a##b##c\nCAT3(R, W, Buffer)\n", "\nRWBuffer\n");
    expect_text("#define SUFFIX(a) a##_t\nSUFFIX(uint)\n", "\nuint_t\n");
    expect_text("#define SHIFT(a) a ## <\nSHIFT(<)\n", "\n<<\n");
    /* An operand of `##` is pasted as written, not expanded first. */
    expect_text("#define N 4\n#define CAT(a, b) a##b\nCAT(N, x)\n", "\n\nNx\n");
    expect_error("#define CAT(a, b) a##b\nCAT(+, /)\n",
                 MAIN_PATH "(2,5): error: pasting '+' and '/' does not give a valid token");
}

TB_TEST(shader_preprocess, variadic_macros_take_the_rest)
{
    expect_text("#define CALL(f, ...) f(__VA_ARGS__)\nCALL(g, a, b, c)\n", "\ng(a, b, c)\n");
    expect_text("#define LIST(...) [__VA_ARGS__]\nLIST() LIST(1) LIST(1, 2)\n",
                "\n[] [1] [1, 2]\n");
    /* `, ## __VA_ARGS__` drops the comma when there is nothing after it. */
    expect_text("#define LOG(f, ...) print(f, ## __VA_ARGS__)\nLOG(a) LOG(a, b)\n",
                "\nprint(a) print(a, b)\n");
    expect_text("#define FIRST(a, ...) a\nFIRST(1)\n", "\n1\n");
}

TB_TEST(shader_preprocess, builtin_macros_say_where_they_are)
{
    expect_text("x\n__LINE__\n", "x\n2\n");
    expect_text("__FILE__\n", "\"" MAIN_PATH "\"\n");
    expect_text("#define HERE __LINE__\n\nHERE\n", "\n\n3\n");
}

TB_TEST(shader_preprocess, definitions_that_are_malformed_are_errors)
{
    static const struct
    {
        const char *source;
        const char *expected;
    } cases[] = {
        {"#define\n", "(1,2): error: #define needs a macro name"},
        {"#define 1 x\n", "(1,2): error: #define needs a macro name"},
        {"#define defined 1\n", "(1,9): error: 'defined' cannot be a macro's name"},
        {"#undef __LINE__\n", "(1,8): error: '__LINE__' cannot be a macro's name"},
        {"#define F(a, a) a\n", "(1,14): error: parameter 'a' is named twice"},
        {"#define F(a b) a\n", "(1,13): error: expected ',' or ')' in the parameters of a macro"},
        {"#define F(a, ..., b) a\n",
         "(1,17): error: expected ',' or ')' in the parameters of a macro"},
        {"#define F(1) a\n", "(1,11): error: expected a parameter name, not '1'"},
        {"#define F(a\n", "(2,1): error: missing ')' in the parameters of a macro"},
        {"#define F(a) #b\n", "(1,14): error: '#' is not followed by a macro parameter"},
        {"#define F(a) ## a\n", "(1,14): error: '##' cannot be at either end of a macro"},
        {"#define A a ##\n", "(1,13): error: '##' cannot be at either end of a macro"},
        {"#define A __VA_ARGS__\n", "(1,11): error: __VA_ARGS__ is only for a variadic macro"},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        char expected[256] = {0};
        (void)tb_fmt_buffer(expected, sizeof(expected), "%s%s", MAIN_PATH, cases[index].expected);
        expect_error(cases[index].source, expected);
    }
}

TB_TEST(shader_preprocess, a_different_redefinition_is_a_warning)
{
    run out = {0};
    run_text(&out, "#define A 1\n#define A 2\nA\n");
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    TB_TEST_EXPECT_STRING(out.output.text, ==, "\n\n2\n");
    char message[256] = {0};
    first_diagnostic(&out, message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==, MAIN_PATH "(2,9): warning: 'A' is redefined");
    finish(&out);
}

/* -------------------------------------------------------------------------
 * Defines from the request.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_preprocess, defines_from_the_request_come_first)
{
    /* As Unreal's compilation environment sets them: a value, none (1), or empty. */
    const tb_shader_define defines[] = {{"SM6_PROFILE", NULL}, {"NUM_LIGHTS", "4"}, {"EMPTY", ""}};
    const sample_file file = {MAIN_PATH, "SM6_PROFILE NUM_LIGHTS [EMPTY]\n"
                                         "#if NUM_LIGHTS > 2\nmany\n#endif\n"};
    run out = {0};
    run_files(&out, &file, 1, defines, TB_TEST_COUNT_OF(defines), 0);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    TB_TEST_EXPECT_STRING(out.output.text, ==, "1 4 []\n\nmany\n");
    TB_TEST_REQUIRE_UINT(out.output.dependency_count, ==, 1);
    TB_TEST_EXPECT_STRING(out.output.dependencies[0], ==, MAIN_PATH);
    finish(&out);
}

/* -------------------------------------------------------------------------
 * Conditionals.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_preprocess, conditionals_choose_one_group)
{
    expect_text("#if 1\na\n#else\nb\n#endif\n", "\na\n");
    expect_text("#if 0\na\n#elif 1\nb\n#elif 1\nc\n#else\nd\n#endif\n", "\n\n\nb\n");
    expect_text("#if 0\na\n#elif 0\nb\n#else\nc\n#endif\n", "\n\n\n\n\nc\n");
    expect_text("#define A\n#ifdef A\nyes\n#endif\n#ifndef A\nno\n#endif\n", "\n\nyes\n");
    expect_text("#define A\n#if defined(A) && defined B\nboth\n#elif defined A\na\n#endif\n",
                "\n\n\n\na\n");
    /* Groups nest, and a skipped group skips the conditionals inside it. */
    expect_text("#if 0\n#if 1\na\n#else\nb\n#endif\n#else\nc\n#endif\n", "\n\n\n\n\n\n\nc\n");
    /* A skipped group need not be valid: its directives are not read. */
    expect_text("#if 0\n#bogus directive\n#include \"nowhere\"\n#endif\nx\n", "\n\n\n\nx\n");
}

TB_TEST(shader_preprocess, conditions_are_integer_arithmetic)
{
    static const char *const truths[] = {
        "1 + 2 * 3 == 7",
        "(1 + 2) * 3 == 9",
        "(1 << 62) > 0",
        "-1 < 0",
        "0x1F == 31",
        "0XfF == 255",
        "010 == 8",
        "0b101 == 5",
        "10u == 10",
        "10UL == 10",
        "!0",
        "~0 == -1",
        "7 / 2 == 3",
        "-7 / 2 == -3",
        "-7 % 2 == -1",
        "-8 >> 1 == -4",
        "(6 & 3) == 2 && (6 | 3) == 7 && (6 ^ 3) == 5",
        "1 ? 2 : 3 == 2",
        "0 ? 0 : 1",
        "2 >= 2 && 2 <= 2 && 3 != 2",
        "true",
        "!false",
        "UNDEFINED == 0",
        "9223372036854775807 > 0",
        "-9223372036854775807 - 1 < 0",
        /* Operands that are not evaluated cannot fail. */
        "1 || 1 / 0",
        "!(0 && 1 / 0)",
        "1 ? 1 : 1 / 0",
        "0 ? 1 / 0 : 1",
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(truths); index++)
    {
        char source[256] = {0};
        (void)tb_fmt_buffer(source, sizeof(source), "#if %s\nyes\n#else\nno\n#endif\n",
                            truths[index]);
        expect_text(source, "\nyes\n");
    }
}

TB_TEST(shader_preprocess, conditions_that_cannot_be_evaluated_are_errors)
{
    static const struct
    {
        const char *condition;
        const char *expected;
    } cases[] = {
        {"1 / 0", "(1,7): error: division by zero"},
        {"1 % 0", "(1,7): error: division by zero"},
        {"9223372036854775807 + 1", "(1,25): error: the addition overflows"},
        {"-9223372036854775807 - 2", "(1,26): error: the subtraction overflows"},
        {"4611686018427387904 * 2", "(1,25): error: the multiplication overflows"},
        {"-(-9223372036854775807 - 1)", "(1,5): error: the negation overflows"},
        {"(-9223372036854775807 - 1) / -1", "(1,32): error: the division overflows"},
        {"1 << 64", "(1,7): error: the shift count is not from 0 to 63"},
        {"1 >> -1", "(1,7): error: the shift count is not from 0 to 63"},
        {"-1 << 1", "(1,8): error: the left shift overflows"},
        {"1 << 63", "(1,7): error: the left shift overflows"},
        {"99999999999999999999", "(1,5): error: the number is too large for #if"},
        {"1.5", "(1,5): error: #if takes integers, not floating-point numbers"},
        {"1e3", "(1,5): error: #if takes integers, not floating-point numbers"},
        {"0x", "(1,5): error: the number is not an integer #if can read"},
        {"12abc", "(1,5): error: the number is not an integer #if can read"},
        {"(1", "(1,2): error: the expression ends too soon"},
        {"1 2", "(1,7): error: unexpected '2'"},
        {"1 +", "(1,2): error: the expression ends too soon"},
        {"1 ? 2", "(1,2): error: the expression ends too soon"},
        {"\"text\"", "(1,5): error: expected a value, not '\"text\"'"},
        {"defined", "(1,5): error: 'defined' needs a macro name"},
        {"defined(A", "(1,5): error: missing ')' after 'defined'"},
        {"", "(1,2): error: #if needs an expression"},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        char source[256] = {0};
        char expected[256] = {0};
        (void)tb_fmt_buffer(source, sizeof(source), "#if %s\n#endif\n", cases[index].condition);
        (void)tb_fmt_buffer(expected, sizeof(expected), "%s%s", MAIN_PATH, cases[index].expected);
        expect_error(source, expected);
    }
}

TB_TEST(shader_preprocess, conditionals_that_do_not_match_are_errors)
{
    expect_error("#else\n", MAIN_PATH "(1,2): error: #else without #if");
    expect_error("#elif 1\n", MAIN_PATH "(1,2): error: #elif without #if");
    expect_error("#endif\n", MAIN_PATH "(1,2): error: #endif without #if");
    expect_error("#if 1\n#else\n#else\n#endif\n", MAIN_PATH "(3,2): error: #else after #else");
    expect_error("#if 1\n#else\n#elif 1\n#endif\n", MAIN_PATH "(3,2): error: #elif after #else");
    expect_error("x\n#if 1\n",
                 MAIN_PATH "(2,2): error: unterminated conditional: #endif is missing");
    expect_error("#if 0\n", MAIN_PATH "(1,2): error: unterminated conditional: #endif is missing");
    expect_error("#ifdef\n#endif\n", MAIN_PATH "(1,2): error: #ifdef needs a macro name");
}

TB_TEST(shader_preprocess, a_conditional_belongs_to_one_file)
{
    /* An #if must end in the file that opens it, and an #endif cannot close
     * a conditional of the file that included it. */
    const sample_file opens[] = {
        {MAIN_PATH, "#include \"Open.hlsli\"\n#endif\n"},
        {"/Engine/Private/Open.hlsli", "#if 1\n"},
    };
    run out = {0};
    run_files(&out, opens, TB_TEST_COUNT_OF(opens), NULL, 0, 0);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.output.diagnostic_count, ==, 2);
    char message[256] = {0};
    (void)tb_shader_diagnostic_format(&out.output.diagnostics[0], message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==,
                          "/Engine/Private/Open.hlsli(1,2): error: unterminated conditional: "
                          "#endif is missing");
    (void)tb_shader_diagnostic_format(&out.output.diagnostics[1], message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==, MAIN_PATH "(2,2): error: #endif without #if");
    finish(&out);
}

/* -------------------------------------------------------------------------
 * Includes.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_preprocess, includes_name_virtual_paths_or_relative_ones)
{
    const sample_file files[] = {
        {MAIN_PATH, "#include \"/Engine/Public/Common.hlsli\"\n"
                    "#include \"Lighting.hlsli\"\n"
                    "#include \"../Public/Common.hlsli\"\n"
                    "main\n"},
        {"/Engine/Public/Common.hlsli", "#pragma once\ncommon\n"},
        {"/Engine/Private/Lighting.hlsli", "#include \"./Shadows/Shadows.hlsli\"\nlighting\n"},
        {"/Engine/Private/Shadows/Shadows.hlsli", "shadows\n"},
    };
    run out = {0};
    run_files(&out, files, TB_TEST_COUNT_OF(files), NULL, 0, 0);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    /* Common.hlsli has `#pragma once`, so the second include of it adds nothing. */
    TB_TEST_EXPECT_STRING(out.output.text, ==, "\ncommon\nshadows\nlighting\nmain\n");
    /* Every file read, each once, in the order first read. */
    static const char *const dependencies[] = {
        MAIN_PATH,
        "/Engine/Public/Common.hlsli",
        "/Engine/Private/Lighting.hlsli",
        "/Engine/Private/Shadows/Shadows.hlsli",
    };
    TB_TEST_REQUIRE_UINT(out.output.dependency_count, ==, TB_TEST_COUNT_OF(dependencies));
    for (size_t index = 0; index < TB_TEST_COUNT_OF(dependencies); index++)
    {
        TB_TEST_EXPECT_STRING(out.output.dependencies[index], ==, dependencies[index]);
    }
    finish(&out);
}

TB_TEST(shader_preprocess, include_guards_and_computed_includes_work)
{
    const sample_file files[] = {
        {MAIN_PATH, "#define COMMON \"/Engine/Public/Common.hlsli\"\n"
                    "#include COMMON\n#include COMMON\nmain\n"},
        {"/Engine/Public/Common.hlsli", "#ifndef COMMON_HLSLI\n#define COMMON_HLSLI\n"
                                        "common\n#endif\n"},
    };
    run out = {0};
    run_files(&out, files, TB_TEST_COUNT_OF(files), NULL, 0, 0);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    TB_TEST_EXPECT_STRING(out.output.text, ==, "\n\ncommon\nmain\n");
    TB_TEST_EXPECT_UINT(out.output.dependency_count, ==, 2);
    finish(&out);
}

TB_TEST(shader_preprocess, includes_that_cannot_be_read_are_errors)
{
    expect_error("\n#include \"/Engine/Missing.hlsli\"\n",
                 MAIN_PATH "(2,10): error: '/Engine/Missing.hlsli' was not found");
    expect_error("#include \"../../../Up.hlsli\"\n",
                 MAIN_PATH "(1,10): error: \"../../../Up.hlsli\" is not a valid shader path");
    expect_error("#include \"Common.txt\"\n",
                 MAIN_PATH "(1,10): error: \"Common.txt\" is not a valid shader path");
    expect_error("#include <Common.hlsli>\n",
                 MAIN_PATH "(1,10): error: shader includes name virtual paths in quotes: "
                           "#include \"/Engine/...\"");
    expect_error("#include\n", MAIN_PATH "(1,2): error: #include needs a path");
    expect_error("#include Common\n", MAIN_PATH "(1,2): error: #include needs a path in quotes");
}

TB_TEST(shader_preprocess, includes_nest_only_so_deep)
{
    /* A file that includes itself, with no guard, stops at the depth limit. */
    const sample_file files[] = {
        {MAIN_PATH, "#include \"Loop.hlsli\"\n"},
        {"/Engine/Private/Loop.hlsli", "#include \"Loop.hlsli\"\n"},
    };
    run out = {0};
    run_files(&out, files, TB_TEST_COUNT_OF(files), NULL, 0, 0);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    char message[256] = {0};
    first_diagnostic(&out, message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==,
                          "/Engine/Private/Loop.hlsli(1,10): error: includes nest deeper than 64 "
                          "files");
    finish(&out);
}

/* A loader of one file, which counts its calls. */
typedef struct counting_loader
{
    const char *path;
    const char *text;
    tb_shader_load_result failure;
    size_t calls;
} counting_loader;

static tb_shader_load_result load_counted(void *context, const char *path, size_t length,
                                          tb_arena *arena, const char **text, size_t *text_length)
{
    (void)arena;
    counting_loader *const loader = context;
    loader->calls++;
    TB_TEST_EXPECT_UINT(length, ==, tb_str_length(path));
    if (!tb_str_equal(path, loader->path))
    {
        return loader->failure;
    }
    *text = loader->text;
    *text_length = tb_str_length(loader->text);
    return TB_SHADER_LOAD_OK;
}

TB_TEST(shader_preprocess, files_in_the_request_win_over_the_loader)
{
    counting_loader context = {"/Engine/Public/Common.hlsli", "from the loader\n",
                               TB_SHADER_LOAD_NOT_FOUND, 0};
    const tb_shader_loader loader = {load_counted, &context};
    const tb_shader_file files[] = {
        {MAIN_PATH,
         "#include \"/Engine/Public/Common.hlsli\"\n#include "
         "\"/Engine/Generated/Material.hlsli\"\n",
         0},
        {"/Engine/Generated/Material.hlsli", "generated\n", 0},
    };
    tb_shader_file entries[2] = {files[0], files[1]};
    entries[0].length = tb_str_length(entries[0].text);
    entries[1].length = tb_str_length(entries[1].text);
    tb_shader_preprocess_request request = {0};
    request.path = MAIN_PATH;
    request.files = entries;
    request.file_count = 2;
    request.loader = &loader;
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_preprocessed output = {0};
    TB_TEST_EXPECT_UINT(tb_shader_preprocess(&request, arena, &output), ==, TB_SHADER_OK);
    TB_TEST_EXPECT_STRING(output.text, ==, "from the loader\ngenerated\n");
    /* Only Common.hlsli came from the loader. */
    TB_TEST_EXPECT_UINT(context.calls, ==, 1);
    tb_arena_destroy(arena);
}

TB_TEST(shader_preprocess, a_loader_that_fails_is_an_error)
{
    counting_loader context = {"/Engine/Other.hlsli", "", TB_SHADER_LOAD_FAILED, 0};
    const tb_shader_loader loader = {load_counted, &context};
    tb_shader_file file = {MAIN_PATH, "#include \"/Engine/Broken.hlsli\"\n", 0};
    file.length = tb_str_length(file.text);
    tb_shader_preprocess_request request = {0};
    request.path = MAIN_PATH;
    request.files = &file;
    request.file_count = 1;
    request.loader = &loader;
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_preprocessed output = {0};
    TB_TEST_EXPECT_UINT(tb_shader_preprocess(&request, arena, &output), ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(output.diagnostic_count, ==, 1);
    TB_TEST_EXPECT_STRING(output.diagnostics[0].message, ==,
                          "'/Engine/Broken.hlsli' could not be read");
    tb_arena_destroy(arena);
}

TB_TEST(shader_preprocess, the_vfs_loader_reads_mounted_shaders)
{
    /* The engine mounts its shaders at Engine, so /Engine/... is Engine/... in the VFS. */
    static const struct
    {
        const char *name;
        const char *text;
    } entries[] = {
        {"Engine/Private/Main.hlsl", "#include \"/Engine/Public/Common.hlsli\"\nmain\n"},
        {"Engine/Public/Common.hlsli", "common\n"},
        {"Engine/Public/Empty.hlsli", ""},
    };
    tb_stream buffer = {0};
    tb_stream_open_buffer(NULL, &buffer);
    tb_package_writer *writer = NULL;
    TB_TEST_REQUIRE_UINT(tb_package_writer_create(&buffer, NULL, NULL, &writer), ==, TB_PACKAGE_OK);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(entries); index++)
    {
        TB_TEST_REQUIRE_UINT(tb_package_add(writer, entries[index].name,
                                            tb_str_length(entries[index].name), entries[index].text,
                                            tb_str_length(entries[index].text), 6),
                             ==, TB_PACKAGE_OK);
    }
    TB_TEST_REQUIRE_UINT(tb_package_writer_finish(writer), ==, TB_PACKAGE_OK);
    tb_package_writer_destroy(writer);
    size_t length = 0;
    const unsigned char *const bytes = tb_stream_buffer_bytes(&buffer, &length);
    tb_vfs *const vfs = tb_vfs_create(NULL);
    TB_TEST_REQUIRE_UINT(tb_vfs_mount_archive_memory(vfs, "", 0, bytes, length, NULL, 0), ==,
                         TB_VFS_OK);
    const tb_shader_loader loader = tb_shader_vfs_loader(vfs);
    tb_arena *const arena = tb_arena_create(0);

    tb_shader_preprocess_request request = {0};
    request.path = MAIN_PATH;
    request.loader = &loader;
    tb_shader_preprocessed output = {0};
    TB_TEST_EXPECT_UINT(tb_shader_preprocess(&request, arena, &output), ==, TB_SHADER_OK);
    TB_TEST_EXPECT_STRING(output.text, ==, "common\nmain\n");

    /* An empty file reads as empty; a missing one, or a directory, is not found. */
    const char *text = NULL;
    size_t text_length = 1;
    TB_TEST_EXPECT_UINT(
        loader.load(loader.context, "/Engine/Public/Empty.hlsli", 26, arena, &text, &text_length),
        ==, TB_SHADER_LOAD_OK);
    TB_TEST_EXPECT_UINT(text_length, ==, 0);
    TB_TEST_EXPECT_UINT(
        loader.load(loader.context, "/Engine/Public/Missing.hlsli", 28, arena, &text, &text_length),
        ==, TB_SHADER_LOAD_NOT_FOUND);
    TB_TEST_EXPECT_UINT(
        loader.load(loader.context, "/Engine/Public", 14, arena, &text, &text_length), ==,
        TB_SHADER_LOAD_NOT_FOUND);

    tb_arena_destroy(arena);
    tb_vfs_destroy(vfs);
    (void)tb_stream_close(&buffer);
}

/* -------------------------------------------------------------------------
 * Other directives.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_preprocess, line_renames_and_renumbers_what_follows)
{
    expect_text("#line 100\n__LINE__\n", "\n100\n");
    expect_text("#line 7 \"/Game/Generated.hlsl\"\n__FILE__ __LINE__\n",
                "\n\n\n\n\n\n\"/Game/Generated.hlsl\" 7\n");
    expect_error("#line 50 \"/Game/Generated.hlsl\"\n\n#error here\n",
                 "/Game/Generated.hlsl(51,2): error: #error here");
    expect_error("#line 0\n", MAIN_PATH "(1,2): error: #line needs a line number from 1 to "
                                        "2147483647");
    expect_error("#line x\n", MAIN_PATH "(1,2): error: #line needs a line number from 1 to "
                                        "2147483647");
    expect_error("#line 5 name\n", MAIN_PATH "(1,9): error: #line names a file in quotes");
}

TB_TEST(shader_preprocess, line_markers_say_where_text_came_from)
{
    const sample_file files[] = {
        {MAIN_PATH, "#include \"Common.hlsli\"\nmain\n"},
        {"/Engine/Private/Common.hlsli", "common\n"},
    };
    run out = {0};
    run_files(&out, files, TB_TEST_COUNT_OF(files), NULL, 0, TB_SHADER_LINE_MARKERS);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    TB_TEST_EXPECT_STRING(out.output.text, ==,
                          "#line 1 \"/Engine/Private/Common.hlsli\"\n"
                          "common\n"
                          "#line 2 \"" MAIN_PATH "\"\n"
                          "main\n");
    finish(&out);
}

TB_TEST(shader_preprocess, pragmas_are_passed_on)
{
    /* `#pragma once` is the preprocessor's own; any other is the compiler's. */
    expect_text("#pragma pack_matrix(row_major)\nx\n", "#pragma pack_matrix(row_major)\nx\n");
    expect_text("x #define\n#pragma   warning( disable : 3571 )\ny\n",
                "x #define\n#pragma warning( disable : 3571 )\ny\n");
    expect_text("#pragma\n", "#pragma\n");
    expect_text("#pragma once\nx\n", "\nx\n");
}

TB_TEST(shader_preprocess, error_and_warning_directives_report_their_text)
{
    expect_error("#error   SM6 is required\n", MAIN_PATH "(1,2): error: #error SM6 is required");
    expect_error("#error\n", MAIN_PATH "(1,2): error: #error");
    run out = {0};
    run_text(&out, "#warning check this\nx\n");
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    TB_TEST_EXPECT_STRING(out.output.text, ==, "\nx\n");
    char message[256] = {0};
    first_diagnostic(&out, message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==, MAIN_PATH "(1,2): warning: #warning check this");
    finish(&out);
}

TB_TEST(shader_preprocess, unknown_directives_are_errors)
{
    expect_error("#bogus\n", MAIN_PATH "(1,2): error: unknown directive '#bogus'");
    expect_error("#1\n", MAIN_PATH "(1,2): error: '1' is not a directive");
    /* `#` alone is the null directive, and `#` in the middle of a line is a token. */
    expect_text("#\nx # y\n", "\nx # y\n");
    /* A `#` that a macro gives never starts a directive. */
    expect_text("#define HASH #\nHASH define X\nX\n", "\n# define X\nX\n");
}

/* -------------------------------------------------------------------------
 * Limits.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_preprocess, a_compile_stops_after_too_many_errors)
{
    char source[4096] = {0};
    size_t written = 0;
    for (size_t index = 0; index < TB_SHADER_MAX_ERRORS * 2; index++)
    {
        written += tb_fmt_buffer(source + written, sizeof(source) - written, "#bogus\n");
    }
    run out = {0};
    run_text(&out, source);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.output.diagnostic_count, ==, TB_SHADER_MAX_ERRORS + 1);
    TB_TEST_EXPECT_STRING(out.output.diagnostics[TB_SHADER_MAX_ERRORS].message, ==,
                          "too many errors; stopping");
    finish(&out);
}

TB_TEST(shader_preprocess, expansions_are_bounded)
{
    /* Each macro doubles the one before it: 2^30 tokens, unless stopped. */
    char source[4096] = {0};
    size_t written = tb_fmt_buffer(source, sizeof(source), "#define A0 x x\n");
    for (unsigned level = 1; level <= 30; level++)
    {
        written += tb_fmt_buffer(source + written, sizeof(source) - written,
                                 "#define A%u A%u A%u\n", level, level - 1, level - 1);
    }
    (void)tb_fmt_buffer(source + written, sizeof(source) - written, "A30\n");
    tb_shader_file file = {MAIN_PATH, source, tb_str_length(source)};
    tb_shader_preprocess_request request = {0};
    request.path = MAIN_PATH;
    request.files = &file;
    request.file_count = 1;
    request.max_tokens = 4096;
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_preprocessed output = {0};
    TB_TEST_EXPECT_UINT(tb_shader_preprocess(&request, arena, &output), ==, TB_SHADER_FAILED);
    char message[256] = {0};
    TB_TEST_REQUIRE_UINT(output.diagnostic_count, ==, 1);
    (void)tb_shader_diagnostic_format(&output.diagnostics[0], message, sizeof(message));
    TB_TEST_EXPECT_STRING(message, ==,
                          MAIN_PATH "(32,1): error: the shader expands to more than 4096 tokens");
    tb_arena_destroy(arena);
}

TB_TEST(shader_preprocess, nested_invocations_are_bounded)
{
    /* Arguments are expanded inside one another, so nesting is bounded too. */
    char source[8192] = {0};
    size_t written = tb_fmt_buffer(source, sizeof(source), "#define F(a) a\n");
    for (size_t level = 0; level < 300; level++)
    {
        written += tb_fmt_buffer(source + written, sizeof(source) - written, "F(");
    }
    written += tb_fmt_buffer(source + written, sizeof(source) - written, "x");
    for (size_t level = 0; level < 300; level++)
    {
        written += tb_fmt_buffer(source + written, sizeof(source) - written, ")");
    }
    (void)tb_fmt_buffer(source + written, sizeof(source) - written, "\n");
    run out = {0};
    run_text(&out, source);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    char message[256] = {0};
    first_diagnostic(&out, message, sizeof(message));
    TB_TEST_EXPECT(tb_str_find(message, 0, TB_STR_END, "macro invocations nest deeper than 256") !=
                   TB_STR_NOT_FOUND);
    finish(&out);
}

/* -------------------------------------------------------------------------
 * Output and names.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_preprocess, an_arena_that_is_full_is_no_memory)
{
    unsigned char memory[TB_ARENA_MINIMUM_SIZE] = {0};
    tb_arena *const arena = tb_arena_create_fixed(memory, sizeof(memory));
    tb_shader_file file = {MAIN_PATH, "", 0};
    char text[1024] = {0};
    memset(text, 'x', sizeof(text) - 2);
    text[sizeof(text) - 2] = '\n';
    file.text = text;
    file.length = sizeof(text) - 1;
    tb_shader_preprocess_request request = {0};
    request.path = MAIN_PATH;
    request.files = &file;
    request.file_count = 1;
    tb_shader_preprocessed output = {0};
    output.length = 5;
    TB_TEST_EXPECT_UINT(tb_shader_preprocess(&request, arena, &output), ==, TB_SHADER_NO_MEMORY);
    TB_TEST_EXPECT_POINTER(output.text, ==, NULL);
    TB_TEST_EXPECT_UINT(output.length, ==, 0);
    TB_TEST_EXPECT_UINT(output.diagnostic_count, ==, 0);
    tb_arena_destroy(arena);
}

TB_TEST(shader_preprocess, diagnostics_and_results_have_names)
{
    const tb_shader_diagnostic located = {TB_SHADER_WARNING, "/Game/A.hlsl", 3, 14, "careful"};
    const tb_shader_diagnostic unlocated = {TB_SHADER_ERROR, "", 0, 0, "no such file"};
    char buffer[128] = {0};
    TB_TEST_EXPECT_UINT(tb_shader_diagnostic_format(&located, buffer, sizeof(buffer)), ==,
                        tb_str_length("/Game/A.hlsl(3,14): warning: careful"));
    TB_TEST_EXPECT_STRING(buffer, ==, "/Game/A.hlsl(3,14): warning: careful");
    (void)tb_shader_diagnostic_format(&unlocated, buffer, sizeof(buffer));
    TB_TEST_EXPECT_STRING(buffer, ==, "error: no such file");
    /* A buffer too small holds what fits, and the length says what was needed. */
    char small[8] = {0};
    TB_TEST_EXPECT_UINT(tb_shader_diagnostic_format(&unlocated, small, sizeof(small)), ==, 19);
    TB_TEST_EXPECT_STRING(small, ==, "error: ");

    TB_TEST_EXPECT_STRING(tb_shader_result_name(TB_SHADER_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_shader_result_name(TB_SHADER_INVALID), ==, "invalid request");
    TB_TEST_EXPECT_STRING(tb_shader_result_name(TB_SHADER_FAILED), ==, "failed");
    TB_TEST_EXPECT_STRING(tb_shader_result_name(TB_SHADER_NO_MEMORY), ==, "no memory");
    TB_TEST_EXPECT_STRING(tb_shader_result_name((tb_shader_result)99), ==, "unknown result");
}
