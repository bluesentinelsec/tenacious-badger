/* The checker's foundation: diagnostics, the names in scope, and the entry
 * point. */
#include "shader_check_internal.h"

#include "badger/arena.h"
#include "badger/fmt.h"

#include <stdarg.h>
#include <string.h>

/* The longest qualified name looked up without allocating. */
#define LOOKUP_BUFFER_SIZE 256

/* -------------------------------------------------------------------------
 * Diagnostics.
 * ------------------------------------------------------------------------- */

static void report(tb_shader_checker *checker, tb_shader_severity severity,
                   tb_shader_location location, const char *format, va_list arguments)
{
    if (checker->stopped)
    {
        return;
    }
    const char *const message = tb_fmt_vstring(&checker->allocator, format, arguments);
    const tb_shader_diagnostic diagnostic = {severity, checker->files[location.file], location.line,
                                             location.column, message};
    (void)tb_array_push(checker->diagnostics, &diagnostic);
    if (severity != TB_SHADER_ERROR)
    {
        return;
    }
    checker->error_count++;
    if (checker->error_count >= TB_SHADER_MAX_ERRORS)
    {
        const tb_shader_diagnostic last = {TB_SHADER_ERROR, diagnostic.path, location.line,
                                           location.column, "too many errors; stopping"};
        (void)tb_array_push(checker->diagnostics, &last);
        checker->stopped = true;
    }
}

void tb_shader_check_error(tb_shader_checker *checker, tb_shader_location location,
                           const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    report(checker, TB_SHADER_ERROR, location, format, arguments);
    va_end(arguments);
}

void tb_shader_check_warning(tb_shader_checker *checker, tb_shader_location location,
                             const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    report(checker, TB_SHADER_WARNING, location, format, arguments);
    va_end(arguments);
}

/* -------------------------------------------------------------------------
 * Names.
 * ------------------------------------------------------------------------- */

typedef struct scope_change
{
    const tb_shader_name *key;
    tb_shader_symbol *previous; /* NULL when the name was not in scope. */
} scope_change;

tb_shader_name tb_shader_check_join(tb_shader_checker *checker, tb_shader_name left,
                                    tb_shader_name right)
{
    const size_t length = left.length + 2 + right.length;
    char *const text = tb_arena_alloc(checker->arena, length + 1, 1);
    memcpy(text, left.text, left.length);
    memcpy(text + left.length, "::", 2);
    memcpy(text + left.length + 2, right.text, right.length);
    text[length] = '\0';
    const tb_shader_name joined = {text, length};
    return joined;
}

tb_shader_symbol *tb_shader_check_new_symbol(tb_shader_checker *checker, tb_shader_symbol_kind kind,
                                             tb_shader_name name, tb_shader_node *declaration)
{
    tb_shader_symbol *const symbol = TB_ARENA_NEW(checker->arena, tb_shader_symbol);
    symbol->kind = kind;
    symbol->name = name;
    symbol->declaration = declaration;
    symbol->type = tb_shader_error_type(checker->types);
    return symbol;
}

size_t tb_shader_check_push_scope(tb_shader_checker *checker)
{
    checker->scope_depth++;
    return tb_array_length(checker->scope_changes);
}

void tb_shader_check_pop_scope(tb_shader_checker *checker, size_t mark)
{
    while (tb_array_length(checker->scope_changes) > mark)
    {
        scope_change change = {0};
        tb_array_pop(checker->scope_changes, &change);
        if (change.previous != NULL)
        {
            (void)tb_table_put(checker->locals, change.key, change.previous);
        }
        else
        {
            (void)tb_table_remove(checker->locals, change.key, NULL);
        }
    }
    checker->scope_depth--;
}

static bool declare_local(tb_shader_checker *checker, tb_shader_symbol *symbol,
                          tb_shader_location location)
{
    tb_shader_symbol *const previous = tb_table_get(checker->locals, &symbol->name);
    if (previous != NULL && previous->scope_depth == checker->scope_depth)
    {
        tb_shader_check_error(checker, location, "redefinition of '%.*s'", (int)symbol->name.length,
                              symbol->name.text);
        return false;
    }
    symbol->scope_depth = checker->scope_depth;
    const scope_change change = {&symbol->name, previous};
    (void)tb_array_push(checker->scope_changes, &change);
    (void)tb_table_put(checker->locals, &symbol->name, symbol);
    return true;
}

bool tb_shader_check_declare(tb_shader_checker *checker, tb_shader_symbol *symbol,
                             tb_shader_location location)
{
    if (symbol->name.length == 0)
    {
        return true;
    }
    if (checker->scope_depth > 0)
    {
        return declare_local(checker, symbol, location);
    }
    if (checker->path.length > 0)
    {
        symbol->name = tb_shader_check_join(checker, checker->path, symbol->name);
    }
    if (tb_table_get(checker->globals, &symbol->name) != NULL)
    {
        tb_shader_check_error(checker, location, "redefinition of '%.*s'", (int)symbol->name.length,
                              symbol->name.text);
        return false;
    }
    (void)tb_table_put(checker->globals, &symbol->name, symbol);
    return true;
}

static tb_shader_symbol *lookup_global(tb_shader_checker *checker, tb_shader_name prefix,
                                       tb_shader_name name)
{
    if (prefix.length == 0)
    {
        return tb_table_get(checker->globals, &name);
    }
    char buffer[LOOKUP_BUFFER_SIZE] = {0};
    const size_t length = prefix.length + 2 + name.length;
    if (length > sizeof(buffer))
    {
        const tb_shader_name joined = tb_shader_check_join(checker, prefix, name);
        return tb_table_get(checker->globals, &joined);
    }
    memcpy(buffer, prefix.text, prefix.length);
    memcpy(buffer + prefix.length, "::", 2);
    memcpy(buffer + prefix.length + 2, name.text, name.length);
    const tb_shader_name joined = {buffer, length};
    return tb_table_get(checker->globals, &joined);
}

static tb_shader_name outer_prefix(tb_shader_name prefix)
{
    size_t length = prefix.length;
    while (length >= 2 && !(prefix.text[length - 2] == ':' && prefix.text[length - 1] == ':'))
    {
        length--;
    }
    const tb_shader_name outer = {prefix.text, length >= 2 ? length - 2 : 0};
    return outer;
}

/* `Counter::Twice`: a static method, named by its struct's name; NULL when
 * the name names no such method. */
static tb_shader_symbol *static_method(tb_shader_checker *checker, tb_shader_name name)
{
    const tb_shader_name owner_name = outer_prefix(name);
    if (owner_name.length == 0)
    {
        return NULL;
    }
    const tb_shader_name method = {name.text + owner_name.length + 2,
                                   name.length - owner_name.length - 2};
    const tb_shader_symbol *const owner = tb_shader_check_lookup(checker, owner_name);
    if (owner == NULL || owner->kind != TB_SHADER_SYMBOL_TYPE ||
        owner->type->type_class != TB_SHADER_CLASS_STRUCT)
    {
        return NULL;
    }
    tb_shader_symbol *const member = tb_shader_find_member(owner->type, method);
    const bool is_static = member != NULL && member->kind == TB_SHADER_SYMBOL_FUNCTION &&
                           member->declaration != NULL &&
                           (member->declaration->modifiers & TB_SHADER_MODIFIER_STATIC) != 0;
    return is_static ? member : NULL;
}

tb_shader_symbol *tb_shader_check_lookup(tb_shader_checker *checker, tb_shader_name name)
{
    if (name.length > 2 && name.text[0] == ':' && name.text[1] == ':')
    {
        const tb_shader_name global = {name.text + 2, name.length - 2};
        return tb_table_get(checker->globals, &global);
    }
    tb_shader_symbol *const local = tb_table_get(checker->locals, &name);
    if (local != NULL)
    {
        return local;
    }
    if (checker->method_owner != NULL)
    {
        tb_shader_symbol *const member = tb_shader_find_member(checker->method_owner, name);
        if (member != NULL)
        {
            return member;
        }
    }
    tb_shader_name prefix = checker->path;
    for (;;)
    {
        tb_shader_symbol *const found = lookup_global(checker, prefix, name);
        if (found != NULL)
        {
            return found;
        }
        if (prefix.length == 0)
        {
            return static_method(checker, name);
        }
        prefix = outer_prefix(prefix);
    }
}

/* -------------------------------------------------------------------------
 * Checking a shader.
 * ------------------------------------------------------------------------- */

tb_shader_result tb_shader_check(tb_shader_syntax *syntax, tb_arena *arena,
                                 tb_shader_checked *checked)
{
    tb_shader_checker checker = {0};
    checker.arena = arena;
    checker.allocator = tb_arena_allocator(arena);
    checker.types = tb_shader_types_create(arena);
    checker.files = syntax->files;
    checker.diagnostics = TB_ARRAY_CREATE(&checker.allocator, tb_shader_diagnostic);
    tb_array_append(checker.diagnostics, syntax->diagnostics, syntax->diagnostic_count);
    checker.error_count = syntax->error_count;
    checker.stopped = syntax->error_count >= TB_SHADER_MAX_ERRORS;
    checker.globals =
        tb_table_create(&checker.allocator, 256, tb_shader_name_hash, tb_shader_name_equal);
    checker.locals =
        tb_table_create(&checker.allocator, 64, tb_shader_name_hash, tb_shader_name_equal);
    checker.scope_changes = TB_ARRAY_CREATE(&checker.allocator, scope_change);

    tb_shader_check_declarations(&checker, syntax->declarations);

    checked->types = checker.types;
    checked->diagnostics = tb_array_data(checker.diagnostics);
    checked->diagnostic_count = tb_array_length(checker.diagnostics);
    checked->error_count = checker.error_count;
    return checker.error_count > 0 ? TB_SHADER_FAILED : TB_SHADER_OK;
}
