/* What the engine's diagnostics cost a program that has them on: a record
 * sent to a logger, and an allocation made through the checking memory
 * functions. And what address space costs: reserving it, committing it, and
 * allocating from an arena that does both. */
#include "tb_bench.h"

#include "badger/arena.h"
#include "badger/log.h"
#include "badger/memory.h"
#include "badger/memory_debug.h"
#include "badger/virtual_memory.h"

#include <stddef.h>
#include <stdint.h>

/* --- logging -------------------------------------------------------------------- */

#define LOG_RECORDS ((size_t)500000)

typedef struct log_state
{
    tb_log *log;
    uint64_t bytes; /* Bytes of the records that reached the sink. */
    size_t records; /* Records sent in a run. */
} log_state;

/* A sink that looks at a record and keeps nothing: what is measured is the
 * logger, not a file or a terminal. */
static bool count_the_record(void *context, const tb_log_record *record)
{
    log_state *const state = context;

    state->bytes += record->length;
    return true;
}

static void run_log_write(void *context)
{
    log_state *const state = context;
    static const char message[] = "level loaded: 1,204 entities, 88 MiB of assets";

    for (size_t index = 0; index < state->records; index++)
    {
        (void)tb_log_write(state->log, TB_LOG_INFO, "assets", message, sizeof(message) - 1);
    }
    tb_bench_keep(state->bytes);
}

static void run_log_printf(void *context)
{
    log_state *const state = context;

    for (size_t index = 0; index < state->records; index++)
    {
        (void)tb_log_printf(state->log, TB_LOG_INFO, "assets",
                            "level %s loaded: %zu entities, %d MiB of assets", "burrow_01", index,
                            88);
    }
    tb_bench_keep(state->bytes);
}

static void run_log_filtered(void *context)
{
    log_state *const state = context;

    /* Below the least severity that the logger accepts: the common case of a
     * trace line in a build that is not tracing. */
    for (size_t index = 0; index < state->records; index++)
    {
        (void)tb_log_printf(state->log, TB_LOG_TRACE, "assets",
                            "level %s loaded: %zu entities, %d MiB of assets", "burrow_01", index,
                            88);
    }
    tb_bench_keep(state->bytes);
}

void tb_bench_log(tb_bench *bench)
{
    log_state state = {0};
    tb_log_config config = {.minimum = TB_LOG_INFO, .write_record = count_the_record};
    tb_bench_case cases[] = {
        {.name = "send a record of 46 bytes", .variant = "tb_log_write", .run = run_log_write},
        {.name = "format and send a record", .variant = "tb_log_printf", .run = run_log_printf},
        {.name = "a record that is filtered out",
         .variant = "tb_log_printf",
         .run = run_log_filtered},
    };

    if (!tb_bench_wants(bench, "log"))
    {
        return;
    }
    state.log = tb_log_create();
    config.context = &state;
    (void)tb_log_configure(state.log, &config);
    state.records = tb_bench_size(bench, LOG_RECORDS);
    for (size_t index = 0; index < 3; index++)
    {
        cases[index].context = &state;
        cases[index].operations = (double)state.records;
        tb_bench_measure(bench, "log", &cases[index], 1);
    }
    (void)tb_log_destroy(state.log);
}

/* --- virtual memory and arenas --------------------------------------------------- */

#define VM_REGION_SIZE ((size_t)64 << 20)
#define VM_ROUNDS ((size_t)2000)
#define ARENA_BLOCKS ((size_t)200000)
#define ARENA_BLOCK_SIZE ((size_t)64)

typedef struct vm_state
{
    tb_vm_region region; /* Reserved for the cases that commit. */
    size_t page_size;
    size_t rounds;
    tb_arena *arena;
    size_t blocks; /* Blocks allocated in a run. */
} vm_state;

static void run_reserve_and_release(void *context)
{
    const vm_state *const state = context;
    uint64_t total = 0;

    for (size_t round = 0; round < state->rounds; round++)
    {
        tb_vm_region region = {0};

        if (tb_vm_reserve(VM_REGION_SIZE, &region) == TB_VM_OK)
        {
            total += region.size;
            (void)tb_vm_release(&region);
        }
    }
    tb_bench_keep(total);
}

/* Commits a page, writes to it so that the system must really supply it, and
 * gives it back. */
static void run_commit_and_decommit(void *context)
{
    vm_state *const state = context;
    unsigned char *const bytes = state->region.address;
    uint64_t total = 0;

    for (size_t round = 0; round < state->rounds; round++)
    {
        const size_t offset = (round % 1024) * state->page_size;

        if (tb_vm_commit(&state->region, offset, state->page_size) == TB_VM_OK)
        {
            bytes[offset] = (unsigned char)round;
            total += bytes[offset];
            (void)tb_vm_decommit(&state->region, offset, state->page_size);
        }
    }
    tb_bench_keep(total);
}

static void reset_the_arena(void *context)
{
    vm_state *const state = context;

    tb_arena_reset(state->arena);
}

static void run_arena_blocks(void *context)
{
    vm_state *const state = context;
    uint64_t total = 0;

    for (size_t index = 0; index < state->blocks; index++)
    {
        unsigned char *const block = tb_arena_alloc(state->arena, ARENA_BLOCK_SIZE, 16);

        block[0] = (unsigned char)index;
        total += block[0];
    }
    tb_bench_keep(total);
}

void tb_bench_vm(tb_bench *bench)
{
    vm_state state = {0};
    vm_state reserved = {0};
    vm_state chunked = {0};
    tb_bench_case system[] = {
        {.name = "reserve 64 MiB of address space and release it",
         .variant = "tb_vm_reserve",
         .run = run_reserve_and_release},
        {.name = "commit a page, write to it, and give it back",
         .variant = "tb_vm_commit",
         .run = run_commit_and_decommit},
    };
    tb_bench_case arenas[] = {
        {.name = "allocate 64 bytes from an arena, 200,000 times after a reset",
         .variant = "an arena that reserves",
         .setup = reset_the_arena,
         .run = run_arena_blocks},
        {.name = "allocate 64 bytes from an arena, 200,000 times after a reset",
         .variant = "an arena of chunks",
         .setup = reset_the_arena,
         .run = run_arena_blocks},
    };

    if (!tb_bench_wants(bench, "vm"))
    {
        return;
    }
    state.page_size = tb_vm_get_info().page_size;
    state.rounds = tb_bench_size(bench, VM_ROUNDS);
    if (tb_vm_reserve(VM_REGION_SIZE, &state.region) == TB_VM_OK)
    {
        for (size_t index = 0; index < 2; index++)
        {
            system[index].context = &state;
            system[index].operations = (double)state.rounds;
            tb_bench_measure(bench, "vm", &system[index], 1);
        }
        (void)tb_vm_release(&state.region);
    }

    reserved.arena = tb_arena_create_reserved(VM_REGION_SIZE);
    chunked.arena = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
    reserved.blocks = tb_bench_size(bench, ARENA_BLOCKS);
    chunked.blocks = reserved.blocks;
    arenas[0].context = &reserved;
    arenas[1].context = &chunked;
    arenas[0].operations = (double)reserved.blocks;
    arenas[1].operations = (double)chunked.blocks;
    tb_bench_measure(bench, "vm", arenas, 2);
    tb_arena_destroy(reserved.arena);
    tb_arena_destroy(chunked.arena);
}

/* --- the checking memory functions ------------------------------------------------ */

#define CHECKED_BLOCKS ((size_t)200000)

typedef struct checked_state
{
    tb_memory_functions functions;
    size_t blocks;
} checked_state;

/* Allocates and frees a block of 64 bytes, as the engine's allocation
 * functions would ask it to. */
static void run_blocks(void *context)
{
    const checked_state *const state = context;
    uint64_t total = 0;

    for (size_t index = 0; index < state->blocks; index++)
    {
        unsigned char *const block = state->functions.malloc_function(64);

        block[0] = (unsigned char)index;
        total += block[0];
        state->functions.free_function(block);
    }
    tb_bench_keep(total);
}

void tb_bench_memory_debug(tb_bench *bench)
{
    checked_state plain = {0};
    checked_state checked = {0};
    tb_bench_case cases[] = {
        {.name = "allocate 64 bytes and free them",
         .variant = "the engine's memory functions",
         .run = run_blocks},
        {.name = "allocate 64 bytes and free them",
         .variant = "the checking memory functions",
         .run = run_blocks},
    };

    if (!tb_bench_wants(bench, "memory_debug"))
    {
        return;
    }
    tb_memory_get_original_functions(&plain.functions);
    tb_memory_debug_get_functions(&checked.functions);
    plain.blocks = tb_bench_size(bench, CHECKED_BLOCKS);
    checked.blocks = plain.blocks;
    cases[0].context = &plain;
    cases[1].context = &checked;
    cases[0].operations = (double)plain.blocks;
    cases[1].operations = (double)checked.blocks;
    tb_bench_measure(bench, "memory_debug", cases, 2);
    (void)tb_memory_debug_reset();
}
