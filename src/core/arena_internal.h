#ifndef TB_ARENA_INTERNAL_H
#define TB_ARENA_INTERNAL_H
#include "badger/arena.h"
#include "badger/virtual_memory.h"
/* Per-arena native operations let fault tests refuse a particular commit
 * without replacing process-wide memory behavior or depending on exhaustion. */
typedef struct tb_arena_vm_operations
{
    tb_vm_result (*reserve)(size_t, tb_vm_region *);
    tb_vm_result (*commit)(tb_vm_region *, size_t, size_t);
    tb_vm_result (*decommit)(tb_vm_region *, size_t, size_t);
    tb_vm_result (*release)(tb_vm_region *);
} tb_arena_vm_operations;
tb_arena *tb_arena_try_create_reserved_with_operations(size_t size,
                                                       const tb_arena_vm_operations *operations);
#endif
