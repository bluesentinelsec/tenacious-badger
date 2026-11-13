/** @file memory.h
 * @brief The engine's memory allocation functions.
 *
 * All project code allocates through this header; nothing else calls the C
 * library's allocator. That gives one place to count allocations, to handle
 * exhaustion, and to substitute another allocator.
 *
 * **Failing fast is the default.** `tb_malloc()`, `tb_calloc()`,
 * `tb_realloc()`, and their relatives never return NULL. When memory runs out
 * they report the request and end the process, so ordinary code carries no
 * allocation error paths. A caller that can recover from a failed allocation,
 * such as a loader that can refuse an oversized asset, uses the `tb_try_*`
 * form of the same function and checks for NULL.
 *
 * **Sizes behave the same on every platform.** A request for zero bytes is
 * served as a request for one: the result is a unique pointer that must be
 * freed. Array sizes are checked for overflow before anything is allocated.
 *
 * @par Threading
 * The allocation functions are thread-safe when the underlying memory
 * functions are, which is true of the defaults. Functions that install
 * something (memory functions, the failure handler) are stage-owned: call them
 * during single-threaded startup.
 */
#ifndef TB_MEMORY_H
#define TB_MEMORY_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Allocation that fails fast.
 * ------------------------------------------------------------------------- */

/**
 * @brief Allocate uninitialized memory.
 * @param size Bytes to allocate; 0 is served as 1.
 * @return A block of at least `size` bytes, aligned for any object type; never
 * NULL. The caller owns it and releases it with tb_free().
 * @par Errors Exhaustion calls the failure handler and ends the process.
 */
TB_API void *tb_malloc(size_t size);

/**
 * @brief Allocate a zero-filled array.
 * @param count Number of elements; 0 is served as 1.
 * @param size Bytes per element; 0 is served as 1.
 * @return A zero-filled block of at least `count * size` bytes; never NULL. The
 * caller owns it and releases it with tb_free().
 * @par Errors Exhaustion, or a `count * size` that overflows `size_t`, calls
 * the failure handler and ends the process.
 */
TB_API void *tb_calloc(size_t count, size_t size);

/**
 * @brief Change the size of a block, moving it if necessary.
 * @param memory Block from this header's allocation functions, or NULL to
 * allocate a new one. Not a block from tb_aligned_alloc().
 * @param size New size in bytes; 0 is served as 1 and never frees the block.
 * @return The resized block, whose contents are preserved up to the smaller of
 * the old and new sizes; never NULL. The pointer passed in is no longer valid.
 * @par Errors Exhaustion calls the failure handler and ends the process.
 */
TB_API void *tb_realloc(void *memory, size_t size);

/**
 * @brief Change the size of an array, checking the element arithmetic.
 * @param memory Block as for tb_realloc(), or NULL.
 * @param count New number of elements; 0 is served as 1.
 * @param size Bytes per element; 0 is served as 1.
 * @return The resized block; never NULL. New elements are not initialized.
 * @par Errors Exhaustion, or a `count * size` that overflows `size_t`, calls
 * the failure handler and ends the process.
 */
TB_API void *tb_realloc_array(void *memory, size_t count, size_t size);

/**
 * @brief Release a block.
 * @param memory Block from tb_malloc(), tb_calloc(), tb_realloc(), or their
 * `tb_try_*` and array forms; NULL is ignored. Not a block from
 * tb_aligned_alloc(). The pointer is invalid afterwards.
 */
TB_API void tb_free(void *memory);

/**
 * @brief Allocate one zero-filled object: `player *hero = TB_NEW(player);`.
 *
 * Use it only for types whose all-zero representation is a valid state.
 *
 * @param type Object type.
 * @return Pointer to the object; never NULL. Release it with tb_free().
 */
#define TB_NEW(type) ((type *)tb_calloc(1, sizeof(type)))

/**
 * @brief Allocate a zero-filled array of objects: `TB_NEW_ARRAY(vertex, count)`.
 * @param type Element type.
 * @param count Number of elements, as a `size_t`.
 * @return Pointer to the first element; never NULL. Release it with tb_free().
 */
#define TB_NEW_ARRAY(type, count) ((type *)tb_calloc((count), sizeof(type)))

/* -------------------------------------------------------------------------
 * Allocation for callers that can recover.
 * ------------------------------------------------------------------------- */

/**
 * @brief Allocate uninitialized memory, or report that it is not available.
 * @param size Bytes to allocate; 0 is served as 1.
 * @return A block as from tb_malloc(), or NULL when memory is exhausted.
 */
TB_API void *tb_try_malloc(size_t size);

/**
 * @brief Allocate a zero-filled array, or report that it is not available.
 * @param count Number of elements; 0 is served as 1.
 * @param size Bytes per element; 0 is served as 1.
 * @return A block as from tb_calloc(), or NULL when memory is exhausted or
 * `count * size` overflows `size_t`.
 */
TB_API void *tb_try_calloc(size_t count, size_t size);

/**
 * @brief Change the size of a block, or report that it cannot be done.
 * @param memory Block as for tb_realloc(), or NULL.
 * @param size New size in bytes; 0 is served as 1.
 * @return The resized block, or NULL when memory is exhausted. On NULL the
 * original block is untouched and still owned by the caller.
 */
TB_API void *tb_try_realloc(void *memory, size_t size);

/**
 * @brief Change the size of an array, or report that it cannot be done.
 * @param memory Block as for tb_realloc(), or NULL.
 * @param count New number of elements; 0 is served as 1.
 * @param size Bytes per element; 0 is served as 1.
 * @return The resized block, or NULL when memory is exhausted or
 * `count * size` overflows `size_t`. On NULL the original block is untouched.
 */
TB_API void *tb_try_realloc_array(void *memory, size_t count, size_t size);

/* -------------------------------------------------------------------------
 * Aligned allocation, for SIMD types and hardware that demands it.
 * ------------------------------------------------------------------------- */

/**
 * @brief Allocate uninitialized memory at a chosen alignment.
 * @param alignment Required alignment in bytes; a power of two.
 * @param size Bytes to allocate; 0 is served as 1.
 * @return A block whose address is a multiple of `alignment`; never NULL.
 * Release it with tb_aligned_free(), never with tb_free(); it cannot be resized.
 * @par Errors Exhaustion, or a size too large to pad for the alignment, calls
 * the failure handler and ends the process.
 */
TB_API void *tb_aligned_alloc(size_t alignment, size_t size);

/**
 * @brief Allocate aligned memory, or report that it is not available.
 * @param alignment Required alignment in bytes; a power of two.
 * @param size Bytes to allocate; 0 is served as 1.
 * @return A block as from tb_aligned_alloc(), or NULL when memory is exhausted
 * or the size is too large to pad for the alignment.
 */
TB_API void *tb_try_aligned_alloc(size_t alignment, size_t size);

/**
 * @brief Release a block from tb_aligned_alloc() or tb_try_aligned_alloc().
 * @param memory Aligned block; NULL is ignored.
 */
TB_API void tb_aligned_free(void *memory);

/* -------------------------------------------------------------------------
 * Accounting.
 * ------------------------------------------------------------------------- */

/**
 * @brief Number of blocks currently allocated and not yet freed.
 *
 * A count that is higher at shutdown than at startup is a leak. Tests compare
 * it before and after each case.
 *
 * @par Threading Thread-safe. With other threads allocating, the value is a
 * snapshot.
 */
TB_API size_t tb_memory_allocation_count(void);

/* -------------------------------------------------------------------------
 * Replacing the underlying memory functions.
 * ------------------------------------------------------------------------- */

/**
 * @brief Allocate `size` bytes, aligned for any object type, or return NULL.
 * The engine never passes 0.
 */
typedef void *tb_memory_malloc_function(size_t size);

/**
 * @brief Allocate `count * size` zero-filled bytes, or return NULL. The engine
 * never passes 0 and has already checked that the product does not overflow.
 */
typedef void *tb_memory_calloc_function(size_t count, size_t size);

/**
 * @brief Resize a block to `size` bytes, preserving its contents, or return
 * NULL and leave it untouched. The engine never passes NULL or 0.
 */
typedef void *tb_memory_realloc_function(void *memory, size_t size);

/** @brief Release a block. The engine never passes NULL. */
typedef void tb_memory_free_function(void *memory);

/**
 * @brief The four functions every allocation in the engine goes through.
 *
 * They must work together as one allocator and be safe to call from any
 * thread. A block from one of them is resized and released only by the others.
 */
typedef struct tb_memory_functions
{
    tb_memory_malloc_function *malloc_function;   /**< Allocates uninitialized memory. */
    tb_memory_calloc_function *calloc_function;   /**< Allocates zero-filled memory. */
    tb_memory_realloc_function *realloc_function; /**< Resizes a block. */
    tb_memory_free_function *free_function;       /**< Releases a block. */
} tb_memory_functions;

/**
 * @brief Get the engine's built-in memory functions, which use the C library.
 *
 * A replacement allocator can call these to do the actual work, for example
 * to add tracking around them.
 *
 * @param[out] functions Required non-null; receives the four functions.
 * @par Threading Thread-safe.
 */
TB_API void tb_memory_get_original_functions(tb_memory_functions *functions);

/**
 * @brief Get the memory functions currently in use.
 * @param[out] functions Required non-null; receives the four functions.
 * @par Threading Stage-owned, like tb_memory_set_functions().
 */
TB_API void tb_memory_get_functions(tb_memory_functions *functions);

/**
 * @brief Replace the memory functions used by every allocation in the engine.
 *
 * Call it at startup, before the engine allocates. A block must be released by
 * the allocator that produced it, so the replacement is refused while any
 * block is still allocated.
 *
 * @param functions Required non-null; all four members must be non-null. The
 * structure is copied.
 * @return true when the functions were installed. false, changing nothing,
 * when a member is NULL or tb_memory_allocation_count() is not 0.
 * @par Threading Stage-owned: call during single-threaded startup or teardown.
 */
TB_API bool tb_memory_set_functions(const tb_memory_functions *functions);

/* -------------------------------------------------------------------------
 * Running out of memory.
 * ------------------------------------------------------------------------- */

/** @brief The request that a fail-fast allocation function could not satisfy. */
typedef struct tb_memory_failure
{
    size_t count;     /**< Number of elements requested; 1 for a plain allocation. */
    size_t size;      /**< Bytes per element. `count * size` may overflow `size_t`. */
    size_t alignment; /**< Requested alignment, or 0 for the default alignment. */
} tb_memory_failure;

/**
 * @brief Receives a failed fail-fast allocation in place of the default report.
 * @param failure Borrowed description, valid for the duration of the call.
 * @param context The pointer registered with tb_memory_set_failure_handler().
 * @par Errors A handler must not return; the caller was promised memory. If it
 * returns, the process ends.
 */
typedef void tb_memory_failure_handler(const tb_memory_failure *failure, void *context);

/**
 * @brief Replace what happens when a fail-fast allocation cannot be satisfied.
 *
 * The default writes the request to stderr and ends the process with
 * tb_platform_abort(). The `tb_try_*` functions never call the handler.
 *
 * @param handler Handler to install, or NULL to restore the default.
 * @param context Passed to every handler call; may be NULL. The caller keeps
 * ownership and must keep it valid while the handler is installed.
 * @par Threading Stage-owned: call during single-threaded startup.
 */
TB_API void tb_memory_set_failure_handler(tb_memory_failure_handler *handler, void *context);

#endif
