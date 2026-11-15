/* An atom is a block of its own: a header with the hash and the length,
 * followed by the bytes and a terminator. The pointer the caller holds is to
 * the bytes, so the header is found just before it.
 *
 * The table is an array of pointers to those headers, searched by open
 * addressing: an atom goes in the first empty slot at or after the one its
 * hash names. Atoms are never removed, so a search ends at the first empty
 * slot, and the array is kept at most three quarters full so that one is
 * always near. */
#include "badger/atom.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/hash.h"
#include "badger/size.h"
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

typedef struct tb_atom_header
{
    uint64_t hash; /* tb_hash_bytes() of the bytes. */
    size_t length; /* Number of bytes, terminator not counted. */
} tb_atom_header;

struct tb_atom_table
{
    tb_allocator allocator; /* Supplies the table, the slots, and the atoms. */
    tb_atom_header **slots; /* `capacity` slots; an empty one is NULL. */
    size_t capacity;        /* Number of slots: 0 or a power of two. */
    size_t count;           /* Number of atoms. */
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

/* Size in bytes of the block that holds an atom of a length. Fails fast when
 * it cannot be expressed. */
static size_t tb_atom_block_size(size_t length)
{
    size_t size = {0};

    if (!tb_size_add(length, sizeof(tb_atom_header) + 1, &size))
    {
        tb_memory_fail(1, length, _Alignof(tb_atom_header));
    }
    return size;
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
 * Slots.
 * ------------------------------------------------------------------------- */

/* The slot that holds the atom with these bytes, or the empty slot where it
 * would go. The table has slots, and at least one of them is empty. */
static tb_atom_header **tb_atom_slot(const tb_atom_table *table, const char *bytes, size_t length,
                                     uint64_t hash)
{
    const size_t mask = table->capacity - 1;
    size_t index = (size_t)hash & mask;

    for (;;)
    {
        tb_atom_header *const header = table->slots[index];

        if (header == NULL)
        {
            return &table->slots[index];
        }
        /* The hash and the length rule out nearly every other atom before
         * any bytes are compared. */
        if (header->hash == hash && header->length == length &&
            (length == 0 || memcmp(tb_atom_of_header(header), bytes, length) == 0))
        {
            return &table->slots[index];
        }
        index = (index + 1) & mask;
    }
}

/* Whether the table can take one more atom without growing. */
static bool tb_atom_has_room(const tb_atom_table *table)
{
    /* Compared as (count + 1) / capacity <= 3 / 4 without dividing. The
     * products cannot overflow: there is a pointer of several bytes for
     * every slot. */
    return (table->count + 1) * TB_ATOM_LOAD_DENOMINATOR <=
           table->capacity * TB_ATOM_LOAD_NUMERATOR;
}

/* Doubles the array of slots and puts every atom in its new place. */
static void tb_atom_grow(tb_atom_table *table)
{
    tb_atom_header **const old_slots = table->slots;
    const size_t old_capacity = table->capacity;
    size_t capacity = TB_ATOM_FIRST_CAPACITY;

    if (old_capacity > 0 && !tb_size_multiply(old_capacity, 2, &capacity))
    {
        tb_memory_fail(old_capacity, sizeof(*old_slots), _Alignof(tb_atom_header *));
    }
    table->slots = TB_ALLOCATOR_NEW_ARRAY(&table->allocator, tb_atom_header *, capacity);
    table->capacity = capacity;
    for (size_t index = 0; index < old_capacity; index++)
    {
        tb_atom_header *const header = old_slots[index];

        if (header != NULL)
        {
            *tb_atom_slot(table, tb_atom_of_header(header), header->length, header->hash) = header;
        }
    }
    TB_ALLOCATOR_DELETE_ARRAY(&table->allocator, tb_atom_header *, old_slots, old_capacity);
}

/* ---------------------------------------------------------------------------
 * Tables.
 * ------------------------------------------------------------------------- */

tb_atom_table *tb_atom_table_create(const tb_allocator *allocator)
{
    tb_atom_table *const table = TB_ALLOCATOR_NEW(allocator, tb_atom_table);

    table->allocator = tb_allocator_resolve(allocator);
    return table;
}

void tb_atom_table_destroy(tb_atom_table *table)
{
    tb_allocator allocator = {0};

    if (table == NULL)
    {
        return;
    }
    allocator = table->allocator;
    for (size_t index = 0; index < table->capacity; index++)
    {
        tb_atom_header *const header = table->slots[index];

        if (header != NULL)
        {
            tb_allocator_free(&allocator, header, tb_atom_block_size(header->length),
                              _Alignof(tb_atom_header));
        }
    }
    TB_ALLOCATOR_DELETE_ARRAY(&allocator, tb_atom_header *, table->slots, table->capacity);
    TB_ALLOCATOR_DELETE(&allocator, tb_atom_table, table);
}

size_t tb_atom_table_count(const tb_atom_table *table)
{
    TB_ASSERT(table != NULL);
    return table->count;
}

/* ---------------------------------------------------------------------------
 * Interning.
 * ------------------------------------------------------------------------- */

/* The atom with these bytes and this hash, or NULL when the table has none. */
static const char *tb_atom_lookup(const tb_atom_table *table, const char *bytes, size_t length,
                                  uint64_t hash)
{
    const tb_atom_header *header = NULL;

    if (table->capacity == 0)
    {
        return NULL;
    }
    header = *tb_atom_slot(table, bytes, length, hash);
    if (header == NULL)
    {
        return NULL;
    }
    return tb_atom_of_header(header);
}

const char *tb_atom_find(const tb_atom_table *table, const char *bytes, size_t length)
{
    TB_ASSERT(table != NULL);
    TB_ASSERT(bytes != NULL || length == 0);
    return tb_atom_lookup(table, bytes, length, tb_hash_bytes(bytes, length));
}

const char *tb_atom_new(tb_atom_table *table, const char *bytes, size_t length)
{
    const char *existing = NULL;
    tb_atom_header *header = NULL;
    char *atom = NULL;
    uint64_t hash = {0};

    TB_ASSERT(table != NULL);
    TB_ASSERT(bytes != NULL || length == 0);
    hash = tb_hash_bytes(bytes, length);
    existing = tb_atom_lookup(table, bytes, length, hash);
    if (existing != NULL)
    {
        return existing;
    }
    /* Room in the table first: if the atom then cannot be allocated, the
     * table is larger than it needs to be and otherwise unchanged. */
    if (!tb_atom_has_room(table))
    {
        tb_atom_grow(table);
    }
    header =
        tb_allocator_alloc(&table->allocator, tb_atom_block_size(length), _Alignof(tb_atom_header));
    header->hash = hash;
    header->length = length;
    atom = (char *)(header + 1);
    if (length > 0)
    {
        memcpy(atom, bytes, length);
    }
    atom[length] = '\0';
    *tb_atom_slot(table, bytes, length, hash) = header;
    table->count++;
    return atom;
}

const char *tb_atom_string(tb_atom_table *table, const char *string)
{
    TB_ASSERT(string != NULL);
    return tb_atom_new(table, string, strlen(string));
}

const char *tb_atom_int(tb_atom_table *table, long long value)
{
    char digits[TB_ATOM_INT_CAPACITY] = {0};
    const size_t length = tb_fmt_buffer(digits, sizeof(digits), "%lld", value);

    return tb_atom_new(table, digits, length);
}
