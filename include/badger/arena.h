/** @file arena.h
 * @brief Arena allocation: objects that share a lifetime, released together.
 *
 * An arena hands out memory by advancing a position through large chunks, and
 * takes it all back at once. Nothing is freed object by object, so allocation
 * costs a few instructions, a lifetime is ended by one call, and a leak of a
 * single object is impossible. Use an arena when a group of objects dies
 * together: the scratch memory of one frame, the data of one level, the tree a
 * parser builds and discards.
 *
 * @code
 * tb_arena *const arena = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
 * token *const tokens = TB_ARENA_NEW_ARRAY(arena, token, token_count);
 *
 * parse(tokens, token_count, arena);
 * tb_arena_destroy(arena); // every object the parse allocated is gone
 * @endcode
 *
 * **Lifetimes.** An object from an arena is valid until the arena is destroyed,
 * reset, or rewound to a mark taken before the object was allocated. After
 * that the pointer must not be used. The arena never runs code for an object
 * it releases, so an object that owns something else (a file, a block from
 * tb_malloc()) must have that released first.
 *
 * **Two kinds.** A growing arena takes chunks from the engine's allocation
 * functions as it fills (tb_arena_create()). A fixed arena lives entirely in a
 * buffer the caller supplies and never allocates (tb_arena_create_fixed()).
 *
 * **Whose memory.** A growing arena takes each chunk with one call to the
 * memory functions installed by tb_memory_set_functions(), and returns it with
 * one call when the arena is destroyed. An application that installs its own
 * allocator therefore supplies all of an arena's memory.
 *
 * **Failing fast is the default**, as in memory.h. tb_arena_alloc() and
 * tb_arena_calloc() never return NULL: when the arena cannot supply the memory
 * they call the memory failure handler and end the process. The
 * `tb_arena_try_*` forms return NULL for callers that can recover.
 *
 * **Finding stale pointers.** In builds with assertions enabled, memory the
 * arena hands out is filled with #TB_ARENA_FILL_BYTE, the first time and
 * every time it is reused, so that reading an object before initializing it
 * or after releasing it shows a recognizable value. Under AddressSanitizer,
 * memory that is not part of a live object is poisoned, and touching it is
 * reported as `use-after-poison`.
 *
 * @par Threading
 * Externally synchronized: an arena has no lock, and the caller serializes
 * every call that names the same arena. Different arenas are independent.
 * Creating and growing an arena uses the engine's allocation functions, which
 * are thread-safe.
 */
#ifndef TB_ARENA_H
#define TB_ARENA_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stddef.h>

/**
 * @brief An arena. Created by tb_arena_create(), tb_arena_try_create(), or
 * tb_arena_create_fixed(); owned by the caller; released by tb_arena_destroy().
 */
typedef struct tb_arena tb_arena;

/** @brief A chunk size that suits most arenas: 64 KiB. */
#define TB_ARENA_DEFAULT_CHUNK_SIZE ((size_t)64 * 1024)

/**
 * @brief The smallest chunk of a growing arena and the smallest buffer of a
 * fixed one, in bytes. An arena keeps its own bookkeeping in its memory, and
 * this leaves room for it.
 */
#define TB_ARENA_MINIMUM_SIZE ((size_t)256)

/**
 * @brief Value of every byte of an object when it is handed out, in builds
 * with assertions enabled (see tb_assert_library_enabled()). Other builds
 * leave the bytes as they were.
 */
#define TB_ARENA_FILL_BYTE 0xDD

/* -------------------------------------------------------------------------
 * Creating and destroying.
 * ------------------------------------------------------------------------- */

/**
 * @brief Create an arena that grows.
 *
 * The arena starts with one chunk and adds another whenever an object does not
 * fit in the memory it holds. An object larger than a chunk gets a chunk of
 * its own, so any size can be served.
 *
 * @param chunk_size Bytes the arena takes from the engine's allocation
 * functions for each chunk, bookkeeping included. 0 selects
 * #TB_ARENA_DEFAULT_CHUNK_SIZE; any other value below #TB_ARENA_MINIMUM_SIZE
 * is raised to it. Pick a size that holds many typical objects.
 * @return The arena; never NULL. The caller owns it and releases it with
 * tb_arena_destroy().
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_arena *tb_arena_create(size_t chunk_size);

/**
 * @brief Create an arena that grows, or report that memory is not available.
 * @param chunk_size As for tb_arena_create().
 * @return An arena as from tb_arena_create(), or NULL when its first chunk
 * cannot be allocated.
 */
TB_API tb_arena *tb_arena_try_create(size_t chunk_size);

/**
 * @brief Create an arena inside a buffer. It never allocates and never grows.
 *
 * The arena and every object it hands out live in the buffer, which may be
 * static, on the stack, or part of a larger allocation. A small part of the
 * buffer holds the arena's bookkeeping; tb_arena_capacity() reports what is
 * left for objects. When the buffer is full, the fail-fast functions end the
 * process and the `tb_arena_try_*` functions return NULL.
 *
 * @param buffer Required non-null; need not be aligned. The caller keeps
 * ownership and must keep it valid, and must not touch it, until the arena is
 * destroyed.
 * @param size Size of the buffer in bytes; at least #TB_ARENA_MINIMUM_SIZE.
 * @return The arena; never NULL. Destroy it with tb_arena_destroy() before the
 * buffer is reused or goes out of scope: that is what tells AddressSanitizer
 * that the buffer is ordinary memory again.
 */
TB_API tb_arena *tb_arena_create_fixed(void *buffer, size_t size);

/**
 * @brief Destroy an arena and release every object allocated from it.
 * @param arena Arena to destroy; NULL is ignored. The pointer, and every
 * pointer to an object from the arena, is invalid afterwards. A growing arena
 * returns its chunks to the engine's allocation functions. A fixed arena
 * leaves its buffer to the caller.
 */
TB_API void tb_arena_destroy(tb_arena *arena);

/* -------------------------------------------------------------------------
 * Allocation that fails fast.
 * ------------------------------------------------------------------------- */

/**
 * @brief Allocate uninitialized memory from an arena.
 * @param arena Required non-null.
 * @param size Bytes to allocate; 0 is served as 1, so the result is always a
 * unique pointer.
 * @param alignment Required alignment in bytes; a power of two. Use
 * `_Alignof(type)` for an object of a known type.
 * @return A block of at least `size` bytes whose address is a multiple of
 * `alignment`; never NULL. It belongs to the arena: do not free it. It is
 * valid until the arena is destroyed, reset, or rewound past it.
 * @par Errors A full fixed arena, exhaustion of the engine's allocation
 * functions, or a size too large to pad for the alignment calls the memory
 * failure handler and ends the process.
 */
TB_API void *tb_arena_alloc(tb_arena *arena, size_t size, size_t alignment);

/**
 * @brief Allocate a zero-filled array from an arena.
 * @param arena Required non-null.
 * @param count Number of elements; 0 is served as 1.
 * @param size Bytes per element; 0 is served as 1.
 * @param alignment Required alignment in bytes; a power of two.
 * @return A zero-filled block of at least `count * size` bytes, as from
 * tb_arena_alloc(); never NULL.
 * @par Errors As for tb_arena_alloc(); a `count * size` that overflows
 * `size_t` is reported the same way.
 */
TB_API void *tb_arena_calloc(tb_arena *arena, size_t count, size_t size, size_t alignment);

/**
 * @brief Allocate one zero-filled object: `TB_ARENA_NEW(arena, player)`.
 *
 * Use it only for types whose all-zero representation is a valid state.
 *
 * @param arena Arena to allocate from; required non-null.
 * @param type Object type.
 * @return Pointer to the object, aligned for its type; never NULL.
 */
#define TB_ARENA_NEW(arena, type)                                                                  \
    ((type *)tb_arena_calloc((arena), 1, sizeof(type), _Alignof(type)))

/**
 * @brief Allocate a zero-filled array: `TB_ARENA_NEW_ARRAY(arena, vertex, count)`.
 * @param arena Arena to allocate from; required non-null.
 * @param type Element type.
 * @param count Number of elements, as a `size_t`.
 * @return Pointer to the first element, aligned for its type; never NULL.
 */
#define TB_ARENA_NEW_ARRAY(arena, type, count)                                                     \
    ((type *)tb_arena_calloc((arena), (count), sizeof(type), _Alignof(type)))

/* -------------------------------------------------------------------------
 * Allocation for callers that can recover.
 * ------------------------------------------------------------------------- */

/**
 * @brief Allocate uninitialized memory, or report that it is not available.
 * @param arena Required non-null.
 * @param size Bytes to allocate; 0 is served as 1.
 * @param alignment Required alignment in bytes; a power of two.
 * @return A block as from tb_arena_alloc(), or NULL when a fixed arena is
 * full, the engine's allocation functions are exhausted, or the size is too
 * large to pad for the alignment. On NULL the arena is unchanged and remains
 * usable.
 */
TB_API void *tb_arena_try_alloc(tb_arena *arena, size_t size, size_t alignment);

/**
 * @brief Allocate a zero-filled array, or report that it is not available.
 * @param arena Required non-null.
 * @param count Number of elements; 0 is served as 1.
 * @param size Bytes per element; 0 is served as 1.
 * @param alignment Required alignment in bytes; a power of two.
 * @return A block as from tb_arena_calloc(), or NULL as for
 * tb_arena_try_alloc() or when `count * size` overflows `size_t`. On NULL the
 * arena is unchanged and remains usable.
 */
TB_API void *tb_arena_try_calloc(tb_arena *arena, size_t count, size_t size, size_t alignment);

/* -------------------------------------------------------------------------
 * Releasing.
 * ------------------------------------------------------------------------- */

/**
 * @brief Release every object in an arena and keep its memory for reuse.
 *
 * A growing arena keeps all of its chunks, so an arena that is reset and
 * refilled with similar objects, such as one per frame, stops allocating once
 * it has grown to its working size.
 *
 * @param arena Required non-null. Every pointer to an object from the arena,
 * and every mark taken from it, is invalid afterwards.
 */
TB_API void tb_arena_reset(tb_arena *arena);

/**
 * @brief A position in an arena, for releasing everything allocated after it.
 *
 * Pass it by value. The members are private to the arena. A mark is valid
 * until the arena is destroyed, reset, or rewound to an earlier mark.
 */
typedef struct tb_arena_mark
{
    void *chunk;  /**< Private; do not read or write. */
    void *cursor; /**< Private; do not read or write. */
} tb_arena_mark;

/**
 * @brief Remember the arena's current position.
 *
 * Marks give a function temporary memory in an arena it was passed: take a
 * mark, allocate, and rewind before returning.
 *
 * @code
 * const tb_arena_mark mark = tb_arena_get_mark(arena);
 * char *const scratch = tb_arena_alloc(arena, scratch_size, 1);
 *
 * use(scratch);
 * tb_arena_rewind(arena, mark); // scratch is released; earlier objects are not
 * @endcode
 *
 * @param arena Required non-null.
 * @return The mark. Objects allocated before it are unaffected by a rewind to it.
 */
TB_API tb_arena_mark tb_arena_get_mark(const tb_arena *arena);

/**
 * @brief Release every object allocated since a mark was taken.
 *
 * Marks nest: rewinding to a mark also releases every mark taken after it.
 * The memory is kept for reuse, as with tb_arena_reset().
 *
 * @param arena Required non-null.
 * @param mark A mark from tb_arena_get_mark() on this arena that is still
 * valid. A mark from another arena, or one already released by a reset or by
 * a rewind to an earlier mark, is a contract violation; builds with
 * assertions detect it wherever the arena's state still shows it. Pointers to
 * objects allocated since the mark are invalid afterwards.
 */
TB_API void tb_arena_rewind(tb_arena *arena, tb_arena_mark mark);

/* -------------------------------------------------------------------------
 * An arena as an allocator.
 * ------------------------------------------------------------------------- */

/**
 * @brief Get an allocator (allocator.h) that allocates from an arena.
 *
 * Pass it to anything that takes an allocator, such as a collection, to put
 * that object in the arena:
 *
 * @code
 * const tb_allocator in_frame = tb_arena_allocator(frame_arena);
 * draw_call *const calls = TB_ALLOCATOR_NEW_ARRAY(&in_frame, draw_call, call_count);
 * @endcode
 *
 * The allocator follows the rules of the arena:
 * - **Releasing a block does nothing.** Its memory comes back when the arena
 *   is reset, rewound, or destroyed, which also ends the life of every object
 *   allocated through the allocator. Such an object need not be destroyed
 *   first, and must not be used afterwards.
 * - **Growing the most recent block is free.** It is extended in place while
 *   its chunk has room. Any other block that grows is copied to new memory,
 *   and the old memory stays unused until the next reset or rewind.
 * - **A rewind takes back everything since its mark**, including the growth
 *   of a block that was allocated before the mark and resized after it.
 *   Take marks only around objects that are created and abandoned inside them.
 *
 * @param arena Required non-null. It must outlive every use of the allocator.
 * @return The allocator. It is a value: copy it freely.
 * @par Threading Externally synchronized, like the arena.
 */
TB_API tb_allocator tb_arena_allocator(tb_arena *arena);

/* -------------------------------------------------------------------------
 * Accounting.
 * ------------------------------------------------------------------------- */

/**
 * @brief Bytes the arena holds for objects, in use or not, across every chunk.
 *
 * For a fixed arena this is the size of the buffer less the arena's
 * bookkeeping. For a growing arena it rises as chunks are added and never
 * falls: resetting and rewinding keep the memory.
 *
 * @param arena Required non-null.
 */
TB_API size_t tb_arena_capacity(const tb_arena *arena);

/**
 * @brief Bytes left in the chunk the arena is filling.
 *
 * An allocation of this size with an alignment of 1 fits exactly. For a fixed
 * arena it is everything that is left. For a growing arena a larger request
 * still succeeds, by moving to another chunk and leaving these bytes unused
 * until the next reset or rewind.
 *
 * @param arena Required non-null.
 */
TB_API size_t tb_arena_remaining(const tb_arena *arena);

#endif
