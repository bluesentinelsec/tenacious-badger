/** @file allocator.h
 * @brief An allocator that is passed to the object that uses it.
 *
 * memory.h decides where the whole engine gets its memory. This header lets
 * one object get its memory somewhere else: an array in the arena of a frame,
 * a table in the pool of a level, the rest on the engine's heap. Every engine
 * module that owns memory takes an allocator when it is created and uses it
 * for everything it allocates.
 *
 * @code
 * static vertex *copy_vertices(const tb_allocator *allocator, const vertex *source, size_t count)
 * {
 *     vertex *const copy = TB_ALLOCATOR_NEW_ARRAY(allocator, vertex, count);
 *
 *     memcpy(copy, source, count * sizeof(*copy));
 *     return copy;
 * }
 *
 * const tb_allocator in_frame = tb_arena_allocator(frame_arena);
 * vertex *const scratch = copy_vertices(&in_frame, vertices, count); // in the arena
 * vertex *const kept = copy_vertices(NULL, vertices, count);         // on the heap
 * @endcode
 *
 * **NULL is the engine's heap.** Every function that takes a
 * `const tb_allocator *` accepts NULL, which selects the allocation functions
 * of memory.h. Code that does not care passes NULL.
 *
 * **An allocator is a value.** It is a function and a context pointer. An
 * object that takes an allocator copies it, so the structure passed in need
 * not outlive the call. What the context points to must outlive every block.
 *
 * **Blocks carry no header.** The allocator is told the size and alignment of
 * every block it is asked to resize or release. That is what lets an arena or
 * a pool be an allocator without bookkeeping per block, and it obliges the
 * owner of a block to know its size, which the engine's containers do.
 *
 * **Failing fast is the default**, as in memory.h. tb_allocator_alloc() and
 * its relatives never return NULL: when the allocator cannot supply the memory
 * they call the memory failure handler and end the process. The
 * `tb_allocator_try_*` forms return NULL for callers that can recover.
 *
 * @par Threading
 * The functions here are as thread-safe as the allocator they are given. The
 * engine's heap is thread-safe. An arena allocator is externally
 * synchronized, like its arena.
 */
#ifndef TB_ALLOCATOR_H
#define TB_ALLOCATOR_H

#include "badger/api.h"

#include <stddef.h>

/**
 * @brief The one function of an allocator: allocate, resize, or release.
 *
 * Which of the three it does follows from its arguments:
 *
 * | `memory` | `new_size` | Operation |
 * | --- | --- | --- |
 * | NULL | not 0 | Allocate `new_size` bytes. |
 * | not NULL | not 0 | Resize the block from `old_size` to `new_size` bytes, moving it if necessary
 * and keeping its contents up to the smaller size. | | not NULL | 0 | Release the block. |
 *
 * The functions of this header are the only callers, and they guarantee the
 * following, so an implementation need not check it:
 * - `memory` is NULL or a block this allocator returned and has not released.
 * - `old_size` and `alignment` are the size and alignment the block was last
 *   given; `old_size` is 0 exactly when `memory` is NULL.
 * - `alignment` is a power of two.
 * - `memory` and `new_size` are never both absent.
 *
 * @param context The allocator's own state, from tb_allocator::context.
 * @param memory Block to resize or release, or NULL to allocate.
 * @param old_size Current size of the block in bytes.
 * @param new_size Size wanted in bytes, or 0 to release.
 * @param alignment Alignment of the block in bytes.
 * @return The new or resized block, whose address is a multiple of
 * `alignment`; the address passed in is then no longer valid. NULL when the
 * memory cannot be supplied, leaving the block passed in untouched and still
 * allocated. NULL after a release.
 */
typedef void *tb_allocator_function(void *context, void *memory, size_t old_size, size_t new_size,
                                    size_t alignment);

/**
 * @brief An allocator: where an object gets its memory.
 *
 * Fill one in to supply a custom allocator, or get one from
 * tb_allocator_heap() or tb_arena_allocator(). Pass it by address; whoever
 * receives it copies it.
 */
typedef struct tb_allocator
{
    tb_allocator_function *function; /**< Allocates, resizes, and releases; required non-null. */
    void *context;                   /**< Passed to every call of `function`; may be NULL. */
} tb_allocator;

/**
 * @brief The engine's allocation functions (memory.h) as an allocator.
 *
 * This is what NULL selects. Blocks of any alignment are supported; those
 * beyond what tb_malloc() guarantees are moved when they are resized.
 *
 * @par Threading Thread-safe.
 */
TB_API tb_allocator tb_allocator_heap(void);

/**
 * @brief Copy an allocator, or the engine's heap when there is none.
 *
 * A module that takes an optional allocator calls this once, when it creates
 * an object, and keeps the result in the object.
 *
 * @param allocator Allocator to copy, or NULL for tb_allocator_heap(). Its
 * function must not be NULL.
 */
TB_API tb_allocator tb_allocator_resolve(const tb_allocator *allocator);

/* -------------------------------------------------------------------------
 * Allocation that fails fast.
 * ------------------------------------------------------------------------- */

/**
 * @brief Allocate uninitialized memory from an allocator.
 * @param allocator Allocator to use, or NULL for the engine's heap.
 * @param size Bytes to allocate; 0 is served as 1.
 * @param alignment Required alignment in bytes; a power of two.
 * @return A block of at least `size` bytes; never NULL. The caller owns it and
 * releases it with tb_allocator_free(), passing the same allocator, size, and
 * alignment.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API void *tb_allocator_alloc(const tb_allocator *allocator, size_t size, size_t alignment);

/**
 * @brief Allocate a zero-filled array from an allocator.
 * @param allocator Allocator to use, or NULL for the engine's heap.
 * @param count Number of elements; 0 is served as 1.
 * @param size Bytes per element; 0 is served as 1.
 * @param alignment Required alignment in bytes; a power of two.
 * @return A zero-filled block of at least `count * size` bytes; never NULL.
 * Release it with tb_allocator_free_array().
 * @par Errors Exhaustion, or a `count * size` that overflows `size_t`, calls
 * the memory failure handler and ends the process.
 */
TB_API void *tb_allocator_calloc(const tb_allocator *allocator, size_t count, size_t size,
                                 size_t alignment);

/**
 * @brief Change the size of a block, moving it if necessary.
 * @param allocator The allocator the block came from, or NULL for the heap.
 * @param memory Block to resize, or NULL to allocate a new one.
 * @param old_size Size the block was last given; ignored when `memory` is NULL.
 * @param new_size New size in bytes; 0 is served as 1 and never releases.
 * @param alignment The alignment the block was allocated with.
 * @return The resized block, whose contents are preserved up to the smaller
 * size; never NULL. The pointer passed in is no longer valid.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API void *tb_allocator_realloc(const tb_allocator *allocator, void *memory, size_t old_size,
                                  size_t new_size, size_t alignment);

/**
 * @brief Release a block.
 * @param allocator The allocator the block came from, or NULL for the heap.
 * @param memory Block to release; NULL is ignored.
 * @param size Size the block was last given.
 * @param alignment The alignment the block was allocated with.
 */
TB_API void tb_allocator_free(const tb_allocator *allocator, void *memory, size_t size,
                              size_t alignment);

/**
 * @brief Release an array from tb_allocator_calloc().
 * @param allocator The allocator the array came from, or NULL for the heap.
 * @param memory Array to release; NULL is ignored.
 * @param count Number of elements the array was allocated with.
 * @param size Bytes per element it was allocated with.
 * @param alignment The alignment it was allocated with.
 */
TB_API void tb_allocator_free_array(const tb_allocator *allocator, void *memory, size_t count,
                                    size_t size, size_t alignment);

/**
 * @brief Allocate one zero-filled object: `TB_ALLOCATOR_NEW(allocator, player)`.
 *
 * Use it only for types whose all-zero representation is a valid state.
 * Release the object with TB_ALLOCATOR_DELETE().
 *
 * @param allocator Allocator to use, or NULL for the engine's heap.
 * @param type Object type.
 * @return Pointer to the object, aligned for its type; never NULL.
 */
#define TB_ALLOCATOR_NEW(allocator, type)                                                          \
    ((type *)tb_allocator_calloc((allocator), 1, sizeof(type), _Alignof(type)))

/**
 * @brief Allocate a zero-filled array: `TB_ALLOCATOR_NEW_ARRAY(allocator, vertex, count)`.
 * @param allocator Allocator to use, or NULL for the engine's heap.
 * @param type Element type.
 * @param count Number of elements, as a `size_t`.
 * @return Pointer to the first element; never NULL. Release it with
 * TB_ALLOCATOR_DELETE_ARRAY() and the same count.
 */
#define TB_ALLOCATOR_NEW_ARRAY(allocator, type, count)                                             \
    ((type *)tb_allocator_calloc((allocator), (count), sizeof(type), _Alignof(type)))

/**
 * @brief Release an object from TB_ALLOCATOR_NEW().
 * @param allocator The allocator the object came from, or NULL for the heap.
 * @param type The type it was allocated as.
 * @param object Object to release; NULL is ignored.
 */
#define TB_ALLOCATOR_DELETE(allocator, type, object)                                               \
    tb_allocator_free_array((allocator), (object), 1, sizeof(type), _Alignof(type))

/**
 * @brief Release an array from TB_ALLOCATOR_NEW_ARRAY().
 * @param allocator The allocator the array came from, or NULL for the heap.
 * @param type The element type it was allocated as.
 * @param array Array to release; NULL is ignored.
 * @param count The number of elements it was allocated with.
 */
#define TB_ALLOCATOR_DELETE_ARRAY(allocator, type, array, count)                                   \
    tb_allocator_free_array((allocator), (array), (count), sizeof(type), _Alignof(type))

/* -------------------------------------------------------------------------
 * Allocation for callers that can recover.
 * ------------------------------------------------------------------------- */

/**
 * @brief Allocate uninitialized memory, or report that it is not available.
 * @param allocator Allocator to use, or NULL for the engine's heap.
 * @param size Bytes to allocate; 0 is served as 1.
 * @param alignment Required alignment in bytes; a power of two.
 * @return A block as from tb_allocator_alloc(), or NULL when the allocator
 * cannot supply it.
 */
TB_API void *tb_allocator_try_alloc(const tb_allocator *allocator, size_t size, size_t alignment);

/**
 * @brief Allocate a zero-filled array, or report that it is not available.
 * @param allocator Allocator to use, or NULL for the engine's heap.
 * @param count Number of elements; 0 is served as 1.
 * @param size Bytes per element; 0 is served as 1.
 * @param alignment Required alignment in bytes; a power of two.
 * @return A block as from tb_allocator_calloc(), or NULL when the allocator
 * cannot supply it or `count * size` overflows `size_t`.
 */
TB_API void *tb_allocator_try_calloc(const tb_allocator *allocator, size_t count, size_t size,
                                     size_t alignment);

/**
 * @brief Change the size of a block, or report that it cannot be done.
 * @param allocator The allocator the block came from, or NULL for the heap.
 * @param memory Block to resize, or NULL to allocate a new one.
 * @param old_size Size the block was last given; ignored when `memory` is NULL.
 * @param new_size New size in bytes; 0 is served as 1 and never releases.
 * @param alignment The alignment the block was allocated with.
 * @return The resized block, or NULL when the allocator cannot supply it. On
 * NULL the original block is untouched and still owned by the caller.
 */
TB_API void *tb_allocator_try_realloc(const tb_allocator *allocator, void *memory, size_t old_size,
                                      size_t new_size, size_t alignment);

#endif
