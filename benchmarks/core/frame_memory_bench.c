/* Frame memory: the small, short-lived allocations of a frame, from frame
 * memory beside the heap, which has to free each one. A frame is 1000
 * allocations of 64 bytes; frame memory releases them all by beginning the
 * next frame. Times are per allocation, its share of the frame's release
 * included. */
#include "tb_bench.h"

#include "badger/frame_memory.h"
#include "badger/memory.h"

#include <stdint.h>

#define FRAME_ALLOCATIONS ((size_t)1000)
#define FRAME_SIZE ((size_t)64)
#define FRAME_COUNT ((size_t)2000)

typedef struct frame_state
{
    tb_frame_memory *memory;
    void **blocks; /* The heap's allocations, kept to free at the frame's end. */
    size_t frames;
} frame_state;

static void run_frame_memory(void *context)
{
    frame_state *const state = context;
    uint64_t sum = 0;

    for (size_t frame = 0; frame < state->frames; frame++)
    {
        for (size_t index = 0; index < FRAME_ALLOCATIONS; index++)
        {
            sum += (uintptr_t)tb_frame_memory_alloc(state->memory, FRAME_SIZE, 8) & 0xFFu;
        }
        tb_frame_memory_begin(state->memory);
    }
    tb_bench_keep(sum);
}

static void run_heap(void *context)
{
    frame_state *const state = context;
    uint64_t sum = 0;

    for (size_t frame = 0; frame < state->frames; frame++)
    {
        for (size_t index = 0; index < FRAME_ALLOCATIONS; index++)
        {
            state->blocks[index] = tb_malloc(FRAME_SIZE);
            sum += (uintptr_t)state->blocks[index] & 0xFFu;
        }
        for (size_t index = 0; index < FRAME_ALLOCATIONS; index++)
        {
            tb_free(state->blocks[index]);
        }
    }
    tb_bench_keep(sum);
}

void tb_bench_frame_memory(tb_bench *bench)
{
    frame_state state = {.frames = tb_bench_size(bench, FRAME_COUNT)};
    const char *const name = "allocate 64 bytes, released at the end of the frame";
    tb_bench_case cases[2] = {
        {.name = name, .variant = "tb_frame_memory_alloc", .run = run_frame_memory},
        {.name = name, .variant = "tb_malloc and tb_free", .run = run_heap},
    };

    if (!tb_bench_wants(bench, "frame_memory"))
    {
        return;
    }
    state.memory = tb_frame_memory_create(NULL);
    state.blocks = tb_malloc(FRAME_ALLOCATIONS * sizeof(void *));
    for (size_t index = 0; index < 2; index++)
    {
        cases[index].operations = (double)state.frames * (double)FRAME_ALLOCATIONS;
        cases[index].context = &state;
    }
    tb_bench_measure(bench, "frame_memory", cases, 2);
    tb_free(state.blocks);
    tb_frame_memory_destroy(state.memory);
}
