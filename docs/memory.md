# Memory allocation

All C code in the project allocates through `include/badger/memory.h`. Nothing
else calls `malloc`, `free`, or any other allocator, and a check in CI enforces
that. One entry point is what makes it possible to count allocations, to decide
in one place what running out of memory means, and to swap the allocator.

The API reference is the documented header
[`include/badger/memory.h`](../include/badger/memory.h).

## Which function to use

| Need | Fails fast | Reports failure |
| --- | --- | --- |
| Uninitialized bytes | `tb_malloc(size)` | `tb_try_malloc(size)` |
| Zero-filled array | `tb_calloc(count, size)` | `tb_try_calloc(count, size)` |
| One zero-filled object | `TB_NEW(type)` | |
| Zero-filled array of objects | `TB_NEW_ARRAY(type, count)` | |
| Resize a block | `tb_realloc(memory, size)` | `tb_try_realloc(memory, size)` |
| Resize an array | `tb_realloc_array(memory, count, size)` | `tb_try_realloc_array(memory, count, size)` |
| Aligned bytes | `tb_aligned_alloc(alignment, size)` | `tb_try_aligned_alloc(alignment, size)` |
| Release | `tb_free(memory)`, or `tb_aligned_free(memory)` for aligned blocks | |

## Fail fast by default

The functions in the middle column never return NULL. When memory runs out they
report the request and end the process. Use them almost everywhere:

```c
vertex *vertices = TB_NEW_ARRAY(vertex, vertex_count);

fill_vertices(vertices, vertex_count);
```

There is no error path to write, to test, or to get wrong. This is the choice
most engines make. A game that cannot allocate a few hundred bytes cannot do
anything useful about it, and code that checks every allocation is mostly
untested error handling.

## Recovering, when it is possible

Use the `tb_try_*` functions where a failed allocation has a sensible answer:
typically a large request whose size comes from outside the program, such as an
asset the loader can refuse. They return NULL and call no handler.

```c
static bool load_texture(const texture_header *header, texture *loaded)
{
    unsigned char *pixels = NULL;
    bool succeeded = false;

    pixels = tb_try_calloc(header->pixel_count, header->bytes_per_pixel);
    if (pixels == NULL)
    {
        goto cleanup;
    }
    ...
    loaded->pixels = pixels;
    pixels = NULL;
    succeeded = true;

cleanup:
    tb_free(pixels);
    return succeeded;
}
```

When a resize fails, the original block is untouched and still yours. Keep the
old pointer until you know the new one is good:

```c
resized = tb_try_realloc(buffer, new_size);
if (resized == NULL)
{
    goto cleanup; /* buffer is still valid and still needs freeing */
}
buffer = resized;
```

## Sizes

- **Zero is served as one.** `tb_malloc(0)` and `tb_calloc(0, 0)` return a
  unique pointer that must be freed. The C library leaves this to each
  platform; here it is the same everywhere. `tb_realloc(memory, 0)` returns a
  valid block and never frees.
- **Array arithmetic is checked.** `tb_calloc`, `tb_realloc_array`, and the
  `TB_NEW_ARRAY` macro multiply `count` by `size` with an overflow check before
  anything is allocated. Prefer them to multiplying sizes yourself.

## Aligned blocks

`tb_aligned_alloc(alignment, size)` returns a block at any power-of-two
alignment, for SIMD types and hardware buffers. Release it with
`tb_aligned_free`, never with `tb_free`, and do not resize it.

## Replacing the allocator

An application can supply the four functions every allocation goes through,
in the manner of SDL's `SDL_SetMemoryFunctions`:

```c
static const tb_memory_functions functions = {.malloc_function = my_malloc,
                                              .calloc_function = my_calloc,
                                              .realloc_function = my_realloc,
                                              .free_function = my_free};

if (!tb_memory_set_functions(&functions))
{
    /* A member was NULL, or something is already allocated. */
}
```

Install them at startup, before the engine allocates: a block must be released
by the allocator that produced it, so the call is refused while any block is
allocated. The functions have the shapes of the C library's, which lets most
allocators be plugged in directly. The engine guarantees more than the C
library does, to make them easy to write:

- A size is never 0.
- The calloc function is never given a `count * size` that overflows.
- The realloc and free functions are never given NULL.

Aligned allocation is built on these four, so a replacement needs nothing else.
`tb_memory_get_original_functions` returns the built-in ones, for a replacement
that wraps them to add tracking.

## Running out of memory

When a fail-fast function cannot be satisfied, the failure handler is called
with the request (element count, element size, alignment). The default writes
it to stderr and ends the process with `tb_platform_abort()`. Replace it with
`tb_memory_set_failure_handler` to report through the application's own
channels. A handler must not return.

## Accounting

`tb_memory_allocation_count()` is the number of blocks allocated and not yet
freed. A count that is higher at shutdown than at startup is a leak.

## In tests

The test executable builds on the pieces above (see `tests/support/`):

- **Every case is checked for leaks.** A case that passes its checks but ends
  with more blocks allocated than it started with fails, at its definition.
- **Exhaustion is a test result.** An exhausted fail-fast allocation becomes a
  trap, so `TB_TEST_EXPECT_TRAP(body, context, "out of memory")` can prove it.
- **Allocations can be made to fail.** `tb_engine_test_fail_allocations_after(n)`
  lets `n` more attempts succeed and fails the rest, until the case ends or
  `tb_engine_test_stop_failing_allocations()` is called. Looping `n` from zero
  upward walks a recoverable operation through a failure at every step; with
  the leak check, that proves each early exit cleans up.

## The allocation check

`tools/check_allocations.py` reads every C source and header in
`cmake/FormatSources.cmake` and fails on any use of an allocation function from
the C library or the operating system: `malloc`, `calloc`, `realloc`, `free`,
`strdup`, `alloca`, `aligned_alloc`, `_aligned_malloc`, `HeapAlloc`,
`VirtualAlloc`, `mmap`, and their relatives. It reports each use like a compiler
error:

```
src/core/table.c:88: error: malloc is not allowed here; use tb_malloc, tb_calloc, tb_realloc, or tb_free (see docs/memory.md)
```

It runs in CI and as the CTest test `source_checks.allocations`, so
`ctest --preset debug` catches a violation locally. Comments and string literals
are ignored. The names are reserved everywhere else: a variable or structure
member called `free` is reported too, so pick another name.

A file that must call an allocator directly is authorized by name in the
`AUTHORIZED` table of the script, with its reason. Two files are: the engine's
allocation functions themselves, and the test framework's allocation helper,
which must not depend on the engine it tests. There is no in-source
suppression.

## Not here yet

Arena allocation for objects that share a lifetime, and a debugging allocator
that records call sites and detects overruns, build on this layer and are
tracked as their own issues.
