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

Objects that share a lifetime are better served by an [arena](#arenas).

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
- **Compute other sizes with `badger/size.h`.** A size built from parts, such
  as a header plus a payload whose length comes from a file, can wrap past
  `SIZE_MAX` to a small number. `tb_size_add` and `tb_size_multiply` return
  false instead of wrapping and leave their output untouched:

  ```c
  size_t total_size = {0};

  if (!tb_size_multiply(row_count, row_size, &total_size) ||
      !tb_size_add(total_size, sizeof(table_header), &total_size))
  {
      goto cleanup; /* the file describes a table that cannot exist */
  }
  ```

  `tb_size_grow` chooses the next capacity of a buffer that grows by doubling,
  limited to what `size_t` can express.

## Aligned blocks

`tb_aligned_alloc(alignment, size)` returns a block at any power-of-two
alignment, for SIMD types and hardware buffers. Release it with
`tb_aligned_free`, never with `tb_free`, and do not resize it.

## Arenas

Most objects in a game die in groups: everything a frame allocated, everything
a level loaded, the tree a parser built to produce one result. An arena
(`include/badger/arena.h`) serves such a group from large chunks by advancing a
position, and releases all of it in one call. Allocation costs a few
instructions, nothing is freed object by object, and one object cannot leak.

```c
tb_arena *const arena = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
token *const tokens = TB_ARENA_NEW_ARRAY(arena, token, token_count);
char *const name = tb_arena_alloc(arena, name_length + 1, 1);

parse(tokens, token_count, name, arena);
tb_arena_destroy(arena); /* tokens, name, and all that parse() allocated */
```

| Need | Fails fast | Reports failure |
| --- | --- | --- |
| An arena that grows | `tb_arena_create(chunk_size)` | `tb_arena_try_create(chunk_size)` |
| An arena in your own buffer | `tb_arena_create_fixed(buffer, size)` | |
| Uninitialized bytes | `tb_arena_alloc(arena, size, alignment)` | `tb_arena_try_alloc(arena, size, alignment)` |
| Zero-filled array | `tb_arena_calloc(arena, count, size, alignment)` | `tb_arena_try_calloc(arena, count, size, alignment)` |
| One zero-filled object | `TB_ARENA_NEW(arena, type)` | |
| Zero-filled array of objects | `TB_ARENA_NEW_ARRAY(arena, type, count)` | |
| Release everything, keep the memory | `tb_arena_reset(arena)` | |
| Release back to a point | `tb_arena_get_mark(arena)`, `tb_arena_rewind(arena, mark)` | |
| Release everything and the arena | `tb_arena_destroy(arena)` | |

Every allocation names its alignment, which must be a power of two; the typed
macros pass `_Alignof(type)`. A size of zero is served as one byte, as
everywhere else, so a result is always a unique pointer.

### Lifetimes

An object from an arena is valid until the arena is destroyed, reset, or
rewound to a mark taken before the object was allocated. There is no way to
free one object, and no need to.

The arena runs no code for the objects it releases. An object that owns
something outside the arena, such as a file or a block from `tb_malloc`, must
give that up before the arena lets go of it. Objects that only point at other
objects in the same arena need nothing.

Give each arena one owner, and pass it to the functions that allocate from it.
A function that receives an arena allocates results the caller will keep; for
its own temporary memory it uses a [mark](#marks).

### Growing arenas

`tb_arena_create(chunk_size)` allocates one chunk of `chunk_size` bytes from
the engine's allocation functions and adds another whenever an object does not
fit. An object larger than a chunk gets a chunk of exactly its own size, so any
size can be served. The room left in a chunk when the arena moves on is not
used until the next reset or rewind; choose a chunk size that holds many
typical objects and the waste stays small.

The chunks come from whatever allocator is installed. An application that
[replaces the allocator](#replacing-the-allocator) supplies the memory of every
arena as well, one call per chunk, and gets each chunk back when its arena is
destroyed.

`tb_arena_reset` keeps every chunk. An arena that is reset and refilled with
similar work, such as one reset at the start of each frame, grows to its
working size and then stops allocating:

```c
tb_arena_reset(frame_arena);
build_draw_list(scene, frame_arena);
```

### Fixed arenas

`tb_arena_create_fixed(buffer, size)` builds an arena inside memory you already
have: a static array, a stack array, a slice of a larger block. It never calls
the allocation functions and never grows; when the buffer is full,
`tb_arena_alloc` ends the process and `tb_arena_try_alloc` returns NULL. The
arena's own bookkeeping takes a small part of the buffer, and
`tb_arena_capacity` reports what is left for objects.

```c
unsigned char scratch[4096] = {0};
tb_arena *const arena = tb_arena_create_fixed(scratch, sizeof(scratch));

format_message(arena, message);
tb_arena_destroy(arena);
```

Destroy a fixed arena before its buffer is reused or goes out of scope. Nothing
is freed, but under AddressSanitizer that call is what makes the buffer
ordinary memory again.

### Marks

A mark remembers a position; rewinding to it releases everything allocated
since. This is how a function borrows temporary memory from an arena it was
given without disturbing what the caller keeps there:

```c
static size_t count_unique_names(const entry *entries, size_t entry_count, tb_arena *arena)
{
    const tb_arena_mark mark = tb_arena_get_mark(arena);
    name_set *const seen = name_set_create(arena, entry_count);
    size_t unique_count = {0};

    ...
    tb_arena_rewind(arena, mark); /* seen is gone; earlier objects are not */
    return unique_count;
}
```

Marks nest. Rewinding to a mark releases every mark taken after it, and the
mark itself stays valid until the arena is reset or rewound to an earlier one.

### When an arena cannot supply memory

The arena follows the rule of the rest of this page. `tb_arena_create`,
`tb_arena_alloc`, `tb_arena_calloc`, and the typed macros never return NULL: a
full fixed arena, an exhausted allocator, and a size that cannot be expressed
all go to the [failure handler](#running-out-of-memory) with the request. The
`tb_arena_try_*` forms return NULL instead and leave the arena exactly as it
was, so it can still serve whatever fits.

### Finding stale pointers

A pointer into an arena stays a valid address after its object is released,
which makes such bugs quiet. Two things make them loud:

- **In builds with assertions**, memory is filled with `TB_ARENA_FILL_BYTE`
  (`0xDD`) before it is handed out, the first time and after every release. An
  object read before it is initialized, or through a pointer that outlived a
  reset, shows `0xDD` in every byte instead of plausible data. Optimized builds
  skip the fill.
- **Under AddressSanitizer**, the arena tells the sanitizer which of its bytes
  belong to a live object. Touching a released object, or the unused room after
  the last object, ends the process with a `use-after-poison` report.

Rewinding to a mark that was already released, or that came from another arena,
is a contract violation. Builds with assertions catch it wherever the arena's
state still shows it.

### Threading

An arena has no lock. Give each thread its own, or serialize every call that
names the same arena. Creating and growing an arena uses the allocation
functions, which are thread-safe.

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

## Choosing the allocator of one object

Replacing the allocator changes where the whole engine gets its memory. The
allocator interface (`include/badger/allocator.h`) chooses it for one object at
a time. A module that owns memory takes a `const tb_allocator *` when one of
its objects is created and uses it for everything that object allocates:

```c
static vertex *copy_vertices(const tb_allocator *allocator, const vertex *source, size_t count)
{
    vertex *const copy = TB_ALLOCATOR_NEW_ARRAY(allocator, vertex, count);

    memcpy(copy, source, count * sizeof(*copy));
    return copy;
}

const tb_allocator in_frame = tb_arena_allocator(frame_arena);
vertex *const scratch = copy_vertices(&in_frame, vertices, count); /* in the arena */
vertex *const kept = copy_vertices(NULL, vertices, count);         /* on the heap */
```

- **NULL is the engine's heap**: the functions at the top of this page,
  including a replacement installed with `tb_memory_set_functions`. Code that
  does not care where its memory comes from passes NULL.
- **`tb_arena_allocator(arena)`** puts the object in an arena. Releasing a
  block does nothing; the memory comes back when the arena is reset, rewound,
  or destroyed, and the object need not be destroyed first. The most recent
  block grows in place, so appending to one array in an arena copies nothing.
- **Your own allocator** is a function and a context pointer:

  ```c
  static void *pool_function(void *context, void *memory, size_t old_size, size_t new_size,
                             size_t alignment)
  {
      level_pool *const pool = context;

      if (new_size == 0)
      {
          level_pool_release(pool, memory, old_size);
          return NULL;
      }
      return level_pool_resize(pool, memory, old_size, new_size, alignment);
  }

  const tb_allocator in_level = {.function = pool_function, .context = &level->pool};
  ```

  The one function allocates (`memory` is NULL), resizes, and releases
  (`new_size` is 0). It is always told the size and alignment the block was
  given, so an allocator needs no header on its blocks, and it is never asked
  for a block of zero bytes. It returns NULL when it cannot supply the memory.

An allocator is copied by whoever receives it, so the structure can be a local
variable. What its context points to must outlive every block.

Code that takes an allocator allocates through `tb_allocator_alloc`,
`tb_allocator_calloc`, `tb_allocator_realloc`, and `tb_allocator_free`, or the
typed `TB_ALLOCATOR_NEW` and `TB_ALLOCATOR_DELETE` forms. They fail fast like
the rest of this page, and each has a `tb_allocator_try_*` form that returns
NULL. Releasing takes the size of the block, which the owner of a block knows.

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
- **A module can be handed an allocator that checks it.** `tb_test_allocator`
  (`tests/support/test_allocator.h`) is a `tb_allocator` that records every
  block. It reports a block resized or released with the wrong size or
  alignment, counts what is still live, moves every block it resizes so that a
  stale pointer is an invalid access, and can refuse requests on demand. The
  tests of a module that takes an allocator use it to prove that the module
  allocates nowhere else and releases everything.

## The allocation check

`tools/check_foundation.py` fails on any use of an allocation function from
the C library or the operating system: `malloc`, `calloc`, `realloc`, `free`,
`strdup`, `alloca`, `aligned_alloc`, `_aligned_malloc`, `HeapAlloc`,
`VirtualAlloc`, `mmap`, and their relatives. It reports each use like a compiler
error:

```
src/core/table.c:88: error: malloc is not allowed here; use tb_malloc, tb_calloc, tb_realloc, or tb_free (see docs/memory.md)
```

It runs in CI and with the tests. Two files are authorized to call an
allocator directly: the engine's allocation functions themselves, and the test
framework's allocation helper, which must not depend on the engine it tests.

Allocation is one of the rules that the check enforces.
[The foundation check](development.md#the-foundation-check) describes all of
them, how exceptions are granted, and what the check does not report.

## Not here yet

- A debugging allocator that records call sites and detects overruns in
  individual blocks (#31).
- A scratch arena for each thread. A variable for each thread is available
  ([platforms.md](platforms.md#threads-mutexes-and-condition-variables)); the
  arena itself is not written.
- Arenas that reserve address space and commit it as they grow, which need the
  platform memory interface (#38).
