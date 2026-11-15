/* Searching text: for a pattern, for a byte from the end, for any byte of a
 * set, and trimming a short line. The searches are the same in badger/str.h,
 * which shares their implementation. The C library's functions search the
 * same text beside each for comparison. */
#include "tb_bench.h"

#include "badger/memory.h"
#include "badger/text.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEXT_SENTENCES ((size_t)512)
#define TEXT_SEARCHES ((size_t)20000)
#define TEXT_NEAR_SEARCHES ((size_t)2000)
#define TEXT_NEAR_RUN ((size_t)32)
#define TEXT_NEAR_RUNS ((size_t)720)
#define TEXT_TRIMS ((size_t)1000000)

typedef struct text_state
{
    char *prose; /* Sentences, then a phrase that appears nowhere else. */
    size_t prose_length;
    char *almost; /* Runs of one letter, each a byte short of the pattern. */
    size_t almost_length;
    char almost_pattern[TEXT_NEAR_RUN + 1];
    size_t searches;
    size_t almost_searches;
    size_t trims;
} text_state;

static const char text_line[] = "   r_fov = 90   ";

/* ---------------------------------------------------------------------------
 * A pattern.
 * ------------------------------------------------------------------------- */

static void find_pattern(const text_state *state, const char *pattern)
{
    const tb_text prose = tb_text_make(state->prose, state->prose_length);
    const tb_text wanted = tb_text_from_cstr(pattern);

    for (size_t search = 0; search < state->searches; search++)
    {
        tb_bench_keep(tb_text_find(prose, 0, TB_TEXT_END, wanted));
    }
}

static void find_pattern_strstr(const text_state *state, const char *pattern)
{
    for (size_t search = 0; search < state->searches; search++)
    {
        tb_bench_keep((uint64_t)(strstr(state->prose, pattern) - state->prose));
    }
}

/* The first byte of this pattern is rare in the text. */
static void run_find_rare(void *context)
{
    find_pattern(context, "needle in");
}

static void run_find_rare_strstr(void *context)
{
    find_pattern_strstr(context, "needle in");
}

/* The first byte of this pattern is the commonest byte of the text. */
static void run_find_common(void *context)
{
    find_pattern(context, " in a hay");
}

static void run_find_common_strstr(void *context)
{
    find_pattern_strstr(context, " in a hay");
}

/* Every place in this text is nearly a match, and none is one. */
static void run_find_almost(void *context)
{
    const text_state *const state = context;
    const tb_text almost = tb_text_make(state->almost, state->almost_length);
    const tb_text wanted = tb_text_make(state->almost_pattern, TEXT_NEAR_RUN);

    for (size_t search = 0; search < state->almost_searches; search++)
    {
        tb_bench_keep(tb_text_find(almost, 0, TB_TEXT_END, wanted) == TB_TEXT_NOT_FOUND ? 0u : 1u);
    }
}

static void run_find_almost_strstr(void *context)
{
    const text_state *const state = context;

    for (size_t search = 0; search < state->almost_searches; search++)
    {
        tb_bench_keep(strstr(state->almost, state->almost_pattern) == NULL ? 0u : 1u);
    }
}

/* ---------------------------------------------------------------------------
 * Bytes.
 * ------------------------------------------------------------------------- */

static void run_rfind_byte(void *context)
{
    const text_state *const state = context;
    const tb_text prose = tb_text_make(state->prose, state->prose_length);

    for (size_t search = 0; search < state->searches; search++)
    {
        tb_bench_keep(tb_text_rfind_char(prose, 0, TB_TEXT_END, '#') == TB_TEXT_NOT_FOUND ? 0u
                                                                                          : 1u);
    }
}

static void run_rfind_byte_strrchr(void *context)
{
    const text_state *const state = context;

    for (size_t search = 0; search < state->searches; search++)
    {
        tb_bench_keep(strrchr(state->prose, '#') == NULL ? 0u : 1u);
    }
}

static void run_find_any(void *context)
{
    const text_state *const state = context;
    const tb_text prose = tb_text_make(state->prose, state->prose_length);
    const tb_text line_ends = tb_text_from_cstr("\r\n");

    for (size_t search = 0; search < state->searches; search++)
    {
        tb_bench_keep(tb_text_find_any(prose, 0, TB_TEXT_END, line_ends) == TB_TEXT_NOT_FOUND ? 0u
                                                                                              : 1u);
    }
}

static void run_find_any_strcspn(void *context)
{
    const text_state *const state = context;

    for (size_t search = 0; search < state->searches; search++)
    {
        tb_bench_keep(strcspn(state->prose, "\r\n"));
    }
}

/* What a parser of settings or console input does to every line. */
static void run_trim(void *context)
{
    const text_state *const state = context;
    const tb_text line = tb_text_make(text_line, sizeof(text_line) - 1);
    const tb_text blanks = tb_text_from_cstr(" \t\r\n");

    for (size_t trim = 0; trim < state->trims; trim++)
    {
        tb_bench_keep(tb_text_trim(line, blanks).length);
    }
}

/* ---------------------------------------------------------------------------
 * The texts.
 * ------------------------------------------------------------------------- */

static void make_texts(const tb_bench *bench, text_state *state)
{
    static const char sentence[] = "the quick brown fox jumps over the lazy dog; ";
    static const char ending[] = "needle in a haystack";
    const size_t sentence_length = sizeof(sentence) - 1;
    const size_t sentences = tb_bench_size(bench, TEXT_SENTENCES);
    const size_t almost_runs = tb_bench_size(bench, TEXT_NEAR_RUNS);

    state->prose_length = sentences * sentence_length + sizeof(ending) - 1;
    state->prose = tb_malloc(state->prose_length + 1);
    for (size_t copy = 0; copy < sentences; copy++)
    {
        memcpy(state->prose + copy * sentence_length, sentence, sentence_length);
    }
    memcpy(state->prose + sentences * sentence_length, ending, sizeof(ending));

    state->almost_length = almost_runs * TEXT_NEAR_RUN;
    state->almost = tb_malloc(state->almost_length + 1);
    memset(state->almost, 'a', state->almost_length);
    for (size_t run = 0; run < almost_runs; run++)
    {
        state->almost[run * TEXT_NEAR_RUN + TEXT_NEAR_RUN - 1] = 'b';
    }
    state->almost[state->almost_length] = '\0';
    memset(state->almost_pattern, 'a', TEXT_NEAR_RUN);
    state->almost_pattern[TEXT_NEAR_RUN] = '\0';

    state->searches = tb_bench_size(bench, TEXT_SEARCHES);
    state->almost_searches = tb_bench_size(bench, TEXT_NEAR_SEARCHES);
    state->trims = tb_bench_size(bench, TEXT_TRIMS);
}

/* Measures an engine search beside the C library's. */
static void measure_pair(tb_bench *bench, text_state *state, const char *name, double operations,
                         tb_bench_function *run, const char *library_name,
                         tb_bench_function *library_run)
{
    const tb_bench_case cases[2] = {
        {.name = name, .variant = "badger", .operations = operations, .run = run, .context = state},
        {.name = name,
         .variant = library_name,
         .operations = operations,
         .run = library_run,
         .context = state},
    };

    tb_bench_measure(bench, "text", cases, 2);
}

void tb_bench_text(tb_bench *bench)
{
    text_state state = {0};
    tb_bench_case trim = {
        .name = "trim blanks from a 16-byte line", .variant = "badger", .run = run_trim};

    if (!tb_bench_wants(bench, "text"))
    {
        return;
    }
    make_texts(bench, &state);
    measure_pair(bench, &state, "find a pattern at the end of 23 KB, first byte rare",
                 (double)state.searches, run_find_rare, "strstr", run_find_rare_strstr);
    measure_pair(bench, &state, "find a pattern at the end of 23 KB, first byte common",
                 (double)state.searches, run_find_common, "strstr", run_find_common_strstr);
    measure_pair(bench, &state, "look for a pattern in 23 KB of near misses",
                 (double)state.almost_searches, run_find_almost, "strstr", run_find_almost_strstr);
    measure_pair(bench, &state, "look backward for a byte that 23 KB do not hold",
                 (double)state.searches, run_rfind_byte, "strrchr", run_rfind_byte_strrchr);
    measure_pair(bench, &state, "look for either of 2 bytes that 23 KB do not hold",
                 (double)state.searches, run_find_any, "strcspn", run_find_any_strcspn);
    trim.operations = (double)state.trims;
    trim.context = &state;
    tb_bench_measure(bench, "text", &trim, 1);
    tb_free(state.prose);
    tb_free(state.almost);
}
