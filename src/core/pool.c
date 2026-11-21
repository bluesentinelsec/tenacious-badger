/* A pool is a list of blocks, each a small header followed by equal slots:
 *
 *   block:  [ next | slot 0 | slot 1 | ... | slot n-1 ]
 *   slot:   [ check header | object .................. ]   with assertions
 *           [ object ................................. ]   without
 *
 * A slot is handed out from one of two places. Freed slots form a list,
 * linked through the first bytes of their objects, and the most recently
 * freed is taken first, while its memory is likely still in the cache. When
 * the list is empty, the next slot that has never been handed out since the
 * pool was created or reset is taken, in block order: a new block is not
 * touched until it is needed, and a reset pool reuses its blocks from the
 * first.
 *
 * In builds with assertions the header records which pool the slot belongs
 * to and whether its object is live, which is how freeing twice or freeing a
 * stranger's pointer is caught. The header is never poisoned for the
 * sanitizer, so that check reads it even after the object is freed. */
#include "badger/pool.h"

#include "badger/assert.h"
#include "badger/size.h"
#include "exclusive.h"
#include "memory_internal.h"
#include "sanitizer.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* 1 when every slot carries a check header and objects are filled as they are
 * handed out. */
#define TB_POOL_CHECKS TB_ASSERT_ENABLED

/* What a failed guard names. */
#define TB_POOL_KIND "pool"

/* The two states a check header records. Unlikely values, so that a pointer
 * to memory that is not a slot is unlikely to pass for one. */
#define TB_POOL_LIVE UINT32_C(0x4C495645)
#define TB_POOL_FREE UINT32_C(0x46524545)

/* A type with the strictest alignment of any ordinary scalar, which is what
 * tb_malloc() guarantees and what an alignment of 0 asks for. */
typedef union tb_pool_scalar {
    long long integer;
    long double real;
    void *pointer;
    void (*function)(void);
} tb_pool_scalar;

typedef struct tb_pool_block
{
    struct tb_pool_block *next;
} tb_pool_block;

#if TB_POOL_CHECKS
typedef struct tb_pool_header
{
    const tb_pool *owner;
    uint32_t state;
} tb_pool_header;
#endif

struct tb_pool
{
    tb_allocator allocator;
    size_t object_size;       /* As the options gave it. */
    size_t alignment;         /* Of every object. */
    size_t object_offset;     /* From the start of a slot to its object. */
    size_t slot_size;         /* From one slot to the next. */
    size_t slots_offset;      /* From the start of a block to its first slot. */
    size_t objects_per_block; /* Slots in each block. */
    size_t block_size;
    size_t block_alignment;
    size_t object_limit; /* 0 for none. */
    size_t count;        /* Live objects. */
    size_t capacity;     /* Slots in every block. */
    tb_pool_block *first_block;
    tb_pool_block *last_block;
    /* The block holding the next slot never handed out, and its index there;
     * NULL when every block has been handed out in full. */
    tb_pool_block *fresh_block;
    size_t fresh_index;
    unsigned char *free_slots; /* The most recently freed slot, or NULL. */
    tb_exclusive guard;
};

static size_t tb_pool_larger(size_t first, size_t second)
{
    if (first > second)
    {
        return first;
    }
    return second;
}

static bool tb_pool_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

/* `value` rounded up to a multiple of `alignment`, a power of two; false when
 * that overflows. */
static bool tb_pool_round_up(size_t value, size_t alignment, size_t *rounded)
{
    size_t sum = 0;

    if (!tb_size_add(value, alignment - 1, &sum))
    {
        return false;
    }
    *rounded = sum & ~(alignment - 1);
    return true;
}

/* Works out where everything goes; false when a size overflows. */
static bool tb_pool_lay_out(tb_pool *pool, size_t requested_per_block)
{
    const size_t object_area = tb_pool_larger(pool->object_size, sizeof(unsigned char *));
    size_t slot_alignment = pool->alignment;
    size_t slot_end = 0;
    size_t slots_size = 0;

#if TB_POOL_CHECKS
    slot_alignment = tb_pool_larger(slot_alignment, _Alignof(tb_pool_header));
    if (!tb_pool_round_up(sizeof(tb_pool_header), pool->alignment, &pool->object_offset))
    {
        return false;
    }
#endif
    if (!tb_size_add(pool->object_offset, object_area, &slot_end) ||
        !tb_pool_round_up(slot_end, slot_alignment, &pool->slot_size) ||
        !tb_pool_round_up(sizeof(tb_pool_block), slot_alignment, &pool->slots_offset))
    {
        return false;
    }
    pool->block_alignment = tb_pool_larger(slot_alignment, _Alignof(tb_pool_block));
    pool->objects_per_block = requested_per_block;
    if (pool->objects_per_block == 0)
    {
        pool->objects_per_block = tb_pool_larger(TB_POOL_DEFAULT_BLOCK_SIZE / pool->slot_size,
                                                 TB_POOL_MINIMUM_BLOCK_OBJECTS);
    }
    return tb_size_multiply(pool->objects_per_block, pool->slot_size, &slots_size) &&
           tb_size_add(pool->slots_offset, slots_size, &pool->block_size);
}

tb_pool *tb_pool_create(const tb_pool_options *options)
{
    tb_pool layout = {0};
    tb_pool *pool = NULL;

    TB_ASSERT(options != NULL);
    TB_ASSERT_MSG(options->object_size >= 1, "object_size >= 1");
    TB_ASSERT_MSG(options->alignment == 0 || tb_pool_is_power_of_two(options->alignment),
                  "the alignment is 0 or a power of two");
    layout.allocator = tb_allocator_resolve(options->allocator);
    layout.object_size = options->object_size;
    layout.alignment = options->alignment;
    if (layout.alignment == 0)
    {
        layout.alignment = _Alignof(tb_pool_scalar);
    }
    layout.object_limit = options->object_limit;
    if (!tb_pool_lay_out(&layout, options->objects_per_block))
    {
        tb_memory_fail(1, options->object_size, layout.alignment);
    }
    pool = tb_allocator_alloc(&layout.allocator, sizeof(tb_pool), _Alignof(tb_pool));
    *pool = layout;
    return pool;
}

static unsigned char *tb_pool_slots_of(const tb_pool *pool, tb_pool_block *block)
{
    return (unsigned char *)block + pool->slots_offset;
}

static void tb_pool_poison_slots(const tb_pool *pool, tb_pool_block *block)
{
    tb_sanitizer_poison(tb_pool_slots_of(pool, block), pool->block_size - pool->slots_offset);
}

void tb_pool_destroy(tb_pool *pool)
{
    tb_pool_block *block = NULL;
    tb_allocator allocator = {0};

    if (pool == NULL)
    {
        return;
    }
    TB_EXCLUSIVE_ENTER(&pool->guard, TB_POOL_KIND);
    allocator = pool->allocator;
    block = pool->first_block;
    while (block != NULL)
    {
        tb_pool_block *const next = block->next;

        /* The allocator gets back exactly what it gave, sanitizer state and all. */
        tb_sanitizer_unpoison(block, pool->block_size);
        tb_allocator_free(&allocator, block, pool->block_size, pool->block_alignment);
        block = next;
    }
    TB_EXCLUSIVE_LEAVE(&pool->guard);
    tb_allocator_free(&allocator, pool, sizeof(tb_pool), _Alignof(tb_pool));
}

/* Adds a block after the last; NULL when the allocator has none to give. */
static tb_pool_block *tb_pool_add_block(tb_pool *pool)
{
    tb_pool_block *const block =
        tb_allocator_try_alloc(&pool->allocator, pool->block_size, pool->block_alignment);

    if (block == NULL)
    {
        return NULL;
    }
    block->next = NULL;
    if (pool->last_block == NULL)
    {
        pool->first_block = block;
    }
    else
    {
        pool->last_block->next = block;
    }
    pool->last_block = block;
    pool->capacity += pool->objects_per_block;
    tb_pool_poison_slots(pool, block);
    return block;
}

static unsigned char *tb_pool_object_of(const tb_pool *pool, unsigned char *slot)
{
    return slot + pool->object_offset;
}

/* The freed slot after `slot`, which is kept in its object's first bytes. */
static unsigned char *tb_pool_next_free(const tb_pool *pool, unsigned char *slot)
{
    unsigned char *next = NULL;

    memcpy(&next, tb_pool_object_of(pool, slot), sizeof(next));
    return next;
}

/* A slot to hand out, unpoisoned; NULL when a block was needed and the
 * allocator had none. */
static unsigned char *tb_pool_take_slot(tb_pool *pool)
{
    unsigned char *slot = pool->free_slots;

    if (slot != NULL)
    {
        tb_sanitizer_unpoison(tb_pool_object_of(pool, slot), pool->slot_size - pool->object_offset);
        pool->free_slots = tb_pool_next_free(pool, slot);
        return slot;
    }
    if (pool->fresh_block == NULL)
    {
        pool->fresh_block = tb_pool_add_block(pool);
        pool->fresh_index = 0;
        if (pool->fresh_block == NULL)
        {
            return NULL;
        }
    }
    slot = tb_pool_slots_of(pool, pool->fresh_block) + pool->fresh_index * pool->slot_size;
    pool->fresh_index++;
    if (pool->fresh_index == pool->objects_per_block)
    {
        pool->fresh_block = pool->fresh_block->next;
        pool->fresh_index = 0;
    }
    tb_sanitizer_unpoison(slot, pool->slot_size);
    return slot;
}

#if TB_POOL_CHECKS
static void tb_pool_set_state(const tb_pool *pool, unsigned char *slot, uint32_t state)
{
    const tb_pool_header header = {.owner = pool, .state = state};

    memcpy(slot, &header, sizeof(header));
}

static tb_pool_header tb_pool_header_of(const unsigned char *slot)
{
    tb_pool_header header = {0};

    memcpy(&header, slot, sizeof(header));
    return header;
}

/* Marks every slot of a block free, so that a pointer kept across a reset
 * fails the check of tb_pool_free(). */
static void tb_pool_forget_block(const tb_pool *pool, tb_pool_block *block)
{
    unsigned char *const slots = tb_pool_slots_of(pool, block);

    for (size_t index = 0; index < pool->objects_per_block; index++)
    {
        unsigned char *const slot = slots + index * pool->slot_size;

        tb_sanitizer_unpoison(slot, sizeof(tb_pool_header));
        tb_pool_set_state(pool, slot, TB_POOL_FREE);
    }
}
#endif

void *tb_pool_try_alloc(tb_pool *pool)
{
    unsigned char *slot = NULL;
    unsigned char *object = NULL;

    TB_ASSERT(pool != NULL);
    TB_EXCLUSIVE_ENTER(&pool->guard, TB_POOL_KIND);
    if (pool->object_limit != 0 && pool->count == pool->object_limit)
    {
        goto done;
    }
    slot = tb_pool_take_slot(pool);
    if (slot == NULL)
    {
        goto done;
    }
    pool->count++;
    object = tb_pool_object_of(pool, slot);
#if TB_POOL_CHECKS
    tb_pool_set_state(pool, slot, TB_POOL_LIVE);
    memset(object, TB_POOL_FILL_BYTE, pool->object_size);
#endif

done:
    TB_EXCLUSIVE_LEAVE(&pool->guard);
    return object;
}

void *tb_pool_alloc(tb_pool *pool)
{
    void *const object = tb_pool_try_alloc(pool);

    if (object == NULL)
    {
        tb_memory_fail(1, pool->object_size, pool->alignment);
    }
    return object;
}

void tb_pool_free(tb_pool *pool, void *object)
{
    unsigned char *slot = NULL;

    TB_ASSERT(pool != NULL);
    if (object == NULL)
    {
        return;
    }
    TB_EXCLUSIVE_ENTER(&pool->guard, TB_POOL_KIND);
    slot = (unsigned char *)object - pool->object_offset;
#if TB_POOL_CHECKS
    TB_ASSERT_MSG(tb_pool_header_of(slot).owner == pool, "the object came from this pool");
    TB_ASSERT_MSG(tb_pool_header_of(slot).state == TB_POOL_LIVE,
                  "the object is live: it was not freed already");
    tb_pool_set_state(pool, slot, TB_POOL_FREE);
#endif
    TB_ASSERT(pool->count > 0);
    memcpy(object, &pool->free_slots, sizeof(pool->free_slots));
    pool->free_slots = slot;
    pool->count--;
    tb_sanitizer_poison(object, pool->slot_size - pool->object_offset);
    TB_EXCLUSIVE_LEAVE(&pool->guard);
}

void tb_pool_reset(tb_pool *pool)
{
    TB_ASSERT(pool != NULL);
    TB_EXCLUSIVE_ENTER(&pool->guard, TB_POOL_KIND);
    for (tb_pool_block *block = pool->first_block; block != NULL; block = block->next)
    {
        tb_pool_poison_slots(pool, block);
#if TB_POOL_CHECKS
        tb_pool_forget_block(pool, block);
#endif
    }
    pool->free_slots = NULL;
    pool->fresh_block = pool->first_block;
    pool->fresh_index = 0;
    pool->count = 0;
    TB_EXCLUSIVE_LEAVE(&pool->guard);
}

size_t tb_pool_count(const tb_pool *pool)
{
    TB_ASSERT(pool != NULL);
    return pool->count;
}

size_t tb_pool_capacity(const tb_pool *pool)
{
    TB_ASSERT(pool != NULL);
    return pool->capacity;
}
