/* The entries live in one array of slots, searched by open addressing with
 * linear probing: an entry goes in the first empty slot at or after the one
 * its hash names, so the entries that share a slot sit next to each other in
 * memory. Removing an entry moves the ones after it back into the gap, which
 * keeps every search ending at the first empty slot without leaving markers
 * behind. The array is kept at most three quarters full. */
#include "badger/table.h"

#include "badger/assert.h"
#include "badger/hash.h"
#include "badger/size.h"
#include "badger/str.h"
#include "exclusive.h"
#include "memory_internal.h"

#include <string.h>

/* The first array of slots. A power of two, like every later one, so that a
 * hash is reduced to a slot by masking. */
#define TB_TABLE_FIRST_SLOT_COUNT ((size_t)16)

/* The table grows when more than three quarters of its slots would be used. */
#define TB_TABLE_LOAD_NUMERATOR ((size_t)3)
#define TB_TABLE_LOAD_DENOMINATOR ((size_t)4)

typedef struct tb_table_slot
{
    uint64_t hash;   /* Hash of the key; 0 marks an empty slot. */
    const void *key; /* The key, as it was given. */
    void *value;     /* The value stored with it. */
} tb_table_slot;

struct tb_table
{
    tb_allocator allocator;         /* Supplies the table and its slots. */
    tb_table_hash_function *hash;   /* NULL to hash keys by address. */
    tb_table_equal_function *equal; /* NULL to compare keys by address. */
    tb_table_slot *slots;           /* `slot_count` slots; NULL while there are none. */
    size_t slot_count;              /* 0 or a power of two. */
    size_t length;                  /* Number of entries. */
    bool is_being_visited;          /* A visit is running: no entry may come or go. */
    tb_exclusive exclusive;         /* One thread at a time changes it. */
};

/* ---------------------------------------------------------------------------
 * Keys and slots.
 * ------------------------------------------------------------------------- */

/* The hash of a key as the table stores it: never 0, which marks an empty
 * slot. */
static uint64_t tb_table_hash_of(const tb_table *table, const void *key)
{
    uint64_t hash = {0};

    if (table->hash != NULL)
    {
        hash = table->hash(key);
    }
    else
    {
        hash = tb_hash_pointer(key);
    }
    if (hash == 0)
    {
        return 1;
    }
    return hash;
}

static bool tb_table_keys_are_equal(const tb_table *table, const void *left, const void *right)
{
    if (table->equal != NULL)
    {
        return table->equal(left, right);
    }
    return left == right;
}

/* The slot that holds a key, or the empty slot where it would go. The table
 * has slots, and at least one of them is empty. */
static tb_table_slot *tb_table_slot_of(const tb_table *table, const void *key, uint64_t hash)
{
    const size_t mask = table->slot_count - 1;
    size_t index = (size_t)hash & mask;

    for (;;)
    {
        tb_table_slot *const slot = &table->slots[index];

        if (slot->hash == 0)
        {
            return slot;
        }
        /* The stored hash rules out nearly every other key before the keys
         * themselves are compared. */
        if (slot->hash == hash && tb_table_keys_are_equal(table, slot->key, key))
        {
            return slot;
        }
        index = (index + 1) & mask;
    }
}

/* The slot that holds a key, or NULL when the key is not in the table. */
static tb_table_slot *tb_table_lookup(const tb_table *table, const void *key)
{
    tb_table_slot *slot = NULL;

    if (table->slot_count == 0)
    {
        return NULL;
    }
    slot = tb_table_slot_of(table, key, tb_table_hash_of(table, key));
    if (slot->hash == 0)
    {
        return NULL;
    }
    return slot;
}

/* ---------------------------------------------------------------------------
 * Storage.
 * ------------------------------------------------------------------------- */

/* Number of entries that `slot_count` slots hold before the table grows. */
static size_t tb_table_capacity_of(size_t slot_count)
{
    /* Divided first, which is exact for a power of two and cannot overflow. */
    return slot_count / TB_TABLE_LOAD_DENOMINATOR * TB_TABLE_LOAD_NUMERATOR;
}

/* Moves every entry into a new array of `slot_count` slots, a power of two
 * with room for all of them. Returns false, changing nothing, when the
 * allocator cannot supply the array. */
static bool tb_table_set_slot_count(tb_table *table, size_t slot_count)
{
    tb_table_slot *const old_slots = table->slots;
    const size_t old_slot_count = table->slot_count;
    tb_table_slot *const slots = tb_allocator_try_calloc(&table->allocator, slot_count,
                                                         sizeof(*slots), _Alignof(tb_table_slot));

    if (slots == NULL)
    {
        return false;
    }
    table->slots = slots;
    table->slot_count = slot_count;
    for (size_t index = 0; index < old_slot_count; index++)
    {
        const tb_table_slot *const old_slot = &old_slots[index];

        /* The keys are all different, so each goes to the first empty slot
         * its hash leads to. */
        if (old_slot->hash != 0)
        {
            *tb_table_slot_of(table, old_slot->key, old_slot->hash) = *old_slot;
        }
    }
    tb_allocator_free_array(&table->allocator, old_slots, old_slot_count, sizeof(*old_slots),
                            _Alignof(tb_table_slot));
    return true;
}

/* The smallest slot count that holds `capacity` entries. Returns false when
 * it cannot be expressed. */
static bool tb_table_slot_count_for(size_t capacity, size_t *slot_count)
{
    size_t count = TB_TABLE_FIRST_SLOT_COUNT;

    while (tb_table_capacity_of(count) < capacity)
    {
        if (!tb_size_multiply(count, 2, &count))
        {
            return false;
        }
    }
    *slot_count = count;
    return true;
}

/* What each function that changes the object does once inside its guard. */
static bool tb_table_try_reserve_inside(tb_table *table, size_t capacity);
static void tb_table_reserve_inside(tb_table *table, size_t capacity);
static void tb_table_shrink_to_fit_inside(tb_table *table);
static void *tb_table_put_inside(tb_table *table, const void *key, void *value);
static bool tb_table_remove_inside(tb_table *table, const void *key, tb_table_entry *entry);
static void tb_table_clear_inside(tb_table *table);
static void tb_table_visit_inside(tb_table *table, tb_table_visitor *visitor, void *context);

static bool tb_table_try_reserve_inside(tb_table *table, size_t capacity)
{
    size_t slot_count = {0};

    TB_ASSERT(table != NULL);
    TB_ASSERT_MSG(!table->is_being_visited, "the table must not be changed while it is visited");
    if (capacity <= tb_table_capacity_of(table->slot_count))
    {
        return true;
    }
    if (!tb_table_slot_count_for(capacity, &slot_count))
    {
        return false;
    }
    return tb_table_set_slot_count(table, slot_count);
}

bool tb_table_try_reserve(tb_table *table, size_t capacity)
{
    bool result = false;
    TB_ASSERT(table != NULL);
    TB_EXCLUSIVE_ENTER(&table->exclusive, "a hash table");
    result = tb_table_try_reserve_inside(table, capacity);
    TB_EXCLUSIVE_LEAVE(&table->exclusive);
    return result;
}

static void tb_table_reserve_inside(tb_table *table, size_t capacity)
{
    TB_ASSERT(table != NULL);
    if (!tb_table_try_reserve_inside(table, capacity))
    {
        tb_memory_fail(capacity, sizeof(tb_table_slot), _Alignof(tb_table_slot));
    }
}

void tb_table_reserve(tb_table *table, size_t capacity)
{
    TB_ASSERT(table != NULL);
    TB_EXCLUSIVE_ENTER(&table->exclusive, "a hash table");
    tb_table_reserve_inside(table, capacity);
    TB_EXCLUSIVE_LEAVE(&table->exclusive);
}

static void tb_table_shrink_to_fit_inside(tb_table *table)
{
    size_t slot_count = {0};

    TB_ASSERT(table != NULL);
    TB_ASSERT_MSG(!table->is_being_visited, "the table must not be changed while it is visited");
    if (table->length == 0)
    {
        tb_allocator_free_array(&table->allocator, table->slots, table->slot_count,
                                sizeof(*table->slots), _Alignof(tb_table_slot));
        table->slots = NULL;
        table->slot_count = 0;
        return;
    }
    if (tb_table_slot_count_for(table->length, &slot_count) && slot_count < table->slot_count)
    {
        (void)tb_table_set_slot_count(table, slot_count);
    }
}

void tb_table_shrink_to_fit(tb_table *table)
{
    TB_ASSERT(table != NULL);
    TB_EXCLUSIVE_ENTER(&table->exclusive, "a hash table");
    tb_table_shrink_to_fit_inside(table);
    TB_EXCLUSIVE_LEAVE(&table->exclusive);
}

size_t tb_table_capacity(const tb_table *table)
{
    TB_ASSERT(table != NULL);
    return tb_table_capacity_of(table->slot_count);
}

/* ---------------------------------------------------------------------------
 * Creating and destroying.
 * ------------------------------------------------------------------------- */

tb_table *tb_table_create(const tb_allocator *allocator, size_t capacity_hint,
                          tb_table_hash_function *hash, tb_table_equal_function *equal)
{
    tb_table *table = NULL;

    TB_ASSERT_MSG((hash == NULL) == (equal == NULL),
                  "give a hash function and an equality function, or neither");
    table = TB_ALLOCATOR_NEW(allocator, tb_table);
    table->allocator = tb_allocator_resolve(allocator);
    table->hash = hash;
    table->equal = equal;
    if (!tb_table_try_reserve(table, capacity_hint))
    {
        /* Nothing is left behind where the failure handler does not end the
         * process, as in a test. */
        tb_table_destroy(table);
        tb_memory_fail(capacity_hint, sizeof(tb_table_slot), _Alignof(tb_table_slot));
    }
    return table;
}

tb_table *tb_table_copy(const tb_table *table, const tb_allocator *allocator)
{
    tb_table *copy = NULL;

    TB_ASSERT(table != NULL);
    copy = tb_table_create(allocator, table->length, table->hash, table->equal);
    for (size_t index = 0; index < table->slot_count; index++)
    {
        const tb_table_slot *const slot = &table->slots[index];

        if (slot->hash != 0)
        {
            *tb_table_slot_of(copy, slot->key, slot->hash) = *slot;
        }
    }
    copy->length = table->length;
    return copy;
}

void tb_table_destroy(tb_table *table)
{
    tb_allocator allocator = {0};

    if (table == NULL)
    {
        return;
    }
    /* No thread may be inside a table that is going away. */
    TB_EXCLUSIVE_ENTER(&table->exclusive, "a hash table");
    allocator = table->allocator;
    tb_allocator_free_array(&allocator, table->slots, table->slot_count, sizeof(*table->slots),
                            _Alignof(tb_table_slot));
    TB_ALLOCATOR_DELETE(&allocator, tb_table, table);
}

uint64_t tb_table_string_hash(const void *key)
{
    return tb_hash_string(key);
}

bool tb_table_string_equal(const void *left, const void *right)
{
    TB_ASSERT(left != NULL);
    TB_ASSERT(right != NULL);
    return tb_str_equal(left, right);
}

/* ---------------------------------------------------------------------------
 * Entries.
 * ------------------------------------------------------------------------- */

size_t tb_table_length(const tb_table *table)
{
    TB_ASSERT(table != NULL);
    return table->length;
}

static void *tb_table_put_inside(tb_table *table, const void *key, void *value)
{
    tb_table_slot *slot = NULL;
    void *previous = NULL;
    uint64_t hash = {0};

    TB_ASSERT(table != NULL);
    TB_ASSERT_MSG(!table->is_being_visited, "the table must not be changed while it is visited");
    /* The key is hashed once, and one search finds either its entry or the
     * empty slot where a new entry goes. */
    hash = tb_table_hash_of(table, key);
    if (table->slot_count > 0)
    {
        slot = tb_table_slot_of(table, key, hash);
    }
    if (slot != NULL && slot->hash != 0)
    {
        previous = slot->value;
        slot->value = value;
        return previous;
    }
    /* A new key. The empty slot that was found is where it goes, unless the
     * table has no slots yet or must grow to stay no more than three quarters
     * full: then its place is among the new slots. */
    if (slot == NULL || table->length >= tb_table_capacity_of(table->slot_count))
    {
        tb_table_reserve_inside(table, table->length + 1);
        slot = tb_table_slot_of(table, key, hash);
    }
    slot->hash = hash;
    slot->key = key;
    slot->value = value;
    table->length++;
    return NULL;
}

void *tb_table_put(tb_table *table, const void *key, void *value)
{
    void *result = NULL;
    TB_ASSERT(table != NULL);
    TB_EXCLUSIVE_ENTER(&table->exclusive, "a hash table");
    result = tb_table_put_inside(table, key, value);
    TB_EXCLUSIVE_LEAVE(&table->exclusive);
    return result;
}

bool tb_table_find(const tb_table *table, const void *key, tb_table_entry *entry)
{
    const tb_table_slot *slot = NULL;

    TB_ASSERT(table != NULL);
    slot = tb_table_lookup(table, key);
    if (slot == NULL)
    {
        return false;
    }
    if (entry != NULL)
    {
        entry->key = slot->key;
        entry->value = slot->value;
    }
    return true;
}

void *tb_table_get(const tb_table *table, const void *key)
{
    tb_table_entry entry = {0};

    (void)tb_table_find(table, key, &entry);
    return entry.value;
}

bool tb_table_contains(const tb_table *table, const void *key)
{
    return tb_table_find(table, key, NULL);
}

/* Empties a slot and moves the entries after it back, so that each is still
 * reached from the slot its hash names without crossing an empty one. */
static void tb_table_close_gap(tb_table *table, tb_table_slot *removed)
{
    const size_t mask = table->slot_count - 1;
    size_t gap = (size_t)(removed - table->slots);
    size_t index = (gap + 1) & mask;

    while (table->slots[index].hash != 0)
    {
        const size_t home = (size_t)table->slots[index].hash & mask;
        /* How far this entry is from where its hash wants it, and how far
         * it is from the gap. It may move back into the gap only if that
         * does not take it to before its home. */
        const size_t distance_from_home = (index - home) & mask;
        const size_t distance_from_gap = (index - gap) & mask;

        if (distance_from_home >= distance_from_gap)
        {
            table->slots[gap] = table->slots[index];
            gap = index;
        }
        index = (index + 1) & mask;
    }
    table->slots[gap] = (tb_table_slot){0};
}

static bool tb_table_remove_inside(tb_table *table, const void *key, tb_table_entry *entry)
{
    tb_table_slot *slot = NULL;

    TB_ASSERT(table != NULL);
    TB_ASSERT_MSG(!table->is_being_visited, "the table must not be changed while it is visited");
    slot = tb_table_lookup(table, key);
    if (slot == NULL)
    {
        return false;
    }
    if (entry != NULL)
    {
        entry->key = slot->key;
        entry->value = slot->value;
    }
    tb_table_close_gap(table, slot);
    table->length--;
    return true;
}

bool tb_table_remove(tb_table *table, const void *key, tb_table_entry *entry)
{
    bool result = false;
    TB_ASSERT(table != NULL);
    TB_EXCLUSIVE_ENTER(&table->exclusive, "a hash table");
    result = tb_table_remove_inside(table, key, entry);
    TB_EXCLUSIVE_LEAVE(&table->exclusive);
    return result;
}

static void tb_table_clear_inside(tb_table *table)
{
    TB_ASSERT(table != NULL);
    TB_ASSERT_MSG(!table->is_being_visited, "the table must not be changed while it is visited");
    if (table->slot_count > 0)
    {
        memset(table->slots, 0, table->slot_count * sizeof(*table->slots));
    }
    table->length = 0;
}

void tb_table_clear(tb_table *table)
{
    TB_ASSERT(table != NULL);
    TB_EXCLUSIVE_ENTER(&table->exclusive, "a hash table");
    tb_table_clear_inside(table);
    TB_EXCLUSIVE_LEAVE(&table->exclusive);
}

/* ---------------------------------------------------------------------------
 * All entries.
 * ------------------------------------------------------------------------- */

static void tb_table_visit_inside(tb_table *table, tb_table_visitor *visitor, void *context)
{
    TB_ASSERT(table != NULL);
    TB_ASSERT(visitor != NULL);
    TB_ASSERT_MSG(!table->is_being_visited, "the table must not be changed while it is visited");
    table->is_being_visited = true;
    for (size_t index = 0; index < table->slot_count; index++)
    {
        tb_table_slot *const slot = &table->slots[index];

        if (slot->hash != 0)
        {
            visitor(slot->key, &slot->value, context);
        }
    }
    table->is_being_visited = false;
}

void tb_table_visit(tb_table *table, tb_table_visitor *visitor, void *context)
{
    TB_ASSERT(table != NULL);
    TB_EXCLUSIVE_ENTER(&table->exclusive, "a hash table");
    tb_table_visit_inside(table, visitor, context);
    TB_EXCLUSIVE_LEAVE(&table->exclusive);
}

size_t tb_table_to_array(const tb_table *table, tb_table_entry *entries, size_t capacity)
{
    size_t count = {0};

    TB_ASSERT(table != NULL);
    TB_ASSERT(entries != NULL || capacity == 0);
    for (size_t index = 0; index < table->slot_count && count < capacity; index++)
    {
        const tb_table_slot *const slot = &table->slots[index];

        if (slot->hash != 0)
        {
            entries[count].key = slot->key;
            entries[count].value = slot->value;
            count++;
        }
    }
    return table->length;
}

bool tb_table_next(const tb_table *table, size_t *cursor, tb_table_entry *entry)
{
    TB_ASSERT(table != NULL);
    TB_ASSERT(cursor != NULL);
    TB_ASSERT(entry != NULL);
    /* The cursor is the index of the next slot to look at. */
    for (size_t index = *cursor; index < table->slot_count; index++)
    {
        const tb_table_slot *const slot = &table->slots[index];

        if (slot->hash != 0)
        {
            entry->key = slot->key;
            entry->value = slot->value;
            *cursor = index + 1;
            return true;
        }
    }
    *cursor = table->slot_count;
    return false;
}
