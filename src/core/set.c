/* A set is a hash table whose keys are the members. The table does the
 * hashing, probing, and growing, so that there is one implementation of them
 * to get right and to make fast; this file adds what is particular to sets. */
#include "badger/set.h"

#include "badger/assert.h"
#include "badger/table.h"
#include "memory_internal.h"

struct tb_set
{
    tb_allocator allocator;       /* Supplies the set; the table has a copy. */
    tb_table *members;            /* The members are its keys. Owned. */
    tb_set_hash_function *hash;   /* As given at creation; NULL for by address. */
    tb_set_equal_function *equal; /* As given at creation; NULL for by address. */
    bool is_being_visited;        /* A visit is running: no member may come or go. */
};

/* The table stores a value with every key. For a member it is the address of
 * this, which is not NULL, so that a put can tell a new member from one that
 * was already there. Nothing reads or writes it. */
static char tb_set_presence = 0;

/* ---------------------------------------------------------------------------
 * Creating and destroying.
 * ------------------------------------------------------------------------- */

/* A set around a table of members, which it takes ownership of. */
static tb_set *tb_set_wrap(const tb_allocator *allocator, tb_table *members,
                           tb_set_hash_function *hash, tb_set_equal_function *equal)
{
    tb_set *const set = tb_allocator_try_calloc(allocator, 1, sizeof(*set), _Alignof(tb_set));

    if (set == NULL)
    {
        /* The table is not left behind where the failure handler does not
         * end the process, as in a test. */
        tb_table_destroy(members);
        tb_memory_fail(1, sizeof(*set), _Alignof(tb_set));
    }
    set->allocator = tb_allocator_resolve(allocator);
    set->members = members;
    set->hash = hash;
    set->equal = equal;
    return set;
}

tb_set *tb_set_create(const tb_allocator *allocator, size_t capacity_hint,
                      tb_set_hash_function *hash, tb_set_equal_function *equal)
{
    /* The table checks that both functions were given, or neither. */
    return tb_set_wrap(allocator, tb_table_create(allocator, capacity_hint, hash, equal), hash,
                       equal);
}

tb_set *tb_set_copy(const tb_set *set, const tb_allocator *allocator)
{
    TB_ASSERT(set != NULL);
    return tb_set_wrap(allocator, tb_table_copy(set->members, allocator), set->hash, set->equal);
}

void tb_set_destroy(tb_set *set)
{
    tb_allocator allocator = {0};

    if (set == NULL)
    {
        return;
    }
    allocator = set->allocator;
    tb_table_destroy(set->members);
    TB_ALLOCATOR_DELETE(&allocator, tb_set, set);
}

uint64_t tb_set_string_hash(const void *member)
{
    return tb_table_string_hash(member);
}

bool tb_set_string_equal(const void *left, const void *right)
{
    return tb_table_string_equal(left, right);
}

/* ---------------------------------------------------------------------------
 * Members.
 * ------------------------------------------------------------------------- */

size_t tb_set_length(const tb_set *set)
{
    TB_ASSERT(set != NULL);
    return tb_table_length(set->members);
}

bool tb_set_contains(const tb_set *set, const void *member)
{
    TB_ASSERT(set != NULL);
    return tb_table_contains(set->members, member);
}

bool tb_set_find(const tb_set *set, const void *member, const void **stored)
{
    tb_table_entry entry = {0};

    TB_ASSERT(set != NULL);
    if (!tb_table_find(set->members, member, &entry))
    {
        return false;
    }
    if (stored != NULL)
    {
        *stored = entry.key;
    }
    return true;
}

bool tb_set_put(tb_set *set, const void *member)
{
    TB_ASSERT(set != NULL);
    TB_ASSERT_MSG(!set->is_being_visited, "the set must not be changed while it is visited");
    return tb_table_put(set->members, member, &tb_set_presence) == NULL;
}

bool tb_set_remove(tb_set *set, const void *member, const void **stored)
{
    tb_table_entry entry = {0};

    TB_ASSERT(set != NULL);
    TB_ASSERT_MSG(!set->is_being_visited, "the set must not be changed while it is visited");
    if (!tb_table_remove(set->members, member, &entry))
    {
        return false;
    }
    if (stored != NULL)
    {
        *stored = entry.key;
    }
    return true;
}

void tb_set_clear(tb_set *set)
{
    TB_ASSERT(set != NULL);
    TB_ASSERT_MSG(!set->is_being_visited, "the set must not be changed while it is visited");
    tb_table_clear(set->members);
}

/* ---------------------------------------------------------------------------
 * All members.
 * ------------------------------------------------------------------------- */

bool tb_set_next(const tb_set *set, size_t *cursor, const void **member)
{
    tb_table_entry entry = {0};

    TB_ASSERT(set != NULL);
    TB_ASSERT(cursor != NULL);
    TB_ASSERT(member != NULL);
    if (!tb_table_next(set->members, cursor, &entry))
    {
        return false;
    }
    *member = entry.key;
    return true;
}

void tb_set_visit(tb_set *set, tb_set_visitor *visitor, void *context)
{
    const void *member = NULL;
    size_t cursor = {0};

    TB_ASSERT(set != NULL);
    TB_ASSERT(visitor != NULL);
    TB_ASSERT_MSG(!set->is_being_visited, "the set must not be changed while it is visited");
    set->is_being_visited = true;
    while (tb_set_next(set, &cursor, &member))
    {
        visitor(member, context);
    }
    set->is_being_visited = false;
}

size_t tb_set_to_array(const tb_set *set, const void **members, size_t capacity)
{
    const void *member = NULL;
    size_t cursor = {0};
    size_t count = {0};

    TB_ASSERT(set != NULL);
    TB_ASSERT(members != NULL || capacity == 0);
    while (count < capacity && tb_set_next(set, &cursor, &member))
    {
        members[count] = member;
        count++;
    }
    return tb_set_length(set);
}

/* ---------------------------------------------------------------------------
 * Comparing sets.
 * ------------------------------------------------------------------------- */

bool tb_set_is_subset(const tb_set *left, const tb_set *right)
{
    const void *member = NULL;
    size_t cursor = {0};

    TB_ASSERT(left != NULL);
    TB_ASSERT(right != NULL);
    TB_ASSERT_MSG(left->hash == right->hash && left->equal == right->equal,
                  "the sets must have the same hash and equality functions");
    /* A set with more members cannot be inside a smaller one, which saves
     * looking at any of them. */
    if (tb_set_length(left) > tb_set_length(right))
    {
        return false;
    }
    while (tb_set_next(left, &cursor, &member))
    {
        if (!tb_set_contains(right, member))
        {
            return false;
        }
    }
    return true;
}

bool tb_set_equal(const tb_set *left, const tb_set *right)
{
    TB_ASSERT(left != NULL);
    TB_ASSERT(right != NULL);
    TB_ASSERT_MSG(left->hash == right->hash && left->equal == right->equal,
                  "the sets must have the same hash and equality functions");
    return tb_set_length(left) == tb_set_length(right) && tb_set_is_subset(left, right);
}

bool tb_set_is_proper_subset(const tb_set *left, const tb_set *right)
{
    TB_ASSERT(left != NULL);
    TB_ASSERT(right != NULL);
    TB_ASSERT_MSG(left->hash == right->hash && left->equal == right->equal,
                  "the sets must have the same hash and equality functions");
    return tb_set_length(left) < tb_set_length(right) && tb_set_is_subset(left, right);
}

/* ---------------------------------------------------------------------------
 * Set operations.
 * ------------------------------------------------------------------------- */

/* Adds to `result` the members of `source` that are members of `other`, or
 * the ones that are not, as `wanted_in_other` says. */
static void tb_set_put_selected(tb_set *result, const tb_set *source, const tb_set *other,
                                bool wanted_in_other)
{
    const void *member = NULL;
    size_t cursor = {0};

    while (tb_set_next(source, &cursor, &member))
    {
        if (tb_set_contains(other, member) == wanted_in_other)
        {
            (void)tb_set_put(result, member);
        }
    }
}

/* Checks the operands of a set operation and creates the empty set that will
 * hold its result. */
static tb_set *tb_set_create_result(const tb_set *left, const tb_set *right,
                                    const tb_allocator *allocator)
{
    TB_ASSERT(left != NULL);
    TB_ASSERT(right != NULL);
    TB_ASSERT_MSG(left->hash == right->hash && left->equal == right->equal,
                  "the sets must have the same hash and equality functions");
    return tb_set_create(allocator, 0, left->hash, left->equal);
}

tb_set *tb_set_union(const tb_set *left, const tb_set *right, const tb_allocator *allocator)
{
    const void *member = NULL;
    size_t cursor = {0};
    tb_set *result = NULL;

    TB_ASSERT(left != NULL);
    TB_ASSERT(right != NULL);
    TB_ASSERT_MSG(left->hash == right->hash && left->equal == right->equal,
                  "the sets must have the same hash and equality functions");
    /* Starting from the members of `left` keeps its pointers where both sets
     * hold an equal member: a put of a member that is there stores nothing. */
    result = tb_set_copy(left, allocator);
    while (tb_set_next(right, &cursor, &member))
    {
        (void)tb_set_put(result, member);
    }
    return result;
}

tb_set *tb_set_intersection(const tb_set *left, const tb_set *right, const tb_allocator *allocator)
{
    tb_set *const result = tb_set_create_result(left, right, allocator);

    tb_set_put_selected(result, left, right, true);
    return result;
}

tb_set *tb_set_difference(const tb_set *left, const tb_set *right, const tb_allocator *allocator)
{
    tb_set *const result = tb_set_create_result(left, right, allocator);

    tb_set_put_selected(result, left, right, false);
    return result;
}

tb_set *tb_set_symmetric_difference(const tb_set *left, const tb_set *right,
                                    const tb_allocator *allocator)
{
    tb_set *const result = tb_set_create_result(left, right, allocator);

    tb_set_put_selected(result, left, right, false);
    tb_set_put_selected(result, right, left, false);
    return result;
}

/* ---------------------------------------------------------------------------
 * Storage.
 * ------------------------------------------------------------------------- */

void tb_set_reserve(tb_set *set, size_t capacity)
{
    TB_ASSERT(set != NULL);
    TB_ASSERT_MSG(!set->is_being_visited, "the set must not be changed while it is visited");
    tb_table_reserve(set->members, capacity);
}

bool tb_set_try_reserve(tb_set *set, size_t capacity)
{
    TB_ASSERT(set != NULL);
    TB_ASSERT_MSG(!set->is_being_visited, "the set must not be changed while it is visited");
    return tb_table_try_reserve(set->members, capacity);
}

void tb_set_shrink_to_fit(tb_set *set)
{
    TB_ASSERT(set != NULL);
    TB_ASSERT_MSG(!set->is_being_visited, "the set must not be changed while it is visited");
    tb_table_shrink_to_fit(set->members);
}

size_t tb_set_capacity(const tb_set *set)
{
    TB_ASSERT(set != NULL);
    return tb_table_capacity(set->members);
}
