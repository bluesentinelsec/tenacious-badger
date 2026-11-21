/* Frame memory keeps a lane for every thread that has asked it for memory.
 * A lane holds the thread's arena for each frame in flight, created when the
 * thread first asks during that frame's turn. The thread finds its lane
 * through a thread-local place that belongs to this frame memory, so asking
 * for memory takes no lock once the lane exists. The lanes are also kept in
 * a list, under a lock, for tb_frame_memory_begin() to reset and
 * tb_frame_memory_destroy() to release: the only times anything other than
 * a lane's own thread touches it. */
#include "badger/frame_memory.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/thread.h"

#include <stdbool.h>

typedef struct tb_frame_memory_lane
{
    tb_arena *arenas[TB_FRAME_MEMORY_MOST_FRAMES]; /* NULL until first used. */
    struct tb_frame_memory_lane *next;
} tb_frame_memory_lane;

struct tb_frame_memory
{
    size_t frames_in_flight;
    size_t chunk_size;
    size_t current; /* Which of each lane's arenas the current frame uses. */
    uint64_t frame;
    tb_thread_local lane_of_thread;
    tb_mutex lanes_lock;
    tb_frame_memory_lane *lanes;
};

tb_frame_memory *tb_frame_memory_create(const tb_frame_memory_options *options)
{
    const tb_frame_memory_options defaults = {0};
    tb_frame_memory *memory = NULL;

    if (options == NULL)
    {
        options = &defaults;
    }
    TB_ASSERT_MSG(options->frames_in_flight <= TB_FRAME_MEMORY_MOST_FRAMES,
                  "frames_in_flight is at most TB_FRAME_MEMORY_MOST_FRAMES");
    memory = TB_NEW(tb_frame_memory);
    memory->frames_in_flight = options->frames_in_flight;
    if (memory->frames_in_flight == 0)
    {
        memory->frames_in_flight = TB_FRAME_MEMORY_DEFAULT_FRAMES;
    }
    memory->chunk_size = options->chunk_size;
    tb_thread_local_create(&memory->lane_of_thread, NULL);
    return memory;
}

void tb_frame_memory_destroy(tb_frame_memory *memory)
{
    tb_frame_memory_lane *lane = NULL;

    if (memory == NULL)
    {
        return;
    }
    lane = memory->lanes;
    while (lane != NULL)
    {
        tb_frame_memory_lane *const next = lane->next;

        for (size_t index = 0; index < TB_FRAME_MEMORY_MOST_FRAMES; index++)
        {
            tb_arena_destroy(lane->arenas[index]);
        }
        tb_free(lane);
        lane = next;
    }
    /* No destructor runs: the lanes belong to the frame memory, not to the
     * threads, and are released above. */
    tb_thread_local_destroy(&memory->lane_of_thread);
    tb_free(memory);
}

void tb_frame_memory_begin(tb_frame_memory *memory)
{
    TB_ASSERT(memory != NULL);
    memory->frame++;
    memory->current = (memory->current + 1) % memory->frames_in_flight;
    tb_mutex_lock(&memory->lanes_lock);
    for (tb_frame_memory_lane *lane = memory->lanes; lane != NULL; lane = lane->next)
    {
        if (lane->arenas[memory->current] != NULL)
        {
            tb_arena_reset(lane->arenas[memory->current]);
        }
    }
    tb_mutex_unlock(&memory->lanes_lock);
}

uint64_t tb_frame_memory_frame(const tb_frame_memory *memory)
{
    TB_ASSERT(memory != NULL);
    return memory->frame;
}

/* The calling thread's lane, made and listed the first time it asks. */
static tb_frame_memory_lane *tb_frame_memory_lane_of_caller(tb_frame_memory *memory)
{
    tb_frame_memory_lane *lane = tb_thread_local_get(&memory->lane_of_thread);

    if (lane != NULL)
    {
        return lane;
    }
    lane = TB_NEW(tb_frame_memory_lane);
    tb_mutex_lock(&memory->lanes_lock);
    lane->next = memory->lanes;
    memory->lanes = lane;
    tb_mutex_unlock(&memory->lanes_lock);
    tb_thread_local_set(&memory->lane_of_thread, lane);
    return lane;
}

tb_arena *tb_frame_memory_arena(tb_frame_memory *memory)
{
    tb_frame_memory_lane *lane = NULL;
    tb_arena **arena = NULL;

    TB_ASSERT(memory != NULL);
    lane = tb_frame_memory_lane_of_caller(memory);
    arena = &lane->arenas[memory->current];
    if (*arena == NULL)
    {
        *arena = tb_arena_create(memory->chunk_size);
    }
    return *arena;
}

void *tb_frame_memory_alloc(tb_frame_memory *memory, size_t size, size_t alignment)
{
    return tb_arena_alloc(tb_frame_memory_arena(memory), size, alignment);
}
