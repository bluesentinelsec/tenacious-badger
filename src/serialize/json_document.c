/* JSON documents: a tree built from the reader's events in an arena, without
 * recursion. The values of an open container wait in a work list on the
 * heap; when it closes they are copied into the arena at their final size.
 * An object keeps its members in the order of the text, and an index of them
 * sorted by key, which finds a member and every duplicate in O(n log n). */
#include "badger/json.h"

#include "badger/array.h"
#include "badger/assert.h"

#include <string.h>

typedef struct tb_json_member
{
    const char *key;
    size_t key_length;
    const tb_json_value *value;
    size_t offset; /* Of the key in the text, to report a duplicate. */
} tb_json_member;

struct tb_json_value
{
    tb_json_type type;
    bool boolean;
    const char *text; /* A number's text, or a decoded string with a NUL after it. */
    size_t length;
    size_t count;
    const tb_json_value **items;   /* An array's values. */
    const tb_json_member *members; /* An object's members, in the order of the text. */
    const size_t *order;           /* Positions of `members`, sorted by key, then position. */
};

/* A container that is open, where its values start in the work list, and
 * its own key in its parent, which the keys inside it would otherwise replace. */
typedef struct tb_json_frame
{
    tb_json_value *container;
    size_t start;
    const char *key;
    size_t key_length;
    size_t key_offset;
} tb_json_frame;

typedef struct tb_json_builder
{
    tb_arena *arena;
    bool allow_duplicates;
    tb_array *pending; /* tb_json_member: values of open containers. */
    tb_array *frames;  /* tb_json_frame: the open containers. */
    const char *key;   /* The key read for the next value of an object. */
    size_t key_length;
    size_t key_offset;
    const tb_json_value *root;
} tb_json_builder;

/* -------------------------------------------------------------------------
 * Errors found here rather than by the reader.
 * ------------------------------------------------------------------------- */

static void tb_json_locate(const char *text, size_t offset, tb_json_error *error)
{
    size_t line = 1;
    size_t line_start = 0;
    for (size_t index = 0; index < offset; index++)
    {
        if (text[index] == '\n')
        {
            line++;
            line_start = index + 1;
        }
    }
    error->offset = offset;
    error->line = line;
    error->column = offset - line_start + 1;
}

/* -------------------------------------------------------------------------
 * Keys.
 * ------------------------------------------------------------------------- */

static int tb_json_compare_keys(const char *left, size_t left_length, const char *right,
                                size_t right_length)
{
    const size_t shorter = left_length < right_length ? left_length : right_length;
    const int bytes = shorter == 0 ? 0 : memcmp(left, right, shorter);
    if (bytes != 0)
    {
        return bytes;
    }
    if (left_length == right_length)
    {
        return 0;
    }
    return left_length < right_length ? -1 : 1;
}

/* Members by key, then by position, so equal keys keep the order of the text. */
static bool tb_json_member_before(const tb_json_member *members, size_t left, size_t right)
{
    const int keys = tb_json_compare_keys(members[left].key, members[left].key_length,
                                          members[right].key, members[right].key_length);
    if (keys != 0)
    {
        return keys < 0;
    }
    return left < right;
}

/* Heap sort: no memory beyond the index, and O(n log n) for any keys. */
static void tb_json_sift_down(const tb_json_member *members, size_t *order, size_t root,
                              size_t count)
{
    for (;;)
    {
        size_t largest = root;
        const size_t left = 2 * root + 1;
        const size_t right = left + 1;
        size_t swap = 0;
        if (left < count && tb_json_member_before(members, order[largest], order[left]))
        {
            largest = left;
        }
        if (right < count && tb_json_member_before(members, order[largest], order[right]))
        {
            largest = right;
        }
        if (largest == root)
        {
            return;
        }
        swap = order[root];
        order[root] = order[largest];
        order[largest] = swap;
        root = largest;
    }
}

static void tb_json_sort_members(const tb_json_member *members, size_t *order, size_t count)
{
    for (size_t index = 0; index < count; index++)
    {
        order[index] = index;
    }
    for (size_t root = count / 2; root > 0; root--)
    {
        tb_json_sift_down(members, order, root - 1, count);
    }
    for (size_t end = count; end > 1; end--)
    {
        const size_t swap = order[0];
        order[0] = order[end - 1];
        order[end - 1] = swap;
        tb_json_sift_down(members, order, 0, end - 1);
    }
}

/* -------------------------------------------------------------------------
 * Building.
 * ------------------------------------------------------------------------- */

static void *tb_json_arena_alloc(tb_json_builder *builder, size_t count, size_t size,
                                 size_t alignment)
{
    size_t bytes = 0;
    if (count != 0 && size > SIZE_MAX / count)
    {
        return NULL;
    }
    bytes = count * size;
    return tb_arena_try_alloc(builder->arena, bytes == 0 ? 1 : bytes, alignment);
}

static tb_json_value *tb_json_new_value(tb_json_builder *builder, tb_json_type type)
{
    tb_json_value *const value =
        tb_json_arena_alloc(builder, 1, sizeof(tb_json_value), _Alignof(tb_json_value));
    if (value != NULL)
    {
        *value = (tb_json_value){0};
        value->type = type;
    }
    return value;
}

/* Copy text into the arena, decoding escapes, with a NUL after it. */
static const char *tb_json_copy_text(tb_json_builder *builder, const tb_json_token *token,
                                     size_t *length)
{
    char *const copy = tb_json_arena_alloc(builder, token->length + 1, 1, 1);
    if (copy == NULL)
    {
        return NULL;
    }
    if (token->escaped)
    {
        *length = tb_json_unescape(token->text, token->length, copy);
    }
    else
    {
        if (token->length > 0)
        {
            memcpy(copy, token->text, token->length);
        }
        *length = token->length;
    }
    copy[*length] = '\0';
    return copy;
}

/* A value is complete: it is the document, or the next value of its container. */
static void tb_json_attach(tb_json_builder *builder, const tb_json_value *value)
{
    tb_json_member member = {0};
    if (tb_array_length(builder->frames) == 0)
    {
        builder->root = value;
        return;
    }
    member.key = builder->key;
    member.key_length = builder->key_length;
    member.offset = builder->key_offset;
    member.value = value;
    builder->key = NULL;
    builder->key_length = 0;
    (void)tb_array_push(builder->pending, &member);
}

/* Close the innermost container: move its values from the work list into the arena. */
static tb_json_result tb_json_close_container(tb_json_builder *builder, size_t *duplicate_offset)
{
    tb_json_frame frame = {0};
    tb_json_value *container = NULL;
    const tb_json_member *pending = NULL;
    size_t count = 0;
    tb_array_pop(builder->frames, &frame);
    container = frame.container;
    /* The reader closes only what it opened, and each frame holds what it opened. */
    TB_ASSERT(container != NULL);
    count = tb_array_length(builder->pending) - frame.start;
    pending =
        count == 0 ? NULL : (const tb_json_member *)tb_array_at(builder->pending, frame.start);
    container->count = count;
    if (container->type == TB_JSON_ARRAY)
    {
        const tb_json_value **items =
            tb_json_arena_alloc(builder, count, sizeof(*items), _Alignof(const tb_json_value *));
        if (items == NULL)
        {
            return TB_JSON_NO_MEMORY;
        }
        for (size_t index = 0; index < count; index++)
        {
            items[index] = pending[index].value;
        }
        container->items = items;
    }
    else
    {
        tb_json_member *const members =
            tb_json_arena_alloc(builder, count, sizeof(*members), _Alignof(tb_json_member));
        size_t *const order = tb_json_arena_alloc(builder, count, sizeof(*order), _Alignof(size_t));
        if (members == NULL || order == NULL)
        {
            return TB_JSON_NO_MEMORY;
        }
        if (count > 0)
        {
            memcpy(members, pending, count * sizeof(*members));
        }
        tb_json_sort_members(members, order, count);
        for (size_t index = 1; index < count && !builder->allow_duplicates; index++)
        {
            const tb_json_member *const earlier = &members[order[index - 1]];
            const tb_json_member *const later = &members[order[index]];
            if (tb_json_compare_keys(earlier->key, earlier->key_length, later->key,
                                     later->key_length) == 0)
            {
                *duplicate_offset = later->offset;
                return TB_JSON_DUPLICATE_KEY;
            }
        }
        container->members = members;
        container->order = order;
    }
    tb_array_resize(builder->pending, frame.start);
    builder->key = frame.key;
    builder->key_length = frame.key_length;
    builder->key_offset = frame.key_offset;
    tb_json_attach(builder, container);
    return TB_JSON_OK;
}

/* Handle one event. `failure_offset` is where a failure of this builder is. */
static tb_json_result tb_json_build(tb_json_builder *builder, const tb_json_token *token,
                                    size_t *failure_offset)
{
    tb_json_value *value = NULL;
    *failure_offset = token->offset;
    switch (token->event)
    {
    case TB_JSON_EVENT_NULL:
    case TB_JSON_EVENT_FALSE:
    case TB_JSON_EVENT_TRUE:
        value = tb_json_new_value(builder,
                                  token->event == TB_JSON_EVENT_NULL ? TB_JSON_NULL : TB_JSON_BOOL);
        if (value == NULL)
        {
            return TB_JSON_NO_MEMORY;
        }
        value->boolean = token->event == TB_JSON_EVENT_TRUE;
        tb_json_attach(builder, value);
        return TB_JSON_OK;
    case TB_JSON_EVENT_NUMBER:
    case TB_JSON_EVENT_STRING:
        value = tb_json_new_value(builder, token->event == TB_JSON_EVENT_NUMBER ? TB_JSON_NUMBER
                                                                                : TB_JSON_STRING);
        if (value == NULL)
        {
            return TB_JSON_NO_MEMORY;
        }
        value->text = tb_json_copy_text(builder, token, &value->length);
        if (value->text == NULL)
        {
            return TB_JSON_NO_MEMORY;
        }
        tb_json_attach(builder, value);
        return TB_JSON_OK;
    case TB_JSON_EVENT_KEY:
        builder->key = tb_json_copy_text(builder, token, &builder->key_length);
        builder->key_offset = token->offset;
        return builder->key == NULL ? TB_JSON_NO_MEMORY : TB_JSON_OK;
    case TB_JSON_EVENT_BEGIN_ARRAY:
    case TB_JSON_EVENT_BEGIN_OBJECT: {
        tb_json_frame frame = {0};
        value = tb_json_new_value(
            builder, token->event == TB_JSON_EVENT_BEGIN_ARRAY ? TB_JSON_ARRAY : TB_JSON_OBJECT);
        if (value == NULL)
        {
            return TB_JSON_NO_MEMORY;
        }
        frame.container = value;
        frame.start = tb_array_length(builder->pending);
        frame.key = builder->key;
        frame.key_length = builder->key_length;
        frame.key_offset = builder->key_offset;
        builder->key = NULL;
        builder->key_length = 0;
        (void)tb_array_push(builder->frames, &frame);
        return TB_JSON_OK;
    }
    case TB_JSON_EVENT_END_ARRAY:
    case TB_JSON_EVENT_END_OBJECT:
        return tb_json_close_container(builder, failure_offset);
    case TB_JSON_EVENT_END:
    default:
        return TB_JSON_OK;
    }
}

tb_json_result tb_json_parse(const char *text, size_t length, const tb_json_options *options,
                             tb_arena *arena, const tb_json_value **root, tb_json_error *error)
{
    tb_json_builder builder = {0};
    tb_json_reader *reader = NULL;
    tb_json_result result = TB_JSON_OK;
    tb_json_error found = {0};
    TB_ASSERT(text != NULL || length == 0);
    TB_ASSERT(arena != NULL);
    TB_ASSERT(root != NULL);
    *root = NULL;
    found.reason = "";
    builder.arena = arena;
    builder.allow_duplicates =
        options != NULL && (options->flags & TB_JSON_ALLOW_DUPLICATE_KEYS) != 0;
    builder.pending = TB_ARRAY_CREATE(NULL, tb_json_member);
    builder.frames = TB_ARRAY_CREATE(NULL, tb_json_frame);
    reader = tb_json_reader_create(text, length, options, NULL);
    for (;;)
    {
        tb_json_token token = {0};
        size_t failure_offset = 0;
        result = tb_json_reader_next(reader, &token);
        if (result != TB_JSON_OK)
        {
            tb_json_reader_error(reader, &found);
            goto cleanup;
        }
        if (token.event == TB_JSON_EVENT_END)
        {
            break;
        }
        result = tb_json_build(&builder, &token, &failure_offset);
        if (result != TB_JSON_OK)
        {
            found.result = result;
            found.reason = result == TB_JSON_NO_MEMORY ? "the arena is full" : "duplicate key";
            tb_json_locate(text, failure_offset, &found);
            goto cleanup;
        }
    }
    *root = builder.root;
cleanup:
    if (error != NULL)
    {
        *error = found;
    }
    tb_json_reader_destroy(reader);
    tb_array_destroy(builder.frames);
    tb_array_destroy(builder.pending);
    return result;
}

/* -------------------------------------------------------------------------
 * Reading a document.
 * ------------------------------------------------------------------------- */

tb_json_type tb_json_type_of(const tb_json_value *value)
{
    TB_ASSERT(value != NULL);
    return value->type;
}

bool tb_json_as_bool(const tb_json_value *value, bool *out)
{
    TB_ASSERT(value != NULL && out != NULL);
    if (value->type != TB_JSON_BOOL)
    {
        return false;
    }
    *out = value->boolean;
    return true;
}

bool tb_json_as_int64(const tb_json_value *value, int64_t *out)
{
    TB_ASSERT(value != NULL && out != NULL);
    return value->type == TB_JSON_NUMBER && tb_json_number_int64(value->text, value->length, out);
}

bool tb_json_as_uint64(const tb_json_value *value, uint64_t *out)
{
    TB_ASSERT(value != NULL && out != NULL);
    return value->type == TB_JSON_NUMBER && tb_json_number_uint64(value->text, value->length, out);
}

bool tb_json_as_double(const tb_json_value *value, double *out)
{
    TB_ASSERT(value != NULL && out != NULL);
    return value->type == TB_JSON_NUMBER && tb_json_number_double(value->text, value->length, out);
}

const char *tb_json_as_text(const tb_json_value *value, size_t *length)
{
    TB_ASSERT(value != NULL && length != NULL);
    if (value->type != TB_JSON_NUMBER && value->type != TB_JSON_STRING)
    {
        *length = 0;
        return NULL;
    }
    *length = value->length;
    return value->text;
}

size_t tb_json_count(const tb_json_value *value)
{
    TB_ASSERT(value != NULL);
    if (value->type != TB_JSON_ARRAY && value->type != TB_JSON_OBJECT)
    {
        return 0;
    }
    return value->count;
}

const tb_json_value *tb_json_at(const tb_json_value *value, size_t index)
{
    TB_ASSERT(value != NULL);
    TB_ASSERT_MSG(value->type == TB_JSON_ARRAY, "the value is not an array");
    TB_ASSERT_MSG(index < value->count, "index past the end of the array");
    return value->items[index];
}

const tb_json_value *tb_json_member_at(const tb_json_value *value, size_t index, const char **key,
                                       size_t *key_length)
{
    TB_ASSERT(value != NULL && key != NULL && key_length != NULL);
    TB_ASSERT_MSG(value->type == TB_JSON_OBJECT, "the value is not an object");
    TB_ASSERT_MSG(index < value->count, "index past the end of the object");
    *key = value->members[index].key;
    *key_length = value->members[index].key_length;
    return value->members[index].value;
}

const tb_json_value *tb_json_find(const tb_json_value *value, const char *key, size_t key_length)
{
    size_t low = 0;
    size_t high = 0;
    TB_ASSERT(value != NULL);
    TB_ASSERT(key != NULL || key_length == 0);
    if (value->type != TB_JSON_OBJECT)
    {
        return NULL;
    }
    /* The first sorted position whose key is greater; the one before it, if
     * equal, is the last member with the key. */
    high = value->count;
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        const tb_json_member *const member = &value->members[value->order[middle]];
        if (tb_json_compare_keys(member->key, member->key_length, key, key_length) <= 0)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    if (low == 0)
    {
        return NULL;
    }
    {
        const tb_json_member *const member = &value->members[value->order[low - 1]];
        if (tb_json_compare_keys(member->key, member->key_length, key, key_length) != 0)
        {
            return NULL;
        }
        return member->value;
    }
}
