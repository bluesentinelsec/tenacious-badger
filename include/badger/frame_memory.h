/** @file frame_memory.h
 * @brief Memory that lives for a frame or a few, allocated from any thread
 * without contention and released all at once.
 *
 * Much of what a frame makes is dead when the frame's consumers are done with
 * it: render requests, culling results, draw lists, temporary strings. Frame
 * memory serves it with an arena per thread, so a worker allocates with no
 * lock and no atomic operation, and releases all of it by resetting those
 * arenas when the frame's memory comes round again.
 *
 * @code
 * tb_frame_memory *const frame_memory = tb_frame_memory_create(NULL);
 *
 * for (;;)
 * {
 *     tb_frame_memory_begin(frame_memory);
 *     // On any thread, during the frame:
 *     draw *const draws = TB_ARENA_NEW_ARRAY(tb_frame_memory_arena(frame_memory), draw, count);
 *     ...
 * }
 * @endcode
 *
 * **Frames in flight.** A pipelined engine still reads frame N's data while it
 * builds frame N + 1: the render thread consumes what the game thread made the
 * frame before. Frame memory therefore keeps a set of arenas for each frame in
 * flight, two by default, and uses them in turn. Memory allocated during frame
 * N is valid until tb_frame_memory_begin() starts frame N plus the number of
 * frames in flight, which reuses that set: with two, until frame N + 2 begins.
 *
 * **A thread's arena.** Each thread gets an arena of its own for each frame in
 * flight the first time it asks during that frame's turn, and keeps it until
 * the frame memory is destroyed. Its arena is an ordinary tb_arena, used by
 * that thread alone: allocate from it with tb_arena_alloc(), TB_ARENA_NEW(),
 * marks, or tb_arena_allocator(). Pointers into it may be passed to any thread.
 * A thread that ends leaves its arenas to the frame memory, which releases
 * them when it is destroyed; an engine whose threads are a fixed set of
 * workers loses nothing by that.
 *
 * **Failing fast**, as arenas do: when memory runs out, the memory failure
 * handler is called and the process ends.
 *
 * @par Threading
 * tb_frame_memory_arena() and tb_frame_memory_alloc() are thread-safe: any
 * thread may call them during a frame, and each gets its own arena.
 * tb_frame_memory_begin() is stage-owned: one thread, the owner of the frame
 * loop, calls it between frames, when no other thread is using the frame
 * memory and every thread that used it in the previous frame has finished,
 * in a way that synchronizes with the owner, as waiting for its jobs does.
 * tb_frame_memory_create() and tb_frame_memory_destroy() are externally
 * synchronized with everything else.
 */
#ifndef TB_FRAME_MEMORY_H
#define TB_FRAME_MEMORY_H

#include "badger/api.h"
#include "badger/arena.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Frame memory. Created by tb_frame_memory_create(); owned by the
 * caller; released by tb_frame_memory_destroy().
 */
typedef struct tb_frame_memory tb_frame_memory;

/** @brief The most frames that may be in flight. */
#define TB_FRAME_MEMORY_MOST_FRAMES ((size_t)4)

/** @brief Frames in flight when the options leave it to the frame memory. */
#define TB_FRAME_MEMORY_DEFAULT_FRAMES ((size_t)2)

/** @brief How frame memory is set up. Zero-initialize it for the defaults. */
typedef struct tb_frame_memory_options
{
    /** How many frames' memory is live at once: 1 to
     * #TB_FRAME_MEMORY_MOST_FRAMES, or 0 for #TB_FRAME_MEMORY_DEFAULT_FRAMES. */
    size_t frames_in_flight;
    /** The chunk size of every thread's arenas, as for tb_arena_create(); 0
     * for #TB_ARENA_DEFAULT_CHUNK_SIZE. */
    size_t chunk_size;
} tb_frame_memory_options;

/**
 * @brief Create frame memory, in its first frame.
 *
 * Frame 0 has begun: threads may allocate at once, and the first call of
 * tb_frame_memory_begin() starts frame 1.
 *
 * @param options How to set it up; NULL for the defaults.
 * @return The frame memory; never NULL. The caller owns it and releases it
 * with tb_frame_memory_destroy().
 * @par Errors Exhaustion calls the memory failure handler and ends the
 * process, as does running out of the system's thread-local places, of which
 * each frame memory uses one.
 */
TB_API tb_frame_memory *tb_frame_memory_create(const tb_frame_memory_options *options);

/**
 * @brief Destroy frame memory and every arena in it.
 * @param memory Frame memory to destroy; NULL is ignored. No thread may be
 * using it. The pointer, every arena it gave out, and every pointer into those
 * arenas are invalid afterwards.
 */
TB_API void tb_frame_memory_destroy(tb_frame_memory *memory);

/**
 * @brief Start the next frame, releasing the memory of the frame whose arenas
 * it reuses.
 *
 * With F frames in flight, starting frame N resets every thread's arena of
 * frame N - F: what was allocated then is released, and its chunks are kept
 * for this frame.
 *
 * @param memory Required non-null.
 * @par Threading Stage-owned: see the threading contract of this header.
 */
TB_API void tb_frame_memory_begin(tb_frame_memory *memory);

/**
 * @brief The number of the current frame: 0 at creation, and one more for
 * every tb_frame_memory_begin().
 * @param memory Required non-null.
 * @par Threading Stage-owned: read it on the thread that calls
 * tb_frame_memory_begin(), or after synchronizing with it.
 */
TB_API uint64_t tb_frame_memory_frame(const tb_frame_memory *memory);

/**
 * @brief The calling thread's arena for the current frame.
 *
 * The first call on a thread in a frame's turn creates the arena; later calls
 * return the same one until the frame memory is destroyed.
 *
 * @param memory Required non-null.
 * @return The arena; never NULL. Only the calling thread may allocate from
 * it, and only until the frame ends: do not keep it for a later frame, or
 * destroy, reset, or rewind it past what this frame allocated. Its memory is
 * valid until the frame's arenas are reused, as tb_frame_memory_begin()
 * describes.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 * @par Threading Thread-safe.
 */
TB_API tb_arena *tb_frame_memory_arena(tb_frame_memory *memory);

/**
 * @brief Allocate from the calling thread's arena for the current frame.
 *
 * The same as tb_arena_alloc() on tb_frame_memory_arena().
 *
 * @param memory Required non-null.
 * @param size Bytes to allocate; 0 is served as 1.
 * @param alignment Required alignment in bytes; a power of two.
 * @return Uninitialized memory, valid as tb_frame_memory_arena() describes;
 * never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 * @par Threading Thread-safe.
 */
TB_API void *tb_frame_memory_alloc(tb_frame_memory *memory, size_t size, size_t alignment);

#endif
