/* Private to the engine: what the allocators built on badger/memory.h share
 * with it. Not installed and not part of the public API. */
#ifndef TB_CORE_MEMORY_INTERNAL_H
#define TB_CORE_MEMORY_INTERNAL_H

#include <stddef.h>

/* Reports a request that a fail-fast allocation function could not satisfy to
 * the memory failure handler, then ends the process. Every allocator in the
 * engine fails this way, so an application sees one kind of report whichever
 * of them ran out.
 *
 * count: number of elements requested; 1 for a plain allocation.
 * size: bytes per element.
 * alignment: requested alignment, or 0 for the default alignment. */
_Noreturn void tb_memory_fail(size_t count, size_t size, size_t alignment);

#endif
