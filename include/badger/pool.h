/** @file pool.h
 * @brief A pool: objects of one size, allocated and freed one at a time in
 * constant time.
 *
 * A pool holds blocks of equal slots and hands them out one object at a time.
 * Freeing an object puts its slot on a list that the next allocation takes
 * from, so both cost a few instructions, whatever the order, and the memory of
 * freed objects is reused instead of fragmenting the heap. Use a pool for many
 * objects of one type that come and go individually: entities' components,
 * jobs, asset records, nodes of a tree. Objects that all die together are
 * better served by an arena (arena.h), which needs no freeing at all.
 *
 * @code
 * const tb_pool_options options = {.object_size = sizeof(particle),
 *                                   .alignment = _Alignof(particle)};
 * tb_pool *const particles = tb_pool_create(&options);
 *
 * particle *const spark = tb_pool_alloc(particles);
 * *spark = (particle){.position = origin, .life = 1.0f};
 * ...
 * tb_pool_free(particles, spark);
 * tb_pool_destroy(particles);
 * @endcode
 *
 * **Growth.** A pool takes a block of slots from its allocator when every slot
 * is in use, and keeps its blocks until it is destroyed: a pool that has grown
 * to its working size stops allocating. An object never moves, so a pointer to
 * it is valid until the object is freed, the pool is reset, or the pool is
 * destroyed. An object limit, when given, caps how many objects are live at
 * once.
 *
 * **Failing fast is the default**, as in memory.h. tb_pool_alloc() never
 * returns NULL: when the pool is at its limit or its allocator cannot supply a
 * block, the memory failure handler is called and the process ends.
 * tb_pool_try_alloc() returns NULL for callers that can recover.
 *
 * **Finding mistakes.** In builds with assertions, every slot carries a small
 * header that records its pool and whether it is in use, so freeing an object
 * twice, or freeing what did not come from the pool, fails an assertion; and
 * every object is filled with #TB_POOL_FILL_BYTE as it is handed out, so one
 * read before it is initialized shows a recognizable value. Under
 * AddressSanitizer, the bytes of every slot that is not in use are poisoned,
 * and touching a freed object is reported as `use-after-poison`.
 *
 * @par Threading
 * Externally synchronized: a pool has no lock, and the caller serializes every
 * call that names the same pool. Builds with assertions detect two threads
 * using one pool at once. Different pools are independent.
 */
#ifndef TB_POOL_H
#define TB_POOL_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stddef.h>

/**
 * @brief A pool. Created by tb_pool_create(); owned by the caller; released
 * by tb_pool_destroy().
 */
typedef struct tb_pool tb_pool;

/** @brief Bytes of slots in a block when the options leave it to the pool: 16 KiB. */
#define TB_POOL_DEFAULT_BLOCK_SIZE ((size_t)16 * 1024)

/** @brief The fewest objects in a block, whatever their size. */
#define TB_POOL_MINIMUM_BLOCK_OBJECTS ((size_t)8)

/**
 * @brief Value of every byte of an object when it is handed out, in builds
 * with assertions enabled (see tb_assert_library_enabled()). Other builds
 * leave the bytes as they were.
 */
#define TB_POOL_FILL_BYTE 0xDD

/**
 * @brief What a pool holds and where its memory comes from. Zero-initialize
 * it and set at least `object_size`.
 */
typedef struct tb_pool_options
{
    /** Bytes of one object; at least 1. */
    size_t object_size;
    /** Alignment of every object: a power of two, or 0 for the alignment
     * tb_malloc() guarantees, which suits any type without an alignment of
     * its own. */
    size_t alignment;
    /** Objects in each block the pool takes from its allocator; 0 for as
     * many as fit #TB_POOL_DEFAULT_BLOCK_SIZE, and never fewer than
     * #TB_POOL_MINIMUM_BLOCK_OBJECTS. */
    size_t objects_per_block;
    /** The most objects that may be live at once; 0 for no limit. */
    size_t object_limit;
    /** Where the pool's own record and its blocks come from; NULL for the
     * engine's heap. Copied: the structure need not outlive the call, but
     * what its context points to must outlive the pool. */
    const tb_allocator *allocator;
} tb_pool_options;

/**
 * @brief Create an empty pool. It takes its first block when the first object
 * is allocated.
 * @param options Required non-null; see tb_pool_options for the valid values.
 * @return The pool; never NULL. The caller owns it and releases it with
 * tb_pool_destroy().
 * @par Errors Exhaustion of the allocator, or an object size so large that a
 * block's size overflows `size_t`, calls the memory failure handler and ends
 * the process.
 */
TB_API tb_pool *tb_pool_create(const tb_pool_options *options);

/**
 * @brief Destroy a pool, every object in it, and every block it holds.
 *
 * Objects still live are released with it; the pool runs no code for them,
 * so an object that owns something else must have that released first.
 *
 * @param pool Pool to destroy; NULL is ignored. The pointer, and every
 * pointer to an object from the pool, is invalid afterwards.
 */
TB_API void tb_pool_destroy(tb_pool *pool);

/**
 * @brief Allocate an object.
 * @param pool Required non-null.
 * @return Uninitialized memory for one object, aligned as the options asked;
 * never NULL. It belongs to the pool: release it with tb_pool_free(), or with
 * the pool.
 * @par Errors A pool at its object limit, or an allocator that cannot supply
 * a new block, calls the memory failure handler and ends the process.
 */
TB_API void *tb_pool_alloc(tb_pool *pool);

/**
 * @brief Allocate an object, or report that none can be had.
 * @param pool Required non-null.
 * @return An object as from tb_pool_alloc(), or NULL when the pool is at its
 * object limit or its allocator cannot supply a block. On NULL the pool is
 * unchanged and remains usable.
 */
TB_API void *tb_pool_try_alloc(tb_pool *pool);

/**
 * @brief Free an object, making its slot available to the next allocation.
 * @param pool Required non-null.
 * @param object An object that this pool handed out and that is still live,
 * or NULL, which is ignored. Freeing an object twice, or one from somewhere
 * else, is a contract violation that builds with assertions detect. The
 * pointer is invalid afterwards.
 */
TB_API void tb_pool_free(tb_pool *pool, void *object);

/**
 * @brief Free every object in a pool at once, and keep its blocks for reuse.
 * @param pool Required non-null. Every pointer to an object from the pool is
 * invalid afterwards.
 */
TB_API void tb_pool_reset(tb_pool *pool);

/**
 * @brief The number of live objects: allocated and not yet freed.
 * @param pool Required non-null.
 */
TB_API size_t tb_pool_count(const tb_pool *pool);

/**
 * @brief The number of objects the pool's blocks hold, live or not. It rises
 * as blocks are added and never falls until the pool is destroyed.
 * @param pool Required non-null.
 */
TB_API size_t tb_pool_capacity(const tb_pool *pool);

#endif
