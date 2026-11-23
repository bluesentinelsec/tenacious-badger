/* The lowerer's state and the functions its files share. */
#ifndef TB_SHADER_LOWER_INTERNAL_H
#define TB_SHADER_LOWER_INTERNAL_H

#include "shader_lower.h"

#include "badger/allocator.h"
#include "badger/array.h"
#include "badger/table.h"

/* What a name of the shader's is in the form. */
typedef enum tb_shader_binding_kind
{
    TB_SHADER_BINDING_LOCAL,
    TB_SHADER_BINDING_GLOBAL,
    TB_SHADER_BINDING_ARGUMENT /* An `out` or `inout` parameter: a pointer. */
} tb_shader_binding_kind;

typedef struct tb_shader_lowerer
{
    tb_arena *arena;
    tb_allocator allocator;
    tb_shader_form *form;
    const char *const *files;
    tb_array *diagnostics;
    size_t error_count;

    /* The form's index of each checked type, function and variable. */
    tb_table *types;
    tb_table *functions;
    tb_table *bindings;

    /* The function being lowered. */
    tb_shader_form_function *function;
    uint32_t emit_start; /* The first expression no EMIT has evaluated yet. */
    uint32_t block;      /* The block statements are added to. */
    uint32_t last;       /* Its last statement, or NONE. */
    bool ended;          /* The block ended in a jump; what follows is never run. */
} tb_shader_lowerer;

/* Diagnostics. */
void tb_shader_lower_unsupported(tb_shader_lowerer *lowerer, tb_shader_location location,
                                 const char *what);

/* Maps from what checking made to what the form holds. */
void tb_shader_lower_bind(tb_shader_lowerer *lowerer, tb_table *map, const void *key,
                          uint32_t value);
/* NONE when `key` is not bound. */
uint32_t tb_shader_lower_bound(const tb_shader_lowerer *lowerer, const tb_table *map,
                               const void *key);
/* A binding of a variable: its kind and index. */
uint32_t tb_shader_lower_binding(tb_shader_binding_kind kind, uint32_t index);
tb_shader_binding_kind tb_shader_lower_binding_kind(uint32_t binding);
uint32_t tb_shader_lower_binding_index(uint32_t binding);

/* The form's type of a checked type, a literal made concrete; NONE, after
 * reporting why, for a type the form does not hold yet. */
uint32_t tb_shader_lower_type(tb_shader_lowerer *lowerer, const tb_shader_type *type,
                              tb_shader_location location);

/* Statements: added to the current block, after an EMIT of what was made
 * before them. */
uint32_t tb_shader_lower_statement(tb_shader_lowerer *lowerer,
                                   const tb_shader_form_statement *statement);
/* Evaluate what was made since the last EMIT, here. */
void tb_shader_lower_flush(tb_shader_lowerer *lowerer);
/* A new block; statements go into it until it is closed, which restores
 * where they went before. */
typedef struct tb_shader_block_mark
{
    uint32_t block;
    uint32_t last;
    bool ended;
} tb_shader_block_mark;
uint32_t tb_shader_lower_open_block(tb_shader_lowerer *lowerer, tb_shader_block_mark *mark);
void tb_shader_lower_close_block(tb_shader_lowerer *lowerer, const tb_shader_block_mark *mark);

/* Expressions (shader_lower_expression.c). */
uint32_t tb_shader_lower_add(tb_shader_lowerer *lowerer,
                             const tb_shader_form_expression *expression);
uint32_t tb_shader_lower_type_of(const tb_shader_lowerer *lowerer, uint32_t expression);
uint32_t tb_shader_lower_local(tb_shader_lowerer *lowerer, uint32_t local);
uint32_t tb_shader_lower_argument(tb_shader_lowerer *lowerer, uint32_t parameter);
uint32_t tb_shader_lower_load(tb_shader_lowerer *lowerer, uint32_t pointer);
uint32_t tb_shader_lower_not(tb_shader_lowerer *lowerer, uint32_t value);
void tb_shader_lower_store(tb_shader_lowerer *lowerer, uint32_t pointer, uint32_t value);
/* The value of an expression; NONE after reporting why it cannot be. */
uint32_t tb_shader_lower_value(tb_shader_lowerer *lowerer, tb_shader_node *node);
/* An expression whose value is not used: a statement's. */
void tb_shader_lower_effect(tb_shader_lowerer *lowerer, tb_shader_node *node);
/* A variable's initial value of `type`: an expression or a `{...}` list. */
uint32_t tb_shader_lower_initializer(tb_shader_lowerer *lowerer, tb_shader_node *node,
                                     const tb_shader_type *type);

#endif
