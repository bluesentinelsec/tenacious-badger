/* Private to the engine: tells AddressSanitizer which bytes are in use inside
 * a block that an engine allocator manages by itself.
 *
 * The sanitizer tracks blocks from the C library's allocator. It cannot see
 * that an arena has released part of a chunk, so a stale pointer into that
 * part would read and write without complaint. Poisoning the bytes makes any
 * access to them a `use-after-poison` report.
 *
 * TB_SANITIZE_ADDRESS is defined by the build for targets compiled with
 * AddressSanitizer (cmake/Sanitizers.cmake). In every other build these
 * functions do nothing. */
#ifndef TB_CORE_SANITIZER_H
#define TB_CORE_SANITIZER_H

#include <stddef.h>

#if defined(TB_SANITIZE_ADDRESS)
#include <sanitizer/asan_interface.h>
#endif

/* Marks bytes as not in use: the sanitizer reports any access to them.
 * Poisoned bytes must be unpoisoned before they are written, handed to code
 * outside the engine, or returned to the allocator they came from. */
static inline void tb_sanitizer_poison(const void *memory, size_t size)
{
#if defined(TB_SANITIZE_ADDRESS)
    __asan_poison_memory_region(memory, size);
#else
    (void)memory;
    (void)size;
#endif
}

/* Marks bytes as in use again. */
static inline void tb_sanitizer_unpoison(const void *memory, size_t size)
{
#if defined(TB_SANITIZE_ADDRESS)
    __asan_unpoison_memory_region(memory, size);
#else
    (void)memory;
    (void)size;
#endif
}

#endif
