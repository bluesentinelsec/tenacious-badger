/* An arena is a chain of chunks and a position in one of them.
 *
 *   first chunk           a later chunk         kept from before a reset
 *   [arena|objects...] -> [header|objects.  ] -> [header|unused.........]
 *                                        ^ cursor, in the current chunk
 *
 * Chunks before the current one are full or abandoned, the current one is
 * filled from its cursor, and chunks after it hold no objects. The first chunk
 * shares its memory with the arena itself, so a small arena is one allocation
 * and a fixed arena needs none. */
#include "badger/arena.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/size.h"
#include "memory_internal.h"
#include "sanitizer.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* 1 when unused arena memory is filled with TB_ARENA_FILL_BYTE. The fill costs
 * a pass over every chunk that is created or released, so it follows the
 * assertions: on in development builds, off in optimized ones. */
#define TB_ARENA_FILLS_UNUSED_MEMORY TB_ASSERT_ENABLED

typedef struct tb_arena_chunk
{
    struct tb_arena_chunk *next; /* Next chunk in the chain, or NULL. */
    unsigned char *begin;        /* First byte available to objects. */
    unsigned char *end;          /* One past the last byte available to objects. */
} tb_arena_chunk;

struct tb_arena
{
    tb_arena_chunk first;    /* Its objects follow this structure in memory. */
    tb_arena_chunk *current; /* The chunk being filled; never NULL. */
    unsigned char *cursor;   /* Next unused byte of the current chunk. */
    size_t chunk_size;       /* Bytes allocated for each further chunk. */
    size_t capacity;         /* Bytes available to objects across every chunk. */
    bool is_growable;        /* false for an arena in a caller's buffer. */
};

/* A fixed arena places this structure at the first suitably aligned address of
 * a buffer that may start anywhere. */
TB_STATIC_ASSERT(sizeof(tb_arena) + _Alignof(tb_arena) <= TB_ARENA_MINIMUM_SIZE,
                 "TB_ARENA_MINIMUM_SIZE must leave room for the arena's bookkeeping");

/* ---------------------------------------------------------------------------
 * Positions and sizes.
 * ------------------------------------------------------------------------- */

static bool tb_arena_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static size_t tb_arena_at_least_one(size_t value)
{
    if (value == 0)
    {
        return 1;
    }
    return value;
}

/* Bytes to skip from `address` to reach the next multiple of `alignment`. */
static size_t tb_arena_padding(const unsigned char *address, size_t alignment)
{
    const size_t misalignment = (size_t)((uintptr_t)address & (uintptr_t)(alignment - 1));

    if (misalignment == 0)
    {
        return 0;
    }
    return alignment - misalignment;
}

/* Where an object goes in the unused memory from `begin` to `end`: the first
 * address there with the alignment. NULL when the object does not fit. */
static unsigned char *tb_arena_place(unsigned char *begin, const unsigned char *end, size_t size,
                                     size_t alignment)
{
    const size_t available = (size_t)(end - begin);
    const size_t padding = tb_arena_padding(begin, alignment);

    if (padding > available || size > available - padding)
    {
        return NULL;
    }
    return begin + padding;
}

/* ---------------------------------------------------------------------------
 * Unused memory. Every byte that is not part of a live object is retired: it
 * holds the fill byte where filling is enabled, and is poisoned under
 * AddressSanitizer.
 * ------------------------------------------------------------------------- */

static void tb_arena_retire(unsigned char *begin, const unsigned char *end)
{
    const size_t size = (size_t)(end - begin);

#if TB_ARENA_FILLS_UNUSED_MEMORY
    /* Part of the range may already be poisoned, and poisoned memory cannot
     * be written. */
    tb_sanitizer_unpoison(begin, size);
    memset(begin, TB_ARENA_FILL_BYTE, size);
#endif
    tb_sanitizer_poison(begin, size);
}

/* Retires everything from a position in a chunk up to the arena's cursor,
 * which is in that chunk or in a later one. */
static void tb_arena_retire_from(const tb_arena *arena, const tb_arena_chunk *chunk,
                                 unsigned char *position)
{
    while (chunk != arena->current)
    {
        tb_arena_retire(position, chunk->end);
        chunk = chunk->next;
        position = chunk->begin;
    }
    tb_arena_retire(position, arena->cursor);
}

/* ---------------------------------------------------------------------------
 * Creating and destroying.
 * ------------------------------------------------------------------------- */

/* Sets up an arena whose structure is at the start of memory that runs to
 * `end`. The memory after the structure becomes the first chunk. */
static void tb_arena_initialize(tb_arena *arena, unsigned char *end, size_t chunk_size,
                                bool is_growable)
{
    *arena = (tb_arena){0};
    arena->first.begin = (unsigned char *)arena + sizeof(*arena);
    arena->first.end = end;
    arena->current = &arena->first;
    arena->cursor = arena->first.begin;
    arena->chunk_size = chunk_size;
    arena->capacity = (size_t)(arena->first.end - arena->first.begin);
    arena->is_growable = is_growable;
    tb_arena_retire(arena->first.begin, arena->first.end);
}

/* The chunk size a growing arena uses for the one it was asked for. */
static size_t tb_arena_usable_chunk_size(size_t chunk_size)
{
    if (chunk_size == 0)
    {
        return TB_ARENA_DEFAULT_CHUNK_SIZE;
    }
    if (chunk_size < TB_ARENA_MINIMUM_SIZE)
    {
        return TB_ARENA_MINIMUM_SIZE;
    }
    return chunk_size;
}

tb_arena *tb_arena_try_create(size_t chunk_size)
{
    void *block = NULL;

    chunk_size = tb_arena_usable_chunk_size(chunk_size);
    block = tb_try_malloc(chunk_size);
    if (block == NULL)
    {
        return NULL;
    }
    tb_arena_initialize(block, (unsigned char *)block + chunk_size, chunk_size, true);
    return block;
}

tb_arena *tb_arena_create(size_t chunk_size)
{
    tb_arena *const arena = tb_arena_try_create(chunk_size);

    if (arena == NULL)
    {
        tb_memory_fail(1, tb_arena_usable_chunk_size(chunk_size), 0);
    }
    return arena;
}

tb_arena *tb_arena_create_fixed(void *buffer, size_t size)
{
    unsigned char *const bytes = buffer;
    void *aligned = NULL;

    TB_ASSERT(buffer != NULL);
    TB_ASSERT_MSG(size >= TB_ARENA_MINIMUM_SIZE,
                  "the buffer must hold at least TB_ARENA_MINIMUM_SIZE bytes");
    aligned = bytes + tb_arena_padding(bytes, _Alignof(tb_arena));
    /* A fixed arena never adds a chunk, so it has no chunk size. */
    tb_arena_initialize(aligned, bytes + size, 0, false);
    return aligned;
}

void tb_arena_destroy(tb_arena *arena)
{
    tb_arena_chunk *chunk = NULL;

    if (arena == NULL)
    {
        return;
    }
    /* Whoever gets the memory next, an allocator or the owner of a fixed
     * buffer, must be free to write all of it. */
    chunk = arena->first.next;
    while (chunk != NULL)
    {
        tb_arena_chunk *const next = chunk->next;

        tb_sanitizer_unpoison(chunk->begin, (size_t)(chunk->end - chunk->begin));
        tb_free(chunk);
        chunk = next;
    }
    tb_sanitizer_unpoison(arena->first.begin, (size_t)(arena->first.end - arena->first.begin));
    if (arena->is_growable)
    {
        tb_free(arena);
    }
}

/* ---------------------------------------------------------------------------
 * Allocation.
 * ------------------------------------------------------------------------- */

/* Takes an object from the current chunk, or returns NULL when it does not
 * fit there. */
static void *tb_arena_take(tb_arena *arena, size_t size, size_t alignment)
{
    unsigned char *const memory =
        tb_arena_place(arena->cursor, arena->current->end, size, alignment);

    if (memory == NULL)
    {
        return NULL;
    }
    arena->cursor = memory + size;
    tb_sanitizer_unpoison(memory, size);
    return memory;
}

/* Allocates a chunk that can hold an object of the given size and alignment.
 * The chunk is not linked into the chain yet. Returns NULL when memory is
 * exhausted or the size cannot be expressed. */
static tb_arena_chunk *tb_arena_chunk_create(tb_arena *arena, size_t size, size_t alignment)
{
    tb_arena_chunk *chunk = NULL;
    size_t chunk_size = sizeof(tb_arena_chunk);

    /* Room for the object, plus enough slack to reach its alignment from
     * wherever the chunk happens to start. */
    if (!tb_size_add(chunk_size, size, &chunk_size))
    {
        return NULL;
    }
    if (!tb_size_add(chunk_size, alignment - 1, &chunk_size))
    {
        return NULL;
    }
    if (chunk_size < arena->chunk_size)
    {
        chunk_size = arena->chunk_size;
    }
    chunk = tb_try_malloc(chunk_size);
    if (chunk == NULL)
    {
        return NULL;
    }
    chunk->next = NULL;
    chunk->begin = (unsigned char *)chunk + sizeof(*chunk);
    chunk->end = (unsigned char *)chunk + chunk_size;
    /* Blocks that exist at the same time cannot add up to more than the
     * address space, so this sum cannot overflow. */
    arena->capacity += (size_t)(chunk->end - chunk->begin);
    tb_arena_retire(chunk->begin, chunk->end);
    return chunk;
}

/* Moves the arena on to a chunk that fits the object. It reuses the first
 * chunk after the current one that is large enough, and otherwise allocates
 * one. The chosen chunk is linked just after the chunk it follows in use, so
 * chunks that were skipped stay available for smaller objects. Returns false,
 * changing nothing, when no chunk can be had. */
static bool tb_arena_advance(tb_arena *arena, size_t size, size_t alignment)
{
    tb_arena_chunk *previous = arena->current;
    tb_arena_chunk *chunk = arena->current->next;

    while (chunk != NULL && tb_arena_place(chunk->begin, chunk->end, size, alignment) == NULL)
    {
        previous = chunk;
        chunk = chunk->next;
    }
    if (chunk != NULL)
    {
        previous->next = chunk->next;
    }
    else
    {
        chunk = tb_arena_chunk_create(arena, size, alignment);
    }
    if (chunk == NULL)
    {
        return false;
    }
    chunk->next = arena->current->next;
    arena->current->next = chunk;
    arena->current = chunk;
    arena->cursor = chunk->begin;
    return true;
}

void *tb_arena_try_alloc(tb_arena *arena, size_t size, size_t alignment)
{
    void *memory = NULL;

    TB_ASSERT(arena != NULL);
    TB_ASSERT_MSG(tb_arena_is_power_of_two(alignment), "alignment must be a power of two");
    size = tb_arena_at_least_one(size);
    memory = tb_arena_take(arena, size, alignment);
    if (memory != NULL)
    {
        return memory;
    }
    if (!arena->is_growable)
    {
        return NULL;
    }
    if (!tb_arena_advance(arena, size, alignment))
    {
        return NULL;
    }
    return tb_arena_take(arena, size, alignment);
}

void *tb_arena_try_calloc(tb_arena *arena, size_t count, size_t size, size_t alignment)
{
    void *memory = NULL;
    size_t total_size = {0};

    TB_ASSERT(arena != NULL);
    TB_ASSERT_MSG(tb_arena_is_power_of_two(alignment), "alignment must be a power of two");
    count = tb_arena_at_least_one(count);
    size = tb_arena_at_least_one(size);
    if (!tb_size_multiply(count, size, &total_size))
    {
        return NULL;
    }
    memory = tb_arena_try_alloc(arena, total_size, alignment);
    if (memory == NULL)
    {
        return NULL;
    }
    memset(memory, 0, total_size);
    return memory;
}

void *tb_arena_alloc(tb_arena *arena, size_t size, size_t alignment)
{
    void *const memory = tb_arena_try_alloc(arena, size, alignment);

    if (memory == NULL)
    {
        tb_memory_fail(1, size, alignment);
    }
    return memory;
}

void *tb_arena_calloc(tb_arena *arena, size_t count, size_t size, size_t alignment)
{
    void *const memory = tb_arena_try_calloc(arena, count, size, alignment);

    if (memory == NULL)
    {
        tb_memory_fail(count, size, alignment);
    }
    return memory;
}

/* ---------------------------------------------------------------------------
 * Releasing.
 * ------------------------------------------------------------------------- */

void tb_arena_reset(tb_arena *arena)
{
    TB_ASSERT(arena != NULL);
    tb_arena_retire_from(arena, &arena->first, arena->first.begin);
    arena->current = &arena->first;
    arena->cursor = arena->first.begin;
}

tb_arena_mark tb_arena_get_mark(const tb_arena *arena)
{
    tb_arena_mark mark = {0};

    TB_ASSERT(arena != NULL);
    mark.chunk = arena->current;
    mark.cursor = arena->cursor;
    return mark;
}

/* Whether a mark names a position the arena has reached and not released: in
 * a chunk up to the current one, and not past the cursor in the current one.
 * A mark that fails this was released, or belongs to another arena. */
static bool tb_arena_mark_is_live(const tb_arena *arena, const tb_arena_chunk *mark_chunk,
                                  const unsigned char *mark_cursor)
{
    const tb_arena_chunk *chunk = &arena->first;

    while (chunk != mark_chunk)
    {
        if (chunk == arena->current)
        {
            return false;
        }
        chunk = chunk->next;
    }
    if (chunk != arena->current)
    {
        return true;
    }
    /* Both positions are in the current chunk, so they can be compared. */
    return mark_cursor <= arena->cursor;
}

void tb_arena_rewind(tb_arena *arena, tb_arena_mark mark)
{
    tb_arena_chunk *const mark_chunk = mark.chunk;
    unsigned char *const mark_cursor = mark.cursor;

    TB_ASSERT(arena != NULL);
    TB_ASSERT_MSG(tb_arena_mark_is_live(arena, mark_chunk, mark_cursor),
                  "the mark must come from this arena and must not have been released");
    tb_arena_retire_from(arena, mark_chunk, mark_cursor);
    arena->current = mark_chunk;
    arena->cursor = mark_cursor;
}

/* ---------------------------------------------------------------------------
 * Accounting.
 * ------------------------------------------------------------------------- */

size_t tb_arena_capacity(const tb_arena *arena)
{
    TB_ASSERT(arena != NULL);
    return arena->capacity;
}

size_t tb_arena_remaining(const tb_arena *arena)
{
    TB_ASSERT(arena != NULL);
    return (size_t)(arena->current->end - arena->cursor);
}
