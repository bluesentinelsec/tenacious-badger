/* An atom is a header with the hash and the length, followed by the bytes and
 * a terminator. The pointer the caller holds is to the bytes, so the header is
 * found just before it.
 *
 * Atoms are carved one after another from blocks that the table takes from
 * its allocator, so a table of many atoms makes few allocations and gives
 * them back in few steps. The first block is small and each one after it is
 * twice as large, up to a limit: a table with a handful of names takes little
 * and a table with a million takes few blocks. An atom much longer than the
 * rest gets a block of its own.
 *
 * The table is an array of pointers to the headers, searched by open
 * addressing: an atom goes in the first empty slot at or after the one its
 * hash names. Atoms are never removed, so a search ends at the first empty
 * slot, and the array is kept at most three quarters full so that one is
 * always near.
 *
 * A shared table is searched without a lock. That works because nothing a
 * search reads is ever changed or released while the table lives: an atom is
 * complete before the pointer to it is written into a slot, a slot that holds
 * a pointer keeps it, and an array that the table has outgrown is kept until
 * the table is destroyed, for the threads that are still searching it. Only
 * adding an atom takes the table's lock, and it searches again first, in the
 * newest array, for an atom that another thread added meanwhile. */
#include "badger/atom.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/hash.h"
#include "badger/platform.h"
#include "badger/size.h"
#include "badger/str.h"
#include "badger/thread.h"
#include "memory_internal.h"

#include <stdbool.h>
#include <string.h>

/* The first array of slots. A power of two, like every later one, so that a
 * hash is reduced to a slot by masking. */
#define TB_ATOM_FIRST_CAPACITY ((size_t)16)

/* The array grows when more than this fraction of it would be in use. */
#define TB_ATOM_LOAD_NUMERATOR ((size_t)3)
#define TB_ATOM_LOAD_DENOMINATOR ((size_t)4)

/* Room for the decimal digits of any long long, its sign, and a terminator. */
#define TB_ATOM_INT_CAPACITY 32

/* Bytes of the first block that atoms share, and of the largest. Each block
 * is twice the one before until the largest is reached. */
#define TB_ATOM_FIRST_BLOCK_SIZE ((size_t)1024)
#define TB_ATOM_LARGEST_BLOCK_SIZE ((size_t)65536)

/* An atom that would take more than this fraction of the next shared block
 * gets a block of its own. That bounds what is left unused in a block that is
 * given up for a larger atom to the same fraction of the block after it. */
#define TB_ATOM_OWN_BLOCK_DIVISOR ((size_t)4)

typedef struct tb_atom_header
{
    uint64_t hash; /* tb_hash_bytes() of the bytes. */
    size_t length; /* Number of bytes, terminator not counted. */
} tb_atom_header;

typedef struct tb_atom_block
{
    struct tb_atom_block *next; /* The block taken before this one, or NULL. */
    size_t size;                /* Bytes in the block, this structure included. */
} tb_atom_block;

/* Atoms follow the structure in a block, so its size must leave them aligned. */
TB_STATIC_ASSERT(sizeof(tb_atom_block) % _Alignof(tb_atom_header) == 0,
                 "the atoms of a block must be aligned after its structure");
TB_STATIC_ASSERT(_Alignof(tb_atom_block) <= _Alignof(tb_atom_header),
                 "a block is allocated with the alignment of an atom");

/* An array of slots, which follow this structure in memory. A slot holds a
 * pointer to a header, or NULL while it is empty. */
typedef struct tb_atom_slots
{
    /* The array that this one replaced, in a shared table, which keeps it for
     * the threads that may still be searching it; NULL otherwise. */
    struct tb_atom_slots *outgrown;
    size_t capacity; /* Number of slots: a power of two. */
    size_t size;     /* Bytes in the array, this structure included. */
} tb_atom_slots;

/* The slots follow the structure, so its size must leave them aligned. */
TB_STATIC_ASSERT(sizeof(tb_atom_slots) % _Alignof(tb_platform_atomic_pointer) == 0,
                 "the slots of an array must be aligned after its structure");
TB_STATIC_ASSERT(_Alignof(tb_atom_slots) >= _Alignof(tb_platform_atomic_pointer),
                 "an array of slots is allocated with the alignment of a slot");

struct tb_atom_table
{
    tb_allocator allocator; /* Supplies the table, the slots, and the blocks. */
    /* The newest array of slots, a tb_atom_slots, or NULL before the first
     * atom. Read by any thread that searches a shared table. */
    tb_platform_atomic_pointer slots;
    tb_platform_atomic_size count; /* Number of atoms. */
    /* In a shared table, held by the thread that adds an atom. It guards
     * everything below, and the writing of slots. */
    tb_mutex lock;
    bool is_shared;
    tb_atom_block *blocks;  /* Every block, the latest first. */
    unsigned char *cursor;  /* Next unused byte of the block being filled. */
    size_t remaining;       /* Unused bytes of the block being filled. */
    size_t next_block_size; /* Size of the next block that atoms share. */
};

/* ---------------------------------------------------------------------------
 * Atoms and their headers.
 * ------------------------------------------------------------------------- */

static const char *tb_atom_of_header(const tb_atom_header *header)
{
    return (const char *)(header + 1);
}

static const tb_atom_header *tb_atom_header_of(const char *atom)
{
    return (const tb_atom_header *)(const void *)(atom - sizeof(tb_atom_header));
}

/* Bytes that an atom of a length takes in a block: its header, its bytes, and
 * a terminator, rounded up so that the atom after it is aligned. Fails fast
 * when the size cannot be expressed. */
static size_t tb_atom_size(size_t length)
{
    const size_t alignment = _Alignof(tb_atom_header);
    size_t size = {0};

    if (!tb_size_add(length, sizeof(tb_atom_header) + 1 + (alignment - 1), &size))
    {
        tb_memory_fail(1, length, alignment);
    }
    return size - size % alignment;
}

size_t tb_atom_length(const char *atom)
{
    TB_ASSERT(atom != NULL);
    return tb_atom_header_of(atom)->length;
}

uint64_t tb_atom_hash(const char *atom)
{
    TB_ASSERT(atom != NULL);
    return tb_atom_header_of(atom)->hash;
}

/* ---------------------------------------------------------------------------
 * Blocks.
 * ------------------------------------------------------------------------- */

/* Takes a block of `size` bytes from the allocator and adds it to the table's
 * list. Fails fast when the allocator cannot supply it. */
static tb_atom_block *tb_atom_block_create(tb_atom_table *table, size_t size)
{
    tb_atom_block *const block =
        tb_allocator_alloc(&table->allocator, size, _Alignof(tb_atom_header));

    block->size = size;
    block->next = table->blocks;
    table->blocks = block;
    return block;
}

/* Room for the header and the bytes of a new atom, uninitialized. */
static tb_atom_header *tb_atom_allocate(tb_atom_table *table, size_t length)
{
    const size_t size = tb_atom_size(length);
    tb_atom_header *header = NULL;

    if (size > table->remaining && size > table->next_block_size / TB_ATOM_OWN_BLOCK_DIVISOR)
    {
        /* A long atom gets a block of exactly its size, and the block being
         * filled carries on taking the atoms after it. */
        size_t block_size = {0};

        if (!tb_size_add(size, sizeof(tb_atom_block), &block_size))
        {
            tb_memory_fail(1, size, _Alignof(tb_atom_header));
        }
        return (tb_atom_header *)(void *)(tb_atom_block_create(table, block_size) + 1);
    }
    if (size > table->remaining)
    {
        /* The block being filled has no room for this atom: what is left of
         * it is given up for a new block, twice as large up to the limit. */
        tb_atom_block *const block = tb_atom_block_create(table, table->next_block_size);

        table->cursor = (unsigned char *)(block + 1);
        table->remaining = table->next_block_size - sizeof(*block);
        if (table->next_block_size < TB_ATOM_LARGEST_BLOCK_SIZE)
        {
            table->next_block_size *= 2;
        }
    }
    header = (tb_atom_header *)(void *)table->cursor;
    table->cursor += size;
    table->remaining -= size;
    return header;
}

/* ---------------------------------------------------------------------------
 * Slots.
 * ------------------------------------------------------------------------- */

static tb_platform_atomic_pointer *tb_atom_slot_at(tb_atom_slots *slots, size_t index)
{
    return (tb_platform_atomic_pointer *)(void *)(slots + 1) + index;
}

/* Looks for the atom with these bytes in an array, which has at least one
 * empty slot. Returns its header, or NULL when it is not there, in which
 * case `*empty_index` is the slot where it would go. */
static tb_atom_header *tb_atom_search(tb_atom_slots *slots, const char *bytes, size_t length,
                                      uint64_t hash, size_t *empty_index)
{
    const size_t mask = slots->capacity - 1;
    size_t index = (size_t)hash & mask;

    for (;;)
    {
        tb_atom_header *const header =
            tb_platform_atomic_pointer_load(tb_atom_slot_at(slots, index));

        if (header == NULL)
        {
            *empty_index = index;
            return NULL;
        }
        /* The hash and the length rule out nearly every other atom before
         * any bytes are compared. */
        if (header->hash == hash && header->length == length &&
            (length == 0 || memcmp(tb_atom_of_header(header), bytes, length) == 0))
        {
            return header;
        }
        index = (index + 1) & mask;
    }
}

/* Whether an array can take one more atom than `count`. With no array there
 * is no room. */
static bool tb_atom_has_room(const tb_atom_slots *slots, size_t count)
{
    if (slots == NULL)
    {
        return false;
    }
    /* Compared as (count + 1) / capacity <= 3 / 4 without dividing. The
     * products cannot overflow: there is a pointer of several bytes for
     * every slot. */
    return (count + 1) * TB_ATOM_LOAD_DENOMINATOR <= slots->capacity * TB_ATOM_LOAD_NUMERATOR;
}

static void tb_atom_slots_destroy(const tb_allocator *allocator, tb_atom_slots *slots)
{
    tb_allocator_free(allocator, slots, slots->size, _Alignof(tb_atom_slots));
}

/* Replaces the array of slots with one of twice the capacity, with every
 * atom in its new place, and returns it. In a shared table the caller holds
 * the lock. */
static tb_atom_slots *tb_atom_grow(tb_atom_table *table, tb_atom_slots *old_slots)
{
    tb_atom_slots *slots = NULL;
    size_t capacity = TB_ATOM_FIRST_CAPACITY;
    size_t size = {0};

    if (old_slots != NULL && !tb_size_multiply(old_slots->capacity, 2, &capacity))
    {
        tb_memory_fail(old_slots->capacity, sizeof(tb_platform_atomic_pointer),
                       _Alignof(tb_atom_slots));
    }
    if (!tb_size_multiply(capacity, sizeof(tb_platform_atomic_pointer), &size) ||
        !tb_size_add(size, sizeof(tb_atom_slots), &size))
    {
        tb_memory_fail(capacity, sizeof(tb_platform_atomic_pointer), _Alignof(tb_atom_slots));
    }
    /* Zeroed: every slot is empty. */
    slots = tb_allocator_calloc(&table->allocator, 1, size, _Alignof(tb_atom_slots));
    slots->capacity = capacity;
    slots->size = size;
    for (size_t index = 0; old_slots != NULL && index < old_slots->capacity; index++)
    {
        tb_atom_header *const header =
            tb_platform_atomic_pointer_load(tb_atom_slot_at(old_slots, index));
        size_t new_index = {0};

        if (header != NULL)
        {
            (void)tb_atom_search(slots, tb_atom_of_header(header), header->length, header->hash,
                                 &new_index);
            /* Nothing can see the new array yet. */
            tb_atom_slot_at(slots, new_index)->value = header;
        }
    }
    if (table->is_shared)
    {
        /* Kept: a thread may be in the middle of searching the old array. */
        slots->outgrown = old_slots;
    }
    /* Published whole. A thread that searches from now on finds every atom
     * that the old array had. */
    tb_platform_atomic_pointer_store(&table->slots, slots);
    if (!table->is_shared && old_slots != NULL)
    {
        tb_atom_slots_destroy(&table->allocator, old_slots);
    }
    return slots;
}

/* ---------------------------------------------------------------------------
 * Tables.
 * ------------------------------------------------------------------------- */

tb_atom_table *tb_atom_table_create(const tb_allocator *allocator)
{
    tb_atom_table *const table = TB_ALLOCATOR_NEW(allocator, tb_atom_table);

    table->allocator = tb_allocator_resolve(allocator);
    table->next_block_size = TB_ATOM_FIRST_BLOCK_SIZE;
    return table;
}

tb_atom_table *tb_atom_table_create_shared(const tb_allocator *allocator)
{
    tb_atom_table *const table = tb_atom_table_create(allocator);

    table->is_shared = true;
    return table;
}

void tb_atom_table_destroy(tb_atom_table *table)
{
    tb_allocator allocator = {0};
    tb_atom_block *block = NULL;
    tb_atom_slots *slots = NULL;

    if (table == NULL)
    {
        return;
    }
    allocator = table->allocator;
    block = table->blocks;
    while (block != NULL)
    {
        tb_atom_block *const next = block->next;

        tb_allocator_free(&allocator, block, block->size, _Alignof(tb_atom_header));
        block = next;
    }
    /* The newest array, and in a shared table every one before it. */
    slots = tb_platform_atomic_pointer_load(&table->slots);
    while (slots != NULL)
    {
        tb_atom_slots *const outgrown = slots->outgrown;

        tb_atom_slots_destroy(&allocator, slots);
        slots = outgrown;
    }
    TB_ALLOCATOR_DELETE(&allocator, tb_atom_table, table);
}

size_t tb_atom_table_count(const tb_atom_table *table)
{
    TB_ASSERT(table != NULL);
    return tb_platform_atomic_size_load(&table->count);
}

/* ---------------------------------------------------------------------------
 * Interning.
 * ------------------------------------------------------------------------- */

const char *tb_atom_find(const tb_atom_table *table, const char *bytes, size_t length)
{
    const tb_atom_header *header = NULL;
    tb_atom_slots *slots = NULL;
    size_t empty_index = {0};

    TB_ASSERT(table != NULL);
    TB_ASSERT(bytes != NULL || length == 0);
    slots = tb_platform_atomic_pointer_load(&table->slots);
    if (slots == NULL)
    {
        return NULL;
    }
    header = tb_atom_search(slots, bytes, length, tb_hash_bytes(bytes, length), &empty_index);
    if (header == NULL)
    {
        return NULL;
    }
    return tb_atom_of_header(header);
}

/* Looks, in a shared table whose lock the caller holds, for an atom that
 * another thread may have added since the caller looked without the lock.
 * `*slots` becomes the newest array, and `*empty_index` the slot in it where
 * the atom would go. Returns the header, or NULL when it is still not there. */
static tb_atom_header *tb_atom_search_again(tb_atom_table *table, tb_atom_slots **slots,
                                            const char *bytes, size_t length, uint64_t hash,
                                            size_t *empty_index)
{
    *slots = tb_platform_atomic_pointer_load(&table->slots);
    if (*slots == NULL)
    {
        return NULL;
    }
    return tb_atom_search(*slots, bytes, length, hash, empty_index);
}

const char *tb_atom_new(tb_atom_table *table, const char *bytes, size_t length)
{
    tb_atom_header *header = NULL;
    tb_atom_slots *slots = NULL;
    char *atom = NULL;
    size_t empty_index = {0};
    uint64_t hash = {0};

    TB_ASSERT(table != NULL);
    TB_ASSERT(bytes != NULL || length == 0);
    /* The bytes are hashed once, and one search finds either their atom or
     * the empty slot where a new atom goes. An atom that exists is found
     * without the lock, in a shared table as in any other. */
    hash = tb_hash_bytes(bytes, length);
    slots = tb_platform_atomic_pointer_load(&table->slots);
    if (slots != NULL)
    {
        header = tb_atom_search(slots, bytes, length, hash, &empty_index);
    }
    if (header != NULL)
    {
        return tb_atom_of_header(header);
    }
    if (table->is_shared)
    {
        /* Only one thread adds at a time, and it looks again first. */
        tb_mutex_lock(&table->lock);
        header = tb_atom_search_again(table, &slots, bytes, length, hash, &empty_index);
        if (header != NULL)
        {
            tb_mutex_unlock(&table->lock);
            return tb_atom_of_header(header);
        }
    }
    /* Room in the table first: if the atom then cannot be allocated, the
     * table is larger than it needs to be and otherwise unchanged. A table
     * that grew has a new place for the atom. */
    if (!tb_atom_has_room(slots, tb_platform_atomic_size_load(&table->count)))
    {
        slots = tb_atom_grow(table, slots);
        (void)tb_atom_search(slots, bytes, length, hash, &empty_index);
    }
    header = tb_atom_allocate(table, length);
    header->hash = hash;
    header->length = length;
    atom = (char *)(header + 1);
    if (length > 0)
    {
        memcpy(atom, bytes, length);
    }
    atom[length] = '\0';
    if (!table->is_shared)
    {
        /* No other thread reads this table, so the slot and the count are
         * written plainly, which costs a third of an atomic write. */
        tb_atom_slot_at(slots, empty_index)->value = header;
        table->count.value++;
        return atom;
    }
    /* Written as one step, after the atom is complete: from here any thread
     * that reads the slot may read the atom. */
    tb_platform_atomic_pointer_store(tb_atom_slot_at(slots, empty_index), header);
    (void)tb_platform_atomic_size_add(&table->count, 1);
    tb_mutex_unlock(&table->lock);
    return atom;
}

const char *tb_atom_string(tb_atom_table *table, const char *string)
{
    TB_ASSERT(string != NULL);
    return tb_atom_new(table, string, tb_str_length(string));
}

const char *tb_atom_int(tb_atom_table *table, long long value)
{
    char digits[TB_ATOM_INT_CAPACITY] = {0};
    const size_t length = tb_fmt_buffer(digits, sizeof(digits), "%lld", value);

    return tb_atom_new(table, digits, length);
}
