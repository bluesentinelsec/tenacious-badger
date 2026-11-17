/* Allocation: a small object from an arena, and a block allocated and freed
 * through the engine's heap, through the allocator interface, and by the C
 * library that the heap is built on, on one thread and on eight at once. */
#include "tb_bench.h"

#include "badger/allocator.h"
#include "badger/arena.h"
#include "badger/memory.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MEMORY_ALLOCATIONS ((size_t)2000000)

/* Threads that allocate at the same time, and what each of them allocates. */
#define MEMORY_THREADS ((size_t)8)
#define MEMORY_ALLOCATIONS_PER_THREAD ((size_t)500000)
#define MEMORY_OBJECT_SIZE ((size_t)48)
#define MEMORY_BLOCK_SIZE ((size_t)64)
#define MEMORY_ALIGNMENT ((size_t)8)
#define MEMORY_ARENA_CHUNK_SIZE ((size_t)1 << 20)

/* The arena is reset after this many objects, before it would need another
 * chunk, as an arena for one frame is reset every frame. */
#define MEMORY_ARENA_RESET_INTERVAL ((size_t)8192)

typedef struct memory_state
{
    size_t allocations;
} memory_state;

static void run_arena(void *context)
{
    const memory_state *const state = context;
    tb_arena *const arena = tb_arena_create(MEMORY_ARENA_CHUNK_SIZE);
    uint64_t sum = 0;

    for (size_t count = 0; count < state->allocations; count++)
    {
        sum += (uintptr_t)tb_arena_alloc(arena, MEMORY_OBJECT_SIZE, MEMORY_ALIGNMENT) & 0xFFu;
        if (count % MEMORY_ARENA_RESET_INTERVAL == MEMORY_ARENA_RESET_INTERVAL - 1)
        {
            tb_arena_reset(arena);
        }
    }
    tb_bench_keep(sum);
    tb_arena_destroy(arena);
}

static void run_heap(void *context)
{
    const memory_state *const state = context;
    uint64_t sum = 0;

    for (size_t count = 0; count < state->allocations; count++)
    {
        void *const block = tb_malloc(MEMORY_BLOCK_SIZE);

        sum += (uintptr_t)block & 0xFFu;
        tb_free(block);
    }
    tb_bench_keep(sum);
}

/* The C library alone: what the engine's heap adds is the difference. */
static void run_c_library(void *context)
{
    const memory_state *const state = context;
    uint64_t sum = 0;

    for (size_t count = 0; count < state->allocations; count++)
    {
        void *const block = malloc(MEMORY_BLOCK_SIZE);

        sum += (uintptr_t)block & 0xFFu;
        free(block);
    }
    tb_bench_keep(sum);
}

static void run_allocator(void *context)
{
    const memory_state *const state = context;
    uint64_t sum = 0;

    for (size_t count = 0; count < state->allocations; count++)
    {
        void *const block = tb_allocator_alloc(NULL, MEMORY_BLOCK_SIZE, MEMORY_ALIGNMENT);

        sum += (uintptr_t)block & 0xFFu;
        tb_allocator_free(NULL, block, MEMORY_BLOCK_SIZE, MEMORY_ALIGNMENT);
    }
    tb_bench_keep(sum);
}

void tb_bench_memory(tb_bench *bench)
{
    memory_state state = {0};
    memory_state shared = {0};
    tb_bench_case arena = {
        .name = "allocate 48 bytes from an arena", .variant = "badger", .run = run_arena};
    tb_bench_case heap[3] = {
        {.name = "allocate and free 64 bytes", .variant = "tb_malloc", .run = run_heap},
        {.name = "allocate and free 64 bytes",
         .variant = "tb_allocator_alloc",
         .run = run_allocator},
        {.name = "allocate and free 64 bytes", .variant = "malloc", .run = run_c_library},
    };
    /* Every thread allocates and frees its own blocks, so the threads share
     * nothing but the heap and the engine's count of live blocks. */
    tb_bench_case together[2] = {
        {.name = "allocate and free 64 bytes on 8 threads at once",
         .variant = "tb_malloc",
         .run = run_heap,
         .threads = MEMORY_THREADS},
        {.name = "allocate and free 64 bytes on 8 threads at once",
         .variant = "malloc",
         .run = run_c_library,
         .threads = MEMORY_THREADS},
    };

    state.allocations = tb_bench_size(bench, MEMORY_ALLOCATIONS);
    shared.allocations = tb_bench_size(bench, MEMORY_ALLOCATIONS_PER_THREAD);
    arena.operations = (double)state.allocations;
    arena.context = &state;
    for (size_t index = 0; index < 3; index++)
    {
        heap[index].operations = (double)state.allocations;
        heap[index].context = &state;
    }
    for (size_t index = 0; index < 2; index++)
    {
        together[index].operations = (double)shared.allocations;
        together[index].context = &shared;
    }
    tb_bench_measure(bench, "memory", &arena, 1);
    tb_bench_measure(bench, "memory", heap, 3);
    tb_bench_measure(bench, "memory", together, 2);
}
