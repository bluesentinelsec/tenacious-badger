/* JSON reading of a document like an asset manifest: an array of objects with
 * strings, numbers, booleans, and a nested array each. Operations are bytes
 * of text, so the numbers are nanoseconds per byte read. */
#include "tb_bench.h"

#include "badger/arena.h"
#include "badger/fmt.h"
#include "badger/json.h"
#include "badger/memory.h"

#include <stddef.h>
#include <stdint.h>

#define JSON_BENCH_ENTRIES ((size_t)5000)

typedef struct json_state
{
    char *text;
    size_t length;
    tb_arena *arena;
} json_state;

static void run_reader(void *context)
{
    const json_state *const state = context;
    tb_json_reader *const reader = tb_json_reader_create(state->text, state->length, NULL, NULL);
    uint64_t events = 0;
    for (;;)
    {
        tb_json_token token = {0};
        if (tb_json_reader_next(reader, &token) != TB_JSON_OK || token.event == TB_JSON_EVENT_END)
        {
            break;
        }
        events++;
    }
    tb_json_reader_destroy(reader);
    tb_bench_keep(events);
}

static void run_parse(void *context)
{
    const json_state *const state = context;
    const tb_json_value *root = NULL;
    tb_arena_reset(state->arena);
    (void)tb_json_parse(state->text, state->length, NULL, state->arena, &root, NULL);
    tb_bench_keep(root == NULL ? 0 : tb_json_count(root));
}

void tb_bench_json(tb_bench *bench)
{
    json_state state = {0};
    size_t entries = 0;
    size_t capacity = 0;
    tb_bench_case cases[2] = {
        {.name = "read an asset manifest", .variant = "tb_json_reader_next", .run = run_reader},
        {.name = "read an asset manifest", .variant = "tb_json_parse", .run = run_parse},
    };
    if (!tb_bench_wants(bench, "json"))
    {
        return;
    }
    entries = tb_bench_size(bench, JSON_BENCH_ENTRIES);
    capacity = entries * 160 + 2;
    state.text = tb_malloc(capacity);
    state.text[state.length++] = '[';
    for (size_t index = 0; index < entries; index++)
    {
        state.length += tb_fmt_buffer(
            state.text + state.length, capacity - state.length,
            "%s{\"path\": \"textures/rock_%zu.tex\", \"size\": %zu, \"scale\": %zu.25,"
            " \"streamed\": %s, \"mips\": [%zu, %zu, %zu]}",
            index == 0 ? "" : ",\n", index, index * 4096, index % 7,
            index % 2 == 0 ? "true" : "false", index, index / 2, index / 4);
    }
    state.text[state.length++] = ']';
    state.arena = tb_arena_create(0);
    for (size_t index = 0; index < 2; index++)
    {
        cases[index].context = &state;
        cases[index].operations = (double)state.length;
    }
    tb_bench_measure(bench, "json", cases, 2);
    tb_arena_destroy(state.arena);
    tb_free(state.text);
}
