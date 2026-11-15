/* Allocation: a small object from an arena, and a block allocated and freed
 * through the engine's heap and through the allocator interface. */
#include "tb_bench.h"

#include "badger/allocator.h"
#include "badger/arena.h"
#include "badger/memory.h"

#include <stddef.h>
#include <stdint.h>

#define MEMORY_ALLOCATIONS ((size_t)2000000)
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
    tb_bench_case arena = {
        .name = "allocate 48 bytes from an arena", .variant = "badger", .run = run_arena};
    tb_bench_case heap[2] = {
        {.name = "allocate and free 64 bytes", .variant = "tb_malloc", .run = run_heap},
        {.name = "allocate and free 64 bytes",
         .variant = "tb_allocator_alloc",
         .run = run_allocator},
    };

    state.allocations = tb_bench_size(bench, MEMORY_ALLOCATIONS);
    arena.operations = (double)state.allocations;
    arena.context = &state;
    for (size_t index = 0; index < 2; index++)
    {
        heap[index].operations = (double)state.allocations;
        heap[index].context = &state;
    }
    tb_bench_measure(bench, "memory", &arena, 1);
    tb_bench_measure(bench, "memory", heap, 2);
}
