#include "badger/array.h"

#include "badger/assert.h"
#include "badger/size.h"
#include "exclusive.h"
#include "memory_internal.h"

#include <string.h>

/* The first storage an array allocates holds this many elements, so that a
 * short array does not grow through capacities of one, two, and four. */
#define TB_ARRAY_FIRST_CAPACITY ((size_t)8)

struct tb_array
{
    tb_allocator allocator;   /* Supplies the structure and the storage. */
    unsigned char *data;      /* The elements; NULL while the capacity is 0. */
    size_t length;            /* Elements in use. */
    size_t capacity;          /* Elements the storage holds. */
    size_t element_size;      /* Bytes per element; at least 1. */
    size_t element_alignment; /* Alignment of the storage. */
    tb_exclusive exclusive;   /* One thread at a time changes it. */
};

static bool tb_array_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

/* Address of the element at an index, which may be the length. */
static unsigned char *tb_array_slot(const tb_array *array, size_t index)
{
    return array->data + index * array->element_size;
}

/* ---------------------------------------------------------------------------
 * Storage.
 * ------------------------------------------------------------------------- */

/* Changes the capacity to exactly `capacity` elements, which is at least the
 * length and more than zero. Returns false, changing nothing, when the size
 * overflows or the allocator cannot supply the storage. */
static bool tb_array_set_capacity(tb_array *array, size_t capacity)
{
    unsigned char *data = NULL;
    size_t size = {0};

    if (!tb_size_multiply(capacity, array->element_size, &size))
    {
        return false;
    }
    /* The current size cannot overflow: the storage exists. */
    data = tb_allocator_try_realloc(&array->allocator, array->data,
                                    array->capacity * array->element_size, size,
                                    array->element_alignment);
    if (data == NULL)
    {
        return false;
    }
    array->data = data;
    array->capacity = capacity;
    return true;
}

/* What each function that changes the object does once inside its guard. */
static bool tb_array_try_reserve_inside(tb_array *array, size_t capacity);
static void tb_array_reserve_inside(tb_array *array, size_t capacity);
static void tb_array_shrink_to_fit_inside(tb_array *array);
static void tb_array_resize_inside(tb_array *array, size_t length);
static void tb_array_clear_inside(tb_array *array);
static void tb_array_put_inside(tb_array *array, size_t index, const void *element);
static void *tb_array_insert_inside(tb_array *array, size_t index, const void *element);
static void *tb_array_push_inside(tb_array *array, const void *element);
static void tb_array_append_inside(tb_array *array, const void *elements, size_t count);
static void tb_array_remove_inside(tb_array *array, size_t index, void *element);
static void tb_array_remove_unordered_inside(tb_array *array, size_t index, void *element);
static void tb_array_pop_inside(tb_array *array, void *element);
static void *tb_array_push_sized_inside(tb_array *array, size_t element_size);

static bool tb_array_try_reserve_inside(tb_array *array, size_t capacity)
{
    TB_ASSERT(array != NULL);
    if (capacity <= array->capacity)
    {
        return true;
    }
    return tb_array_set_capacity(array, capacity);
}

bool tb_array_try_reserve(tb_array *array, size_t capacity)
{
    bool result = false;
    TB_ASSERT(array != NULL);
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    result = tb_array_try_reserve_inside(array, capacity);
    TB_EXCLUSIVE_LEAVE(&array->exclusive);
    return result;
}

static void tb_array_reserve_inside(tb_array *array, size_t capacity)
{
    TB_ASSERT(array != NULL);
    if (!tb_array_try_reserve_inside(array, capacity))
    {
        tb_memory_fail(capacity, array->element_size, array->element_alignment);
    }
}

void tb_array_reserve(tb_array *array, size_t capacity)
{
    TB_ASSERT(array != NULL);
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    tb_array_reserve_inside(array, capacity);
    TB_EXCLUSIVE_LEAVE(&array->exclusive);
}

/* Makes room for `count` more elements, growing by doubling so that a run of
 * additions costs a constant amount of copying per element. */
static void tb_array_make_room(tb_array *array, size_t count)
{
    size_t required = {0};
    size_t capacity = {0};

    if (count <= array->capacity - array->length)
    {
        return;
    }
    if (!tb_size_add(array->length, count, &required))
    {
        tb_memory_fail(count, array->element_size, array->element_alignment);
    }
    if (!tb_size_grow(array->capacity, required, array->element_size, &capacity))
    {
        tb_memory_fail(required, array->element_size, array->element_alignment);
    }
    if (capacity < TB_ARRAY_FIRST_CAPACITY)
    {
        capacity = TB_ARRAY_FIRST_CAPACITY;
    }
    /* The doubled capacity is a preference. When it cannot be had, the array
     * settles for exactly what it needs. */
    if (tb_array_set_capacity(array, capacity))
    {
        return;
    }
    tb_array_reserve(array, required);
}

static void tb_array_shrink_to_fit_inside(tb_array *array)
{
    TB_ASSERT(array != NULL);
    if (array->length == array->capacity)
    {
        return;
    }
    if (array->length == 0)
    {
        tb_allocator_free(&array->allocator, array->data, array->capacity * array->element_size,
                          array->element_alignment);
        array->data = NULL;
        array->capacity = 0;
        return;
    }
    (void)tb_array_set_capacity(array, array->length);
}

void tb_array_shrink_to_fit(tb_array *array)
{
    TB_ASSERT(array != NULL);
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    tb_array_shrink_to_fit_inside(array);
    TB_EXCLUSIVE_LEAVE(&array->exclusive);
}

/* ---------------------------------------------------------------------------
 * Creating and destroying.
 * ------------------------------------------------------------------------- */

tb_array *tb_array_create(const tb_allocator *allocator, size_t element_size,
                          size_t element_alignment)
{
    tb_array *array = NULL;

    TB_ASSERT_MSG(element_size > 0, "an element has at least one byte");
    TB_ASSERT_MSG(tb_array_is_power_of_two(element_alignment),
                  "element alignment must be a power of two");
    TB_ASSERT_MSG(element_size % element_alignment == 0,
                  "element size must be a multiple of the element alignment");
    array = TB_ALLOCATOR_NEW(allocator, tb_array);
    array->allocator = tb_allocator_resolve(allocator);
    array->element_size = element_size;
    array->element_alignment = element_alignment;
    return array;
}

tb_array *tb_array_copy(const tb_array *array, const tb_allocator *allocator)
{
    tb_array *copy = NULL;

    TB_ASSERT(array != NULL);
    copy = tb_array_create(allocator, array->element_size, array->element_alignment);
    /* Reserving first makes the capacity of the copy exactly its length. */
    tb_array_reserve(copy, array->length);
    tb_array_append(copy, array->data, array->length);
    return copy;
}

void tb_array_destroy(tb_array *array)
{
    tb_allocator allocator = {0};

    if (array == NULL)
    {
        return;
    }
    /* No thread may be inside a array that is going away. */
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    allocator = array->allocator;
    tb_allocator_free(&allocator, array->data, array->capacity * array->element_size,
                      array->element_alignment);
    TB_ALLOCATOR_DELETE(&allocator, tb_array, array);
}

/* ---------------------------------------------------------------------------
 * Size.
 * ------------------------------------------------------------------------- */

size_t tb_array_length(const tb_array *array)
{
    TB_ASSERT(array != NULL);
    return array->length;
}

size_t tb_array_capacity(const tb_array *array)
{
    TB_ASSERT(array != NULL);
    return array->capacity;
}

size_t tb_array_element_size(const tb_array *array)
{
    TB_ASSERT(array != NULL);
    return array->element_size;
}

static void tb_array_resize_inside(tb_array *array, size_t length)
{
    TB_ASSERT(array != NULL);
    if (length > array->length)
    {
        tb_array_reserve_inside(array, length);
        memset(tb_array_slot(array, array->length), 0,
               (length - array->length) * array->element_size);
    }
    array->length = length;
}

void tb_array_resize(tb_array *array, size_t length)
{
    TB_ASSERT(array != NULL);
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    tb_array_resize_inside(array, length);
    TB_EXCLUSIVE_LEAVE(&array->exclusive);
}

static void tb_array_clear_inside(tb_array *array)
{
    TB_ASSERT(array != NULL);
    array->length = 0;
}

void tb_array_clear(tb_array *array)
{
    TB_ASSERT(array != NULL);
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    tb_array_clear_inside(array);
    TB_EXCLUSIVE_LEAVE(&array->exclusive);
}

/* ---------------------------------------------------------------------------
 * Elements.
 * ------------------------------------------------------------------------- */

void *tb_array_at(const tb_array *array, size_t index)
{
    TB_ASSERT(array != NULL);
    TB_ASSERT_MSG(index < array->length, "index must be less than the length");
    return tb_array_slot(array, index);
}

void *tb_array_data(const tb_array *array)
{
    TB_ASSERT(array != NULL);
    return array->data;
}

void tb_array_get(const tb_array *array, size_t index, void *element)
{
    TB_ASSERT(element != NULL);
    memcpy(element, tb_array_at(array, index), array->element_size);
}

static void tb_array_put_inside(tb_array *array, size_t index, const void *element)
{
    TB_ASSERT(element != NULL);
    memcpy(tb_array_at(array, index), element, array->element_size);
}

void tb_array_put(tb_array *array, size_t index, const void *element)
{
    TB_ASSERT(array != NULL);
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    tb_array_put_inside(array, index, element);
    TB_EXCLUSIVE_LEAVE(&array->exclusive);
}

/* Copies an element into a slot, or zero-fills the slot when there is none. */
static void tb_array_fill_slot(const tb_array *array, unsigned char *slot, const void *element)
{
    if (element == NULL)
    {
        memset(slot, 0, array->element_size);
        return;
    }
    memcpy(slot, element, array->element_size);
}

/* Copies an element out of a slot when the caller wants it. */
static void tb_array_read_slot(const tb_array *array, const unsigned char *slot, void *element)
{
    if (element != NULL)
    {
        memcpy(element, slot, array->element_size);
    }
}

static void *tb_array_insert_inside(tb_array *array, size_t index, const void *element)
{
    unsigned char *slot = NULL;

    TB_ASSERT(array != NULL);
    TB_ASSERT_MSG(index <= array->length, "index must be at most the length");
    tb_array_make_room(array, 1);
    slot = tb_array_slot(array, index);
    memmove(slot + array->element_size, slot, (array->length - index) * array->element_size);
    tb_array_fill_slot(array, slot, element);
    array->length++;
    return slot;
}

void *tb_array_insert(tb_array *array, size_t index, const void *element)
{
    void *result = NULL;
    TB_ASSERT(array != NULL);
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    result = tb_array_insert_inside(array, index, element);
    TB_EXCLUSIVE_LEAVE(&array->exclusive);
    return result;
}

static void *tb_array_push_inside(tb_array *array, const void *element)
{
    TB_ASSERT(array != NULL);
    return tb_array_insert_inside(array, array->length, element);
}

void *tb_array_push(tb_array *array, const void *element)
{
    void *result = NULL;
    TB_ASSERT(array != NULL);
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    result = tb_array_push_inside(array, element);
    TB_EXCLUSIVE_LEAVE(&array->exclusive);
    return result;
}

static void tb_array_append_inside(tb_array *array, const void *elements, size_t count)
{
    TB_ASSERT(array != NULL);
    TB_ASSERT(elements != NULL || count == 0);
    if (count == 0)
    {
        return;
    }
    tb_array_make_room(array, count);
    memcpy(tb_array_slot(array, array->length), elements, count * array->element_size);
    array->length += count;
}

void tb_array_append(tb_array *array, const void *elements, size_t count)
{
    TB_ASSERT(array != NULL);
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    tb_array_append_inside(array, elements, count);
    TB_EXCLUSIVE_LEAVE(&array->exclusive);
}

static void tb_array_remove_inside(tb_array *array, size_t index, void *element)
{
    unsigned char *slot = NULL;

    TB_ASSERT(array != NULL);
    TB_ASSERT_MSG(index < array->length, "index must be less than the length");
    slot = tb_array_slot(array, index);
    tb_array_read_slot(array, slot, element);
    array->length--;
    memmove(slot, slot + array->element_size, (array->length - index) * array->element_size);
}

void tb_array_remove(tb_array *array, size_t index, void *element)
{
    TB_ASSERT(array != NULL);
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    tb_array_remove_inside(array, index, element);
    TB_EXCLUSIVE_LEAVE(&array->exclusive);
}

static void tb_array_remove_unordered_inside(tb_array *array, size_t index, void *element)
{
    unsigned char *slot = NULL;

    TB_ASSERT(array != NULL);
    TB_ASSERT_MSG(index < array->length, "index must be less than the length");
    slot = tb_array_slot(array, index);
    tb_array_read_slot(array, slot, element);
    array->length--;
    /* When the removed element was the last one, this copies it onto itself. */
    memmove(slot, tb_array_slot(array, array->length), array->element_size);
}

void tb_array_remove_unordered(tb_array *array, size_t index, void *element)
{
    TB_ASSERT(array != NULL);
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    tb_array_remove_unordered_inside(array, index, element);
    TB_EXCLUSIVE_LEAVE(&array->exclusive);
}

static void tb_array_pop_inside(tb_array *array, void *element)
{
    TB_ASSERT(array != NULL);
    TB_ASSERT_MSG(array->length > 0, "the array must not be empty");
    tb_array_remove_inside(array, array->length - 1, element);
}

void tb_array_pop(tb_array *array, void *element)
{
    TB_ASSERT(array != NULL);
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    tb_array_pop_inside(array, element);
    TB_EXCLUSIVE_LEAVE(&array->exclusive);
}

/* ---------------------------------------------------------------------------
 * Typed access.
 * ------------------------------------------------------------------------- */

void *tb_array_at_sized(const tb_array *array, size_t index, size_t element_size)
{
    TB_ASSERT(array != NULL);
    TB_ASSERT_MSG(element_size == array->element_size, "the array holds elements of another size");
    return tb_array_at(array, index);
}

void *tb_array_data_sized(const tb_array *array, size_t element_size)
{
    TB_ASSERT(array != NULL);
    TB_ASSERT_MSG(element_size == array->element_size, "the array holds elements of another size");
    return tb_array_data(array);
}

static void *tb_array_push_sized_inside(tb_array *array, size_t element_size)
{
    TB_ASSERT(array != NULL);
    TB_ASSERT_MSG(element_size == array->element_size, "the array holds elements of another size");
    tb_array_make_room(array, 1);
    array->length++;
    return tb_array_slot(array, array->length - 1);
}

void *tb_array_push_sized(tb_array *array, size_t element_size)
{
    void *result = NULL;
    TB_ASSERT(array != NULL);
    TB_EXCLUSIVE_ENTER(&array->exclusive, "a dynamic array");
    result = tb_array_push_sized_inside(array, element_size);
    TB_EXCLUSIVE_LEAVE(&array->exclusive);
    return result;
}
