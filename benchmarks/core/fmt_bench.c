/* Formatting: integers of every length, a line of log output, padding, and a
 * line formatted into a new string. The C library's snprintf formats the same
 * text beside each for comparison. */
#include "tb_bench.h"

#include "badger/fmt.h"
#include "badger/str.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define FMT_LINES ((size_t)1000000)
#define FMT_VALUE_COUNT ((size_t)1024)
#define FMT_BUFFER_SIZE ((size_t)128)

typedef struct fmt_state
{
    size_t lines;
    long long values[FMT_VALUE_COUNT]; /* From one to nineteen digits, evenly. */
} fmt_state;

static void run_integer(void *context)
{
    const fmt_state *const state = context;
    char buffer[FMT_BUFFER_SIZE] = {0};

    for (size_t line = 0; line < state->lines; line++)
    {
        tb_bench_keep(
            tb_fmt_buffer(buffer, sizeof(buffer), "%lld", state->values[line % FMT_VALUE_COUNT]));
    }
}

static void run_integer_snprintf(void *context)
{
    const fmt_state *const state = context;
    char buffer[FMT_BUFFER_SIZE] = {0};

    for (size_t line = 0; line < state->lines; line++)
    {
        tb_bench_keep((uint64_t)snprintf(buffer, sizeof(buffer), "%lld",
                                         state->values[line % FMT_VALUE_COUNT]));
    }
}

static void run_log_line(void *context)
{
    const fmt_state *const state = context;
    char buffer[FMT_BUFFER_SIZE] = {0};

    for (size_t line = 0; line < state->lines; line++)
    {
        tb_bench_keep(tb_fmt_buffer(buffer, sizeof(buffer),
                                    "[%s] frame %d: %d draws, %d triangles in %s", "render",
                                    (int)line, 1843, 1204331, "main_pass"));
    }
}

static void run_log_line_snprintf(void *context)
{
    const fmt_state *const state = context;
    char buffer[FMT_BUFFER_SIZE] = {0};

    for (size_t line = 0; line < state->lines; line++)
    {
        tb_bench_keep((uint64_t)snprintf(buffer, sizeof(buffer),
                                         "[%s] frame %d: %d draws, %d triangles in %s", "render",
                                         (int)line, 1843, 1204331, "main_pass"));
    }
}

static void run_padded(void *context)
{
    const fmt_state *const state = context;
    char buffer[FMT_BUFFER_SIZE] = {0};

    for (size_t line = 0; line < state->lines; line++)
    {
        tb_bench_keep(tb_fmt_buffer(buffer, sizeof(buffer), "%08x %-12s|",
                                    (unsigned)line * 2654435761u, "entity"));
    }
}

static void run_padded_snprintf(void *context)
{
    const fmt_state *const state = context;
    char buffer[FMT_BUFFER_SIZE] = {0};

    for (size_t line = 0; line < state->lines; line++)
    {
        tb_bench_keep((uint64_t)snprintf(buffer, sizeof(buffer), "%08x %-12s|",
                                         (unsigned)line * 2654435761u, "entity"));
    }
}

/* The line as a string of its own, allocated and released each time. */
static void run_new_string(void *context)
{
    const fmt_state *const state = context;

    for (size_t line = 0; line < state->lines; line++)
    {
        char *const text = tb_fmt_string(NULL, "[%s] frame %d: %d draws, %d triangles in %s",
                                         "render", (int)line, 1843, 1204331, "main_pass");

        tb_bench_keep((unsigned char)text[1]);
        tb_str_free(NULL, text);
    }
}

void tb_bench_fmt(tb_bench *bench)
{
    static fmt_state state = {0};
    uint64_t sequence = 45;
    double lines = 0;
    tb_bench_case integers[2] = {
        {.name = "%lld, 1 to 19 digits", .variant = "badger", .run = run_integer},
        {.name = "%lld, 1 to 19 digits", .variant = "snprintf", .run = run_integer_snprintf},
    };
    tb_bench_case log_lines[2] = {
        {.name = "a log line: 2 strings, 3 ints", .variant = "badger", .run = run_log_line},
        {.name = "a log line: 2 strings, 3 ints",
         .variant = "snprintf",
         .run = run_log_line_snprintf},
    };
    tb_bench_case padded[2] = {
        {.name = "%08x and %-12s", .variant = "badger", .run = run_padded},
        {.name = "%08x and %-12s", .variant = "snprintf", .run = run_padded_snprintf},
    };
    tb_bench_case new_string = {
        .name = "a log line as a new string", .variant = "badger", .run = run_new_string};

    if (!tb_bench_wants(bench, "fmt"))
    {
        return;
    }
    state.lines = tb_bench_size(bench, FMT_LINES);
    for (size_t index = 0; index < FMT_VALUE_COUNT; index++)
    {
        const uint64_t value = tb_bench_random(&sequence);
        const unsigned shift = (unsigned)(tb_bench_random(&sequence) % 63);

        /* Shifting by an evenly spread amount spreads the number of digits. */
        state.values[index] = (long long)((value >> shift) / 2);
    }
    lines = (double)state.lines;
    for (size_t index = 0; index < 2; index++)
    {
        integers[index].operations = lines;
        integers[index].context = &state;
        log_lines[index].operations = lines;
        log_lines[index].context = &state;
        padded[index].operations = lines;
        padded[index].context = &state;
    }
    new_string.operations = lines;
    new_string.context = &state;
    tb_bench_measure(bench, "fmt", integers, 2);
    tb_bench_measure(bench, "fmt", log_lines, 2);
    tb_bench_measure(bench, "fmt", padded, 2);
    tb_bench_measure(bench, "fmt", &new_string, 1);
}
