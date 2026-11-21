/* Frame memory must give each thread an arena of its own, keep what a frame
 * allocated valid while that frame is in flight, release it when the frame's
 * arenas come round again, stop allocating once every thread's arenas have
 * grown to their working size, and give everything back when destroyed. */
#include "badger/assert.h"
#include "badger/frame_memory.h"
#include "badger/jobs.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * One thread.
 * ------------------------------------------------------------------------- */

TB_TEST(core_frame_memory, the_first_frame_is_frame_0_and_each_begin_counts)
{
    tb_frame_memory *const memory = tb_frame_memory_create(NULL);

    TB_TEST_EXPECT_UINT(tb_frame_memory_frame(memory), ==, 0);
    tb_frame_memory_begin(memory);
    tb_frame_memory_begin(memory);
    TB_TEST_EXPECT_UINT(tb_frame_memory_frame(memory), ==, 2);
    tb_frame_memory_destroy(memory);
    tb_frame_memory_destroy(NULL);
}

TB_TEST(core_frame_memory, a_thread_gets_one_arena_for_the_whole_frame)
{
    tb_frame_memory *const memory = tb_frame_memory_create(NULL);
    tb_arena *const arena = tb_frame_memory_arena(memory);

    TB_TEST_EXPECT_POINTER(tb_frame_memory_arena(memory), ==, arena);
    (void)tb_frame_memory_alloc(memory, 100, 8);
    TB_TEST_EXPECT_POINTER(tb_frame_memory_arena(memory), ==, arena);
    tb_frame_memory_destroy(memory);
}

/* Memory from frame N is untouched by the frames after it until frame
 * N + frames_in_flight begins, which reuses the arena it came from. */
static void check_frames_in_flight(size_t asked, size_t expected)
{
    const tb_frame_memory_options options = {.frames_in_flight = asked};
    tb_frame_memory *const memory = tb_frame_memory_create(&options);
    tb_arena *const first_arena = tb_frame_memory_arena(memory);
    size_t empty_remaining = 0;
    uint64_t *value = NULL;

    empty_remaining = tb_arena_remaining(first_arena);
    value = tb_frame_memory_alloc(memory, sizeof(uint64_t), _Alignof(uint64_t));
    *value = UINT64_C(0x1234567890ABCDEF);
    for (size_t frame = 1; frame < expected; frame++)
    {
        tb_frame_memory_begin(memory);
        TB_TEST_CONTEXT("frames in flight %zu, frame %zu", expected, frame);
        TB_TEST_EXPECT_POINTER(tb_frame_memory_arena(memory), !=, first_arena);
        /* Allocating in a later frame does not disturb it. */
        memset(tb_frame_memory_alloc(memory, 64, 8), 0xEE, 64);
        TB_TEST_EXPECT_HEX(*value, ==, UINT64_C(0x1234567890ABCDEF));
    }
    tb_frame_memory_begin(memory);
    TB_TEST_CONTEXT("frames in flight %zu, frame %zu", expected, expected);
    TB_TEST_EXPECT_POINTER(tb_frame_memory_arena(memory), ==, first_arena);
    TB_TEST_EXPECT_UINT(tb_arena_remaining(first_arena), ==, empty_remaining);
    tb_frame_memory_destroy(memory);
}

TB_TEST(core_frame_memory, memory_lives_until_its_frame_comes_round_again)
{
    check_frames_in_flight(1, 1);
    check_frames_in_flight(2, 2);
    check_frames_in_flight(3, 3);
    check_frames_in_flight(TB_FRAME_MEMORY_MOST_FRAMES, TB_FRAME_MEMORY_MOST_FRAMES);
}

TB_TEST(core_frame_memory, two_frames_are_in_flight_by_default)
{
    check_frames_in_flight(0, TB_FRAME_MEMORY_DEFAULT_FRAMES);
    TB_TEST_EXPECT_UINT(TB_FRAME_MEMORY_DEFAULT_FRAMES, ==, 2);
}

TB_TEST(core_frame_memory, the_chunk_size_is_that_of_every_arena)
{
    const tb_frame_memory_options options = {.chunk_size = 4096};
    tb_frame_memory *const memory = tb_frame_memory_create(&options);
    tb_arena *const reference = tb_arena_create(4096);

    TB_TEST_EXPECT_UINT(tb_arena_capacity(tb_frame_memory_arena(memory)), ==,
                        tb_arena_capacity(reference));
    tb_arena_destroy(reference);
    tb_frame_memory_destroy(memory);
}

/* A thread-local place given back by one frame memory may be the one the
 * system hands the next. The new one must not see the old one's lane. */
TB_TEST(core_frame_memory, frame_memory_made_after_another_is_destroyed_starts_afresh)
{
    for (size_t round = 0; round < 3; round++)
    {
        tb_frame_memory *const memory = tb_frame_memory_create(NULL);
        uint32_t *const value = tb_frame_memory_alloc(memory, sizeof(uint32_t), 4);

        *value = (uint32_t)round;
        TB_TEST_EXPECT_UINT(*value, ==, round);
        tb_frame_memory_destroy(memory);
    }
}

/* ---------------------------------------------------------------------------
 * Many threads.
 * ------------------------------------------------------------------------- */

enum
{
    THREAD_COUNT = 4,
    VALUES_PER_THREAD = 2000
};

typedef struct thread_work
{
    tb_frame_memory *memory;
    uint32_t identity;
    tb_arena *arena;  /* The arena the thread got. */
    uint32_t *values; /* What it allocated, from frame memory. */
} thread_work;

static void allocate_on_a_thread(void *argument)
{
    thread_work *const work = argument;

    work->arena = tb_frame_memory_arena(work->memory);
    work->values = tb_frame_memory_alloc(work->memory, VALUES_PER_THREAD * sizeof(uint32_t),
                                         _Alignof(uint32_t));
    for (size_t index = 0; index < VALUES_PER_THREAD; index++)
    {
        work->values[index] = work->identity * 100000u + (uint32_t)index;
    }
}

TB_TEST(core_frame_memory, every_thread_has_an_arena_of_its_own)
{
    tb_frame_memory *const memory = tb_frame_memory_create(NULL);
    thread_work work[THREAD_COUNT] = {0};
    tb_thread threads[THREAD_COUNT] = {0};
    size_t damaged = 0;

    for (uint32_t index = 0; index < THREAD_COUNT; index++)
    {
        work[index] = (thread_work){.memory = memory, .identity = index + 1};
        tb_thread_create(&threads[index], allocate_on_a_thread, &work[index]);
    }
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }
    for (size_t first = 0; first < THREAD_COUNT; first++)
    {
        TB_TEST_EXPECT_POINTER(work[first].arena, !=, tb_frame_memory_arena(memory));
        for (size_t second = first + 1; second < THREAD_COUNT; second++)
        {
            TB_TEST_EXPECT_POINTER(work[first].arena, !=, work[second].arena);
        }
        for (size_t index = 0; index < VALUES_PER_THREAD; index++)
        {
            if (work[first].values[index] != work[first].identity * 100000u + (uint32_t)index)
            {
                damaged++;
            }
        }
    }
    /* What the threads allocated outlives them, until its frame ends. */
    TB_TEST_EXPECT_UINT(damaged, ==, 0);
    tb_frame_memory_destroy(memory);
}

typedef struct frame_loop
{
    tb_frame_memory *memory;
    uint64_t frame;
    tb_platform_atomic_size overwritten; /* Values found changed, on any thread. */
} frame_loop;

/* A job's share of a frame: a list in frame memory, filled and checked. */
static void fill_some(void *argument, size_t first, size_t count)
{
    frame_loop *const loop = argument;
    uint64_t *const values =
        tb_frame_memory_alloc(loop->memory, count * sizeof(uint64_t), _Alignof(uint64_t));

    for (size_t index = 0; index < count; index++)
    {
        values[index] = loop->frame * 1000003u + first + index;
    }
    for (size_t index = 0; index < count; index++)
    {
        if (values[index] != loop->frame * 1000003u + first + index)
        {
            (void)tb_platform_atomic_size_add(&loop->overwritten, 1);
        }
    }
}

/* The frame loop of an engine: workers allocate from frame memory in every
 * frame, and the owner begins each new frame after waiting for them. Once
 * every thread's arenas have grown to their working size, frames allocate
 * nothing more from the heap. */
TB_TEST(core_frame_memory, frames_on_worker_threads_stop_allocating_once_warmed_up)
{
    const tb_jobs_options jobs_options = {.worker_count = 3};
    tb_jobs *const jobs = tb_jobs_create(&jobs_options);
    tb_frame_memory *const memory = tb_frame_memory_create(NULL);
    frame_loop loop = {.memory = memory};
    size_t blocks_after_warm_up = 0;

    for (uint64_t frame = 0; frame < 40; frame++)
    {
        loop.frame = frame;
        tb_jobs_for(jobs, 20000, 500, fill_some, &loop);
        if (frame == 9)
        {
            blocks_after_warm_up = tb_memory_allocation_count();
        }
        tb_frame_memory_begin(memory);
    }
    /* A worker that first joins a loop late can still add a lane, or an
     * arena for a frame it had not used; anything more would be growth
     * without bound. */
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), <=,
                        blocks_after_warm_up + 4 * TB_FRAME_MEMORY_DEFAULT_FRAMES);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&loop.overwritten), ==, 0);
    tb_frame_memory_destroy(memory);
    tb_jobs_destroy(jobs);
}

/* ---------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static void create_with_too_many_frames(void *context)
{
    const tb_frame_memory_options options = {.frames_in_flight = TB_FRAME_MEMORY_MOST_FRAMES + 1};

    (void)context;
    (void)tb_frame_memory_create(&options);
}

static void begin_null(void *context)
{
    (void)context;
    tb_frame_memory_begin(NULL);
}

static void arena_of_null(void *context)
{
    (void)context;
    (void)tb_frame_memory_arena(NULL);
}

TB_TEST(core_frame_memory, bad_arguments_violate_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(create_with_too_many_frames, NULL,
                        "frames_in_flight is at most TB_FRAME_MEMORY_MOST_FRAMES");
    TB_TEST_EXPECT_TRAP(begin_null, NULL, "memory != NULL");
    TB_TEST_EXPECT_TRAP(arena_of_null, NULL, "memory != NULL");
}
