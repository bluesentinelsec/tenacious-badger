#include "badger/path.h"
#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/size.h"
#include "badger/str.h"
#include "badger/utf8.h"
#include <string.h>

typedef struct tb_path_root
{
    size_t length;
    bool rooted;
    bool absolute;
    bool verbatim;
    bool unc;
} tb_path_root;

typedef enum tb_path_operation
{
    TB_PATH_NORMALIZE,
    TB_PATH_JOIN,
    TB_PATH_ROOTED_JOIN,
    TB_PATH_RELATIVE,
    TB_PATH_EXTENSION,
    TB_PATH_PARENT,
    TB_PATH_NATIVE
} tb_path_operation;

static bool tb_path_separator(tb_path_style style, char byte)
{
    return byte == '/' || (style == TB_PATH_WINDOWS && byte == '\\');
}

static bool tb_path_letter(char byte)
{
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

static tb_path_view tb_path_slice(tb_path_view input, size_t begin, size_t end)
{
    tb_path_view result = {0};
    result.data = input.data;
    if (input.data != NULL)
    {
        result.data += begin;
    }
    result.length = end - begin;
    return result;
}

static bool tb_path_equal(tb_path_view left, tb_path_view right)
{
    return left.length == right.length &&
           (left.length == 0 || memcmp(left.data, right.data, left.length) == 0);
}

static bool tb_path_is_dot(tb_path_view component, size_t dots)
{
    if (component.length != dots)
    {
        return false;
    }
    for (size_t index = {0}; index < dots; index++)
    {
        if (component.data[index] != '.')
        {
            return false;
        }
    }
    return true;
}

static bool tb_path_root_read(tb_path_style style, tb_path_view input, tb_path_root *root)
{
    size_t error = {0};
    size_t position = {0};
    size_t start = {0};
    TB_ASSERT(style == TB_PATH_WINDOWS || style == TB_PATH_POSIX);
    TB_ASSERT(input.data != NULL || input.length == 0);
    if (!tb_utf8_validate(input.data, input.length, &error))
    {
        return false;
    }
    for (size_t index = {0}; index < input.length; index++)
    {
        if (input.data[index] == '\0')
        {
            return false;
        }
    }
    if (input.length == 0)
    {
        return true;
    }
    if (style == TB_PATH_POSIX)
    {
        while (position < input.length && input.data[position] == '/')
        {
            position++;
        }
        root->length = position;
        root->rooted = position != 0;
        root->absolute = root->rooted;
        return true;
    }
    if (input.length >= 4 && tb_path_separator(style, input.data[0]) &&
        tb_path_separator(style, input.data[1]) && tb_path_separator(style, input.data[3]))
    {
        if (input.data[2] == '.')
        {
            return false;
        }
        if (input.data[2] == '?')
        {
            root->verbatim = true;
            position = 4;
        }
    }
    if (input.length - position >= 2 && tb_path_letter(input.data[position]) &&
        input.data[position + 1] == ':')
    {
        position += 2;
        if (position < input.length && tb_path_separator(style, input.data[position]))
        {
            root->absolute = true;
            root->rooted = true;
            while (position < input.length && tb_path_separator(style, input.data[position]))
            {
                position++;
            }
        }
        if (root->verbatim && !root->absolute)
        {
            return false;
        }
        root->length = position;
        return true;
    }
    if (root->verbatim)
    {
        if (input.length - position < 4 || memcmp(input.data + position, "UNC", 3) != 0 ||
            !tb_path_separator(style, input.data[position + 3]))
        {
            return false;
        }
        position += 4;
        root->unc = true;
    }
    else if (input.length >= 2 && tb_path_separator(style, input.data[0]) &&
             tb_path_separator(style, input.data[1]))
    {
        position = 2;
        root->unc = true;
    }
    if (root->unc)
    {
        for (size_t part = {0}; part < 2; part++)
        {
            start = position;
            while (position < input.length && !tb_path_separator(style, input.data[position]))
            {
                position++;
            }
            if (position == start || tb_path_is_dot(tb_path_slice(input, start, position), 1) ||
                tb_path_is_dot(tb_path_slice(input, start, position), 2))
            {
                return false;
            }
            if (part == 0 && position == input.length)
            {
                return false;
            }
            while (position < input.length && tb_path_separator(style, input.data[position]))
            {
                position++;
            }
        }
        root->length = position;
        root->rooted = true;
        root->absolute = true;
        return true;
    }
    if (tb_path_separator(style, input.data[0]))
    {
        root->length = 1;
        root->rooted = true;
    }
    return true;
}

tb_path_result tb_path_parse(tb_path_style style, tb_path_view input, tb_path_parts *parts)
{
    tb_path_root root = {0};
    tb_path_parts found = {0};
    size_t name_start = {0};
    size_t extension = {0};
    TB_ASSERT(parts != NULL);
    if (!tb_path_root_read(style, input, &root))
    {
        return TB_PATH_INVALID;
    }
    name_start = root.length;
    for (size_t index = {0}; index < input.length; index++)
    {
        if (index >= root.length && tb_path_separator(style, input.data[index]))
        {
            name_start = index + 1;
        }
    }
    extension = input.length;
    for (size_t index = {0}; index < input.length - name_start; index++)
    {
        if (input.data[name_start + index] == '.' && index > 0)
        {
            extension = name_start + index;
        }
    }
    if (tb_path_is_dot(tb_path_slice(input, name_start, input.length), 2))
    {
        extension = input.length;
    }
    found.root = tb_path_slice(input, 0, root.length);
    found.directory = tb_path_slice(input, 0, name_start);
    found.name = tb_path_slice(input, name_start, input.length);
    found.stem = tb_path_slice(input, name_start, extension);
    found.extension = tb_path_slice(input, extension, input.length);
    found.absolute = root.absolute;
    found.verbatim = root.verbatim;
    *parts = found;
    return TB_PATH_OK;
}

/* Every working string has capacity input length + 3: canonical roots can
 * gain a slash, and an empty relative path becomes dot plus a terminator. */
static char *tb_path_work(size_t length, tb_path_result *result)
{
    size_t capacity = {0};
    if (!tb_size_add(length, 3, &capacity))
    {
        *result = TB_PATH_TOO_LARGE;
        return NULL;
    }
    return tb_malloc(capacity);
}

static char *tb_path_normalized(tb_path_style style, tb_path_view input, tb_path_result *result)
{
    tb_path_root root = {0};
    char *output = NULL;
    size_t used = {0};
    size_t root_size = {0};
    size_t position = {0};
    if (!tb_path_root_read(style, input, &root))
    {
        *result = TB_PATH_INVALID;
        return NULL;
    }
    output = tb_path_work(input.length, result);
    if (output == NULL)
    {
        return NULL;
    }
    if (style == TB_PATH_POSIX && root.length != 0)
    {
        output[used] = '/';
        used++;
        if (root.length == 2)
        {
            output[used] = '/';
            used++;
        }
    }
    else
    {
        for (size_t index = {0}; index < root.length; index++)
        {
            char byte = {0};
            byte = input.data[index];
            if (tb_path_separator(style, byte))
            {
                byte = '/';
            }
            if (byte == '/' && used > 1 && output[used - 1] == '/')
            {
                continue;
            }
            output[used] = byte;
            used++;
        }
        if (root.unc && output[used - 1] != '/')
        {
            output[used] = '/';
            used++;
        }
    }
    root_size = used;
    position = root.length;
    while (position < input.length)
    {
        size_t start = {0};
        size_t previous = {0};
        tb_path_view component = {0};
        while (position < input.length && tb_path_separator(style, input.data[position]))
        {
            position++;
        }
        start = position;
        while (position < input.length && !tb_path_separator(style, input.data[position]))
        {
            position++;
        }
        component = tb_path_slice(input, start, position);
        if (component.length == 0)
        {
            continue;
        }
        if (!root.verbatim && tb_path_is_dot(component, 1))
        {
            continue;
        }
        if (!root.verbatim && tb_path_is_dot(component, 2))
        {
            previous = used;
            while (previous > root_size && output[previous - 1] != '/')
            {
                previous--;
            }
            if (used > root_size &&
                !(used - previous == 2 && output[previous] == '.' && output[previous + 1] == '.'))
            {
                used = previous;
                if (used > root_size && output[used - 1] == '/')
                {
                    used--;
                }
                continue;
            }
            if (root.rooted)
            {
                continue;
            }
        }
        if (used > root_size && output[used - 1] != '/')
        {
            output[used] = '/';
            used++;
        }
        memcpy(output + used, component.data, component.length);
        used += component.length;
    }
    if (used == 0)
    {
        output[used] = '.';
        used++;
    }
    output[used] = '\0';
    *result = TB_PATH_OK;
    return output;
}

static tb_path_view tb_path_string_view(const char *string)
{
    tb_path_view view = {0};
    view.data = string;
    view.length = tb_str_length(string);
    return view;
}

static char *tb_path_combine(tb_path_style style, tb_path_view first, tb_path_view second,
                             tb_path_result *result)
{
    tb_path_root root = {0};
    char *joined = NULL;
    char *normalized = NULL;
    size_t length = {0};
    size_t used = {0};
    if (!tb_path_root_read(style, second, &root))
    {
        *result = TB_PATH_INVALID;
        return NULL;
    }
    if (root.length != 0)
    {
        return tb_path_normalized(style, second, result);
    }
    if (!tb_size_add(first.length, second.length, &length))
    {
        *result = TB_PATH_TOO_LARGE;
        return NULL;
    }
    joined = tb_path_work(length, result);
    if (joined == NULL)
    {
        return NULL;
    }
    if (first.length != 0)
    {
        memcpy(joined, first.data, first.length);
        used = first.length;
    }
    if (used != 0 && !tb_path_separator(style, joined[used - 1]) &&
        !(style == TB_PATH_WINDOWS && joined[used - 1] == ':'))
    {
        joined[used] = '/';
        used++;
    }
    if (second.length != 0)
    {
        memcpy(joined + used, second.data, second.length);
        used += second.length;
    }
    normalized = tb_path_normalized(style, (tb_path_view){joined, used}, result);
    tb_free(joined);
    return normalized;
}

static bool tb_path_child_is_contained(tb_path_style style, tb_path_view child)
{
    tb_path_root root = {0};
    size_t position = {0};
    size_t depth = {0};
    if (!tb_path_root_read(style, child, &root) || root.length != 0)
    {
        return false;
    }
    while (position < child.length)
    {
        size_t start = {0};
        tb_path_view component = {0};
        while (position < child.length && tb_path_separator(style, child.data[position]))
        {
            position++;
        }
        start = position;
        while (position < child.length && !tb_path_separator(style, child.data[position]))
        {
            position++;
        }
        component = tb_path_slice(child, start, position);
        if (component.length == 0 || tb_path_is_dot(component, 1))
        {
            continue;
        }
        if (tb_path_is_dot(component, 2))
        {
            if (depth == 0)
            {
                return false;
            }
            depth--;
        }
        else
        {
            depth++;
        }
    }
    return true;
}

static char *tb_path_relative_work(tb_path_style style, const char *base, const char *target,
                                   tb_path_result *result)
{
    tb_path_parts base_parts = {0};
    tb_path_parts target_parts = {0};
    tb_path_view base_view = {0};
    tb_path_view target_view = {0};
    size_t base_at = {0};
    size_t target_at = {0};
    size_t up = {0};
    size_t size = {0};
    size_t used = {0};
    char *output = NULL;
    base_view = tb_path_string_view(base);
    target_view = tb_path_string_view(target);
    (void)tb_path_parse(style, base_view, &base_parts);
    (void)tb_path_parse(style, target_view, &target_parts);
    if (!base_parts.absolute || !target_parts.absolute || base_parts.verbatim ||
        target_parts.verbatim || !tb_path_equal(base_parts.root, target_parts.root))
    {
        *result = TB_PATH_DIFFERENT_ROOT;
        return NULL;
    }
    base_at = base_parts.root.length;
    target_at = target_parts.root.length;
    while (base_at < base_view.length && target_at < target_view.length)
    {
        size_t base_end = {0};
        size_t target_end = {0};
        base_end = base_at;
        target_end = target_at;
        while (base_end < base_view.length && base[base_end] != '/')
        {
            base_end++;
        }
        while (target_end < target_view.length && target[target_end] != '/')
        {
            target_end++;
        }
        if (!tb_path_equal(tb_path_slice(base_view, base_at, base_end),
                           tb_path_slice(target_view, target_at, target_end)))
        {
            break;
        }
        base_at = base_end;
        target_at = target_end;
        if (base_at < base_view.length)
        {
            base_at++;
        }
        if (target_at < target_view.length)
        {
            target_at++;
        }
    }
    for (size_t index = {0}; index < base_view.length - base_at; index++)
    {
        if (index == 0 || base[base_at + index - 1] == '/')
        {
            up++;
        }
    }
    if (!tb_size_multiply(up, 3, &size) ||
        !tb_size_add(size, target_view.length - target_at, &size))
    {
        *result = TB_PATH_TOO_LARGE;
        return NULL;
    }
    output = tb_path_work(size, result);
    if (output == NULL)
    {
        return NULL;
    }
    for (size_t index = {0}; index < up; index++)
    {
        memcpy(output + used, "../", 3);
        used += 3;
    }
    if (target_at < target_view.length)
    {
        memcpy(output + used, target + target_at, target_view.length - target_at);
        used += target_view.length - target_at;
    }
    else if (used != 0)
    {
        used--;
    }
    if (used == 0)
    {
        output[used] = '.';
        used++;
    }
    output[used] = '\0';
    *result = TB_PATH_OK;
    return output;
}

static char *tb_path_build(tb_path_style style, tb_path_operation operation, tb_path_view first,
                           tb_path_view second, tb_path_result *result)
{
    char *left = NULL;
    char *right = NULL;
    char *output = NULL;
    tb_path_parts parts = {0};
    tb_path_root root = {0};
    size_t length = {0};
    size_t size = {0};
    TB_ASSERT(first.data != NULL || first.length == 0);
    TB_ASSERT(second.data != NULL || second.length == 0);
    *result = TB_PATH_OK;
    left = tb_path_normalized(style, first, result);
    if (left == NULL)
    {
        goto cleanup;
    }
    switch (operation)
    {
    case TB_PATH_NORMALIZE:
        output = left;
        left = NULL;
        break;
    case TB_PATH_NATIVE:
        if (style == TB_PATH_WINDOWS)
        {
            for (size_t index = {0}; left[index] != '\0'; index++)
            {
                if (left[index] == '/')
                {
                    left[index] = '\\';
                }
            }
        }
        output = left;
        left = NULL;
        break;
    case TB_PATH_JOIN:
        /* Empty first input must not acquire the normalized dot prefix. */
        output = tb_path_combine(style, first, second, result);
        break;
    case TB_PATH_ROOTED_JOIN:
        (void)tb_path_parse(style, tb_path_string_view(left), &parts);
        if (!parts.absolute || parts.verbatim)
        {
            *result = TB_PATH_INVALID;
            break;
        }
        if (!tb_path_root_read(style, second, &root))
        {
            *result = TB_PATH_INVALID;
            break;
        }
        if (!tb_path_child_is_contained(style, second))
        {
            *result = TB_PATH_ESCAPE;
            break;
        }
        output = tb_path_combine(style, tb_path_string_view(left), second, result);
        break;
    case TB_PATH_RELATIVE:
        right = tb_path_normalized(style, second, result);
        if (right != NULL)
        {
            output = tb_path_relative_work(style, left, right, result);
        }
        break;
    case TB_PATH_PARENT:
        length = tb_str_length(left);
        (void)tb_path_parse(style, (tb_path_view){left, length}, &parts);
        length = parts.directory.length;
        while (length > parts.root.length && left[length - 1] == '/')
        {
            length--;
        }
        if (length == 0)
        {
            left[0] = '.';
            length = 1;
        }
        left[length] = '\0';
        output = left;
        left = NULL;
        break;
    case TB_PATH_EXTENSION:
        if (!tb_path_root_read(style, second, &root))
        {
            *result = TB_PATH_INVALID;
            break;
        }
        if (second.length != 0 && second.data[0] != '.')
        {
            *result = TB_PATH_INVALID;
            break;
        }
        for (size_t index = {0}; index < second.length; index++)
        {
            if (tb_path_separator(style, second.data[index]))
            {
                *result = TB_PATH_INVALID;
                break;
            }
        }
        if (*result != TB_PATH_OK)
        {
            break;
        }
        /* Use the original spelling: a trailing separator has no filename. */
        (void)tb_path_parse(style, first, &parts);
        if (parts.name.length == 0 || tb_path_is_dot(parts.name, 1) ||
            tb_path_is_dot(parts.name, 2))
        {
            *result = TB_PATH_INVALID;
            break;
        }
        length = first.length - parts.extension.length;
        if (!tb_size_add(length, second.length, &size))
        {
            *result = TB_PATH_TOO_LARGE;
            break;
        }
        right = tb_path_work(size, result);
        if (right == NULL)
        {
            break;
        }
        if (length != 0)
        {
            memcpy(right, first.data, length);
        }
        if (second.length != 0)
        {
            memcpy(right + length, second.data, second.length);
        }
        output = tb_path_normalized(style, (tb_path_view){right, size}, result);
        break;
    }
cleanup:
    tb_free(left);
    tb_free(right);
    return output;
}

static tb_path_result tb_path_write(tb_path_style style, tb_path_operation operation,
                                    tb_path_view first, tb_path_view second, char *destination,
                                    size_t capacity, size_t *required)
{
    tb_path_result result = {0};
    char *built = NULL;
    size_t size = {0};
    TB_ASSERT(destination != NULL || capacity == 0);
    TB_ASSERT(required != NULL);
    *required = 0;
    built = tb_path_build(style, operation, first, second, &result);
    if (built == NULL)
    {
        return result;
    }
    size = tb_str_length(built) + 1;
    *required = size;
    if (destination != NULL)
    {
        if (capacity < size)
        {
            result = TB_PATH_BUFFER_TOO_SMALL;
        }
        else
        {
            memcpy(destination, built, size);
        }
    }
    tb_free(built);
    return result;
}

static char *tb_path_owned(const tb_allocator *allocator, tb_path_style style,
                           tb_path_operation operation, tb_path_view first, tb_path_view second,
                           tb_path_result *result)
{
    char *built = NULL;
    char *owned = NULL;
    size_t size = {0};
    TB_ASSERT(result != NULL);
    built = tb_path_build(style, operation, first, second, result);
    if (built == NULL)
    {
        return NULL;
    }
    size = tb_str_length(built) + 1;
    owned = tb_allocator_alloc(allocator, size, 1);
    memcpy(owned, built, size);
    tb_free(built);
    return owned;
}

tb_path_result tb_path_normalize(tb_path_style style, tb_path_view input, char *destination,
                                 size_t capacity, size_t *required)
{
    return tb_path_write(style, TB_PATH_NORMALIZE, input, (tb_path_view){0}, destination, capacity,
                         required);
}
char *tb_path_normalize_new(const tb_allocator *allocator, tb_path_style style, tb_path_view input,
                            tb_path_result *result)
{
    return tb_path_owned(allocator, style, TB_PATH_NORMALIZE, input, (tb_path_view){0}, result);
}

tb_path_result tb_path_parent(tb_path_style style, tb_path_view input, char *destination,
                              size_t capacity, size_t *required)
{
    return tb_path_write(style, TB_PATH_PARENT, input, (tb_path_view){0}, destination, capacity,
                         required);
}
char *tb_path_parent_new(const tb_allocator *allocator, tb_path_style style, tb_path_view input,
                         tb_path_result *result)
{
    return tb_path_owned(allocator, style, TB_PATH_PARENT, input, (tb_path_view){0}, result);
}

tb_path_result tb_path_to_native(tb_path_style style, tb_path_view input, char *destination,
                                 size_t capacity, size_t *required)
{
    return tb_path_write(style, TB_PATH_NATIVE, input, (tb_path_view){0}, destination, capacity,
                         required);
}
char *tb_path_to_native_new(const tb_allocator *allocator, tb_path_style style, tb_path_view input,
                            tb_path_result *result)
{
    return tb_path_owned(allocator, style, TB_PATH_NATIVE, input, (tb_path_view){0}, result);
}

tb_path_result tb_path_join(tb_path_style style, tb_path_view first, tb_path_view second,
                            char *destination, size_t capacity, size_t *required)
{
    return tb_path_write(style, TB_PATH_JOIN, first, second, destination, capacity, required);
}
char *tb_path_join_new(const tb_allocator *allocator, tb_path_style style, tb_path_view first,
                       tb_path_view second, tb_path_result *result)
{
    return tb_path_owned(allocator, style, TB_PATH_JOIN, first, second, result);
}

tb_path_result tb_path_rooted_join(tb_path_style style, tb_path_view first, tb_path_view second,
                                   char *destination, size_t capacity, size_t *required)
{
    return tb_path_write(style, TB_PATH_ROOTED_JOIN, first, second, destination, capacity,
                         required);
}
char *tb_path_rooted_join_new(const tb_allocator *allocator, tb_path_style style,
                              tb_path_view first, tb_path_view second, tb_path_result *result)
{
    return tb_path_owned(allocator, style, TB_PATH_ROOTED_JOIN, first, second, result);
}

tb_path_result tb_path_relative(tb_path_style style, tb_path_view first, tb_path_view second,
                                char *destination, size_t capacity, size_t *required)
{
    return tb_path_write(style, TB_PATH_RELATIVE, first, second, destination, capacity, required);
}
char *tb_path_relative_new(const tb_allocator *allocator, tb_path_style style, tb_path_view first,
                           tb_path_view second, tb_path_result *result)
{
    return tb_path_owned(allocator, style, TB_PATH_RELATIVE, first, second, result);
}

tb_path_result tb_path_replace_extension(tb_path_style style, tb_path_view first,
                                         tb_path_view second, char *destination, size_t capacity,
                                         size_t *required)
{
    return tb_path_write(style, TB_PATH_EXTENSION, first, second, destination, capacity, required);
}
char *tb_path_replace_extension_new(const tb_allocator *allocator, tb_path_style style,
                                    tb_path_view first, tb_path_view second, tb_path_result *result)
{
    return tb_path_owned(allocator, style, TB_PATH_EXTENSION, first, second, result);
}

const char *tb_path_result_name(tb_path_result result)
{
    switch (result)
    {
    case TB_PATH_OK:
        return "ok";
    case TB_PATH_INVALID:
        return "invalid path";
    case TB_PATH_BUFFER_TOO_SMALL:
        return "buffer too small";
    case TB_PATH_DIFFERENT_ROOT:
        return "different roots";
    case TB_PATH_ESCAPE:
        return "path escapes its root";
    case TB_PATH_TOO_LARGE:
        return "too large";
    }
    return "unknown result";
}
