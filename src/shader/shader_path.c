/* Virtual shader paths: checking them, and resolving includes against them. */
#include "shader_internal.h"

#include "badger/str.h"

#include <string.h>

static const char hlsl_extension[] = ".hlsl";
static const char hlsli_extension[] = ".hlsli";

/* Components a path may have, which bounds the work of resolving one. */
#define MAX_COMPONENTS ((size_t)256)

static bool ends_with(const char *text, size_t length, const char *suffix)
{
    const size_t size = tb_str_length(suffix);
    return length >= size && memcmp(text + length - size, suffix, size) == 0;
}

static bool is_refused_byte(unsigned char byte)
{
    return byte <= ' ' || byte == 0x7F || byte == '\\' || byte == ':';
}

static bool is_dot_component(const char *component, size_t length)
{
    return (length == 1 && component[0] == '.') ||
           (length == 2 && component[0] == '.' && component[1] == '.');
}

bool tb_shader_path_check(const char *path, size_t length)
{
    if (length < 2 || path[0] != '/')
    {
        return false;
    }
    if (!ends_with(path, length, hlsl_extension) && !ends_with(path, length, hlsli_extension))
    {
        return false;
    }
    size_t components = 0;
    size_t begin = 1;
    for (size_t index = 1; index <= length; index++)
    {
        if (index < length && path[index] != '/')
        {
            if (is_refused_byte((unsigned char)path[index]))
            {
                return false;
            }
            continue;
        }
        const size_t component_length = index - begin;
        if (component_length == 0 || is_dot_component(path + begin, component_length))
        {
            return false;
        }
        components++;
        begin = index + 1;
    }
    return components >= 2;
}

/* The components of a path, as offsets into a text. */
typedef struct components
{
    const char *text[MAX_COMPONENTS];
    size_t length[MAX_COMPONENTS];
    size_t count;
} components;

/* Apply the components of `path` to `stack`: push a name, pop for `..`,
 * skip `.` and empty ones. Returns false when `..` climbs above the root or
 * there are too many components. */
static bool apply(components *stack, const char *path, size_t length)
{
    size_t begin = 0;
    for (size_t index = 0; index <= length; index++)
    {
        if (index < length && path[index] != '/')
        {
            continue;
        }
        const size_t size = index - begin;
        const char *const component = path + begin;
        begin = index + 1;
        if (size == 0 || (size == 1 && component[0] == '.'))
        {
            continue;
        }
        if (size == 2 && component[0] == '.' && component[1] == '.')
        {
            if (stack->count == 0)
            {
                return false;
            }
            stack->count--;
            continue;
        }
        if (stack->count == MAX_COMPONENTS)
        {
            return false;
        }
        stack->text[stack->count] = component;
        stack->length[stack->count] = size;
        stack->count++;
    }
    return true;
}

char *tb_shader_path_resolve(const char *including, const char *include, size_t include_length,
                             tb_arena *arena, size_t *length)
{
    components stack = {0};
    const bool relative = include_length == 0 || include[0] != '/';
    if (relative)
    {
        /* The including file's directory: its path without the last component. */
        const size_t including_length = tb_str_length(including);
        size_t directory_length = including_length;
        while (directory_length > 0 && including[directory_length - 1] != '/')
        {
            directory_length--;
        }
        if (!apply(&stack, including, directory_length))
        {
            return NULL;
        }
    }
    if (!apply(&stack, include, include_length))
    {
        return NULL;
    }
    size_t total = 0;
    for (size_t index = 0; index < stack.count; index++)
    {
        total += 1 + stack.length[index];
    }
    char *const path = tb_arena_alloc(arena, total + 1, 1);
    size_t written = 0;
    for (size_t index = 0; index < stack.count; index++)
    {
        path[written] = '/';
        memcpy(path + written + 1, stack.text[index], stack.length[index]);
        written += 1 + stack.length[index];
    }
    path[written] = '\0';
    if (!tb_shader_path_check(path, written))
    {
        return NULL;
    }
    *length = written;
    return path;
}
