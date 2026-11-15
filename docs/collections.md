# Collections

The engine's general-purpose containers. Each has one public header under
`include/badger/`, which is its API reference.

| Collection | Header | Use it for |
| --- | --- | --- |
| [Dynamic array](#dynamic-array) | `array.h` | Elements of one type, in order, stored contiguously. The default choice. |

## What they have in common

- **The caller chooses the allocator.** A collection that owns memory takes a
  `const tb_allocator *` when it is created and gets all of its memory there.
  NULL is the engine's heap. See
  [Choosing the allocator of one object](memory.md#choosing-the-allocator-of-one-object).
- **Create and destroy.** `tb_<collection>_create` returns an object the caller
  owns, and `tb_<collection>_destroy` releases it. Destroying NULL does nothing.
  A collection in an arena need not be destroyed: the arena releases it.
- **Elements are plain bytes.** A collection copies elements with `memcpy` and
  never runs code for them. An element that owns something else, such as a
  block from `tb_malloc`, must have that released before the element is
  removed or the collection destroyed.
- **Running out of memory is fatal**, as everywhere in the engine. Where a
  caller can sensibly recover, a collection offers one fallible function,
  such as `tb_array_try_reserve`, to get the memory first.
- **Misuse is a contract violation.** An index out of range, a NULL collection,
  or a pop from an empty collection fails a `TB_ASSERT` in builds with
  assertions and is undefined in builds without. These are programming errors,
  not conditions to handle.
- **No locks.** A collection is externally synchronized: the caller serializes
  every call that names the same object.

## Dynamic array

`tb_array` holds elements of one type by value, one after another, and grows as
elements are added. Iterating it is as fast as memory allows, and an element
costs exactly its own size.

```c
tb_array *const scores = TB_ARRAY_CREATE(NULL, int);

TB_ARRAY_PUSH(scores, int, 40);
TB_ARRAY_PUSH(scores, int, 2);
for (size_t index = 0; index < tb_array_length(scores); index++)
{
    total += *TB_ARRAY_AT(scores, int, index);
}
tb_array_destroy(scores);
```

| Need | Use |
| --- | --- |
| Create | `TB_ARRAY_CREATE(allocator, type)`, or `tb_array_create(allocator, size, alignment)` |
| Add at the end | `TB_ARRAY_PUSH(array, type, value)`, `tb_array_push(array, &element)`, `tb_array_append(array, elements, count)` |
| Read or write in place | `TB_ARRAY_AT(array, type, index)`, `TB_ARRAY_DATA(array, type)` |
| Copy an element out or in | `tb_array_get(array, index, &element)`, `tb_array_put(array, index, &element)` |
| Add in the middle | `tb_array_insert(array, index, &element)` |
| Remove, keeping order | `tb_array_remove(array, index, &element)` |
| Remove, in constant time | `tb_array_remove_unordered(array, index, &element)` |
| Remove from the end | `tb_array_pop(array, &element)` |
| Change the length | `tb_array_resize(array, length)`, `tb_array_clear(array)` |
| Control the storage | `tb_array_reserve`, `tb_array_try_reserve`, `tb_array_shrink_to_fit` |
| Duplicate | `tb_array_copy(array, allocator)` |

**Prefer the typed macros.** The functions work on `void *` and an element
size. The `TB_ARRAY_*` macros name the element type, return typed pointers, and
in builds with assertions check that the type has the size the array was
created with, which catches an array used as the wrong type.

**Keep an index, not a pointer.** A pointer to an element, or to the storage, is
valid until the array next grows, shrinks its storage, or is destroyed, and any
function that adds an element may grow it. To keep pointers valid across a run
of additions, reserve the final size first.

**Growth.** The capacity doubles when it runs out, starting at eight elements,
so appending one element at a time copies each element a constant number of
times on average. The capacity never shrinks unless `tb_array_shrink_to_fit` is
called. An array in an arena that is the arena's most recent block grows in
place and copies nothing.

**Removing.** `tb_array_remove` moves every later element down, which costs time
in proportion to their number. When the order of elements does not matter,
`tb_array_remove_unordered` moves the last element into the gap instead.
