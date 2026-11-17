/* UTF-8 and paths: the two modules that every name of a file passes through.
 *
 * The UTF-8 cases run on two texts of the same size, one of ASCII and one
 * that mixes characters of one to four bytes, because a validator has a fast
 * way through the first and not through the second. */
#include "tb_bench.h"

#include "badger/memory.h"
#include "badger/path.h"
#include "badger/str.h"
#include "badger/utf8.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UNITS_TEXT_SIZE ((size_t)65536)
#define UNITS_PATH_ROUNDS ((size_t)200000)

typedef struct units_text
{
    char *bytes;      /* UNITS_TEXT_SIZE bytes of valid UTF-8. */
    uint16_t *wide;   /* Room for it as UTF-16. */
    char *back;       /* Room for it as UTF-8 again. */
    size_t length;    /* Bytes of `bytes` that are used. */
    size_t wide_size; /* Units that `wide` has room for. */
    size_t rounds;    /* Times a run goes over the text. */
} units_text;

/* Fills the text with a repeated sample, whole samples only, so that it ends
 * at the end of a character. */
static void fill_text(units_text *text, const char *sample)
{
    const size_t sample_length = tb_str_length(sample);

    text->length = 0;
    while (text->length + sample_length <= UNITS_TEXT_SIZE)
    {
        memcpy(text->bytes + text->length, sample, sample_length);
        text->length += sample_length;
    }
}

static void run_validate(void *context)
{
    const units_text *const text = context;
    uint64_t valid = 0;

    for (size_t round = 0; round < text->rounds; round++)
    {
        size_t error = 0;

        valid += tb_utf8_validate(text->bytes, text->length, &error) ? 1u : 0u;
    }
    tb_bench_keep(valid);
}

static void run_count(void *context)
{
    const units_text *const text = context;
    uint64_t total = 0;

    for (size_t round = 0; round < text->rounds; round++)
    {
        size_t count = 0;

        (void)tb_utf8_count(text->bytes, text->length, &count);
        total += count;
    }
    tb_bench_keep(total);
}

static void run_to_utf16_and_back(void *context)
{
    const units_text *const text = context;
    uint64_t total = 0;

    for (size_t round = 0; round < text->rounds; round++)
    {
        size_t units = 0;
        size_t bytes = 0;

        (void)tb_utf8_to_utf16(text->bytes, text->length, text->wide, text->wide_size, &units);
        (void)tb_utf8_from_utf16(text->wide, units, text->back, UNITS_TEXT_SIZE, &bytes);
        total += bytes;
    }
    tb_bench_keep(total);
}

static void measure_text(tb_bench *bench, units_text *text, const char *variant)
{
    tb_bench_case cases[] = {
        {.name = "64 KiB of text: validate (per byte)", .run = run_validate},
        {.name = "64 KiB of text: count the characters (per byte)", .run = run_count},
        {.name = "64 KiB of text: to UTF-16 and back (per byte)", .run = run_to_utf16_and_back},
    };

    for (size_t index = 0; index < 3; index++)
    {
        cases[index].variant = variant;
        cases[index].context = text;
        cases[index].operations = (double)text->length * (double)text->rounds;
        tb_bench_measure(bench, "utf8", &cases[index], 1);
    }
}

void tb_bench_utf8(tb_bench *bench)
{
    units_text text = {0};

    if (!tb_bench_wants(bench, "utf8"))
    {
        return;
    }
    text.bytes = tb_malloc(UNITS_TEXT_SIZE);
    text.back = tb_malloc(UNITS_TEXT_SIZE);
    text.wide_size = UNITS_TEXT_SIZE;
    text.wide = tb_calloc(text.wide_size, sizeof(uint16_t));
    text.rounds = tb_bench_size(bench, 64);

    fill_text(&text, "assets/levels/burrow_01/geometry.mesh ");
    measure_text(bench, &text, "ASCII");
    /* One, two, three, and four bytes to a character, in turn. */
    fill_text(&text, "a\xC3\xA9\xE6\xBC\xA2\xF0\x9F\xA6\xA1 na\xC3\xAFve \xE2\x82\xAC ");
    measure_text(bench, &text, "1 to 4 bytes a character");

    tb_free(text.bytes);
    tb_free(text.back);
    tb_free(text.wide);
}

/* --- paths --------------------------------------------------------------------- */

static tb_path_view view_of(const char *text)
{
    return (tb_path_view){text, tb_str_length(text)};
}

typedef struct units_paths
{
    size_t rounds;
} units_paths;

static void run_normalize(void *context)
{
    const units_paths *const paths = context;
    const tb_path_view tangled = view_of("assets/./levels/../levels//burrow_01/geometry.mesh");
    uint64_t total = 0;

    for (size_t round = 0; round < paths->rounds; round++)
    {
        char normal[96] = {0};
        size_t required = 0;

        (void)tb_path_normalize(TB_PATH_POSIX, tangled, normal, sizeof(normal), &required);
        total += required + (unsigned char)normal[7];
    }
    tb_bench_keep(total);
}

static void run_join(void *context)
{
    const units_paths *const paths = context;
    const tb_path_view root = view_of("C:\\Users\\player\\AppData\\Local\\burrow");
    const tb_path_view name = view_of("saves/slot_3/world.sav");
    uint64_t total = 0;

    for (size_t round = 0; round < paths->rounds; round++)
    {
        char joined[128] = {0};
        size_t required = 0;

        (void)tb_path_join(TB_PATH_WINDOWS, root, name, joined, sizeof(joined), &required);
        total += required + (unsigned char)joined[3];
    }
    tb_bench_keep(total);
}

static void run_parse(void *context)
{
    const units_paths *const paths = context;
    const tb_path_view path = view_of("/home/player/.local/share/burrow/saves/world.sav.bak");
    uint64_t total = 0;

    for (size_t round = 0; round < paths->rounds; round++)
    {
        tb_path_parts parts = {0};

        (void)tb_path_parse(TB_PATH_POSIX, path, &parts);
        total += parts.name.length + parts.extension.length;
    }
    tb_bench_keep(total);
}

void tb_bench_path(tb_bench *bench)
{
    units_paths paths = {0};
    tb_bench_case cases[] = {
        {.name = "normalize a path of 50 bytes with dots in it", .run = run_normalize},
        {.name = "join a directory and a name, 60 bytes in all", .run = run_join},
        {.name = "split a path of 52 bytes into its parts", .run = run_parse},
    };

    if (!tb_bench_wants(bench, "path"))
    {
        return;
    }
    paths.rounds = tb_bench_size(bench, UNITS_PATH_ROUNDS);
    for (size_t index = 0; index < 3; index++)
    {
        cases[index].variant = "badger";
        cases[index].context = &paths;
        cases[index].operations = (double)paths.rounds;
        tb_bench_measure(bench, "path", &cases[index], 1);
    }
}
