# Collections

The engine's general-purpose containers. Each has one public header under
`include/badger/`, which is its API reference.

| Collection | Header | Use it for |
| --- | --- | --- |
| [Dynamic array](#dynamic-array) | `array.h` | Elements of one type, in order, stored contiguously. The default choice. |
| [Bit vector](#bit-vector) | `bit.h` | One bit for each of a fixed number of things, with set operations. |
| [Ring](#ring) | `ring.h` | A queue: elements added and removed at both ends in constant time. |
| [List](#list) | `list.h` | Membership: objects that join and leave sets in constant time, without allocation. |

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

The [list](#list) differs in one way: it is intrusive. It owns no memory, takes
no allocator, and is initialized in place instead of created.

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

## Bit vector

`tb_bit` holds a fixed number of bits, one for each of N things: which
components an entity has, which objects are visible, which slots of a pool are
taken. It packs 64 bits to a word, and whole vectors combine a word at a time.

```c
tb_bit *const taken = tb_bit_create(NULL, slot_count);
const size_t slot = tb_bit_find_clear(taken, 0);

if (slot != TB_BIT_NOT_FOUND)
{
    (void)tb_bit_put(taken, slot, true);
}
```

| Need | Use |
| --- | --- |
| Create, copy, destroy | `tb_bit_create(allocator, length)`, `tb_bit_copy(bits, allocator)`, `tb_bit_destroy(bits)` |
| Read or write one bit | `tb_bit_get(bits, index)`, `tb_bit_put(bits, index, value)` |
| Change a range | `tb_bit_set`, `tb_bit_clear`, `tb_bit_flip`, each with `(bits, begin, end)` |
| Count the set bits | `tb_bit_count(bits)` |
| Find the next set or clear bit | `tb_bit_find_set(bits, from)`, `tb_bit_find_clear(bits, from)` |
| Visit every bit | `tb_bit_visit(bits, visitor, context)` |
| Compare two vectors | `tb_bit_equal`, `tb_bit_is_subset`, `tb_bit_is_proper_subset` |
| Combine two vectors | `tb_bit_union`, `tb_bit_intersection`, `tb_bit_difference`, `tb_bit_symmetric_difference` |

**Ranges are half-open.** A range is given as `begin` and `end` and holds the
bits from `begin` up to, but not including, `end`, so `tb_bit_set(bits, 0,
tb_bit_length(bits))` sets every bit and a range with `begin == end` is empty.

**The length is fixed.** A vector is created with its length and allocates once.
The functions that take two vectors require them to have the same length.

**Combining changes the first vector in place** and allocates nothing, which is
what a mask test in a hot loop needs. To keep the original, combine into a copy:

```c
tb_bit *const either = tb_bit_copy(mine, allocator);

tb_bit_union(either, theirs);
```

**To walk the set bits**, search from one past each result. Each search skips
whole words of clear bits:

```c
for (size_t index = tb_bit_find_set(visible, 0); index != TB_BIT_NOT_FOUND;
     index = tb_bit_find_set(visible, index + 1))
{
    draw(objects[index]);
}
```

## Ring

`tb_ring` is a sequence that grows and shrinks at both ends: the collection for
queues. Events waiting to be handled, commands waiting to run, and the last N
samples of a measurement all go in at one end and come out at the other. Both
ends take constant time, because the elements live in a circular buffer whose
start travels around it; nothing between the ends is ever moved.

```c
tb_ring *const pending = TB_RING_CREATE(NULL, event);

TB_RING_PUSH_BACK(pending, event, incoming);
while (tb_ring_length(pending) > 0)
{
    event next = {0};

    tb_ring_pop_front(pending, &next);
    handle(&next);
}
```

| Need | Use |
| --- | --- |
| Create, destroy | `TB_RING_CREATE(allocator, type)`, `tb_ring_destroy(ring)` |
| Add at an end | `TB_RING_PUSH_BACK(ring, type, value)`, `TB_RING_PUSH_FRONT(ring, type, value)` |
| Remove from an end | `tb_ring_pop_front(ring, &element)`, `tb_ring_pop_back(ring, &element)` |
| Read or write by index | `TB_RING_AT(ring, type, index)`, `tb_ring_get`, `tb_ring_put` |
| Add or remove in the middle | `tb_ring_insert(ring, index, &element)`, `tb_ring_remove(ring, index, &element)` |
| Rotate | `tb_ring_rotate_left(ring, count)`, `tb_ring_rotate_right(ring, count)` |
| Control the storage | `tb_ring_reserve`, `tb_ring_try_reserve`, `tb_ring_shrink_to_fit`, `tb_ring_clear` |

**Index 0 is the front.** Elements are numbered from the front, wherever they
are in the buffer. `tb_ring_push_front` makes the new element index 0 and raises
every other index by one.

**The elements are not one block.** The sequence may wrap from the end of the
buffer to its start, so a ring has no pointer to all of its elements. Use a
dynamic array when the elements must be contiguous, and a ring when they come
and go at both ends.

**A queue that stays short never allocates again.** Once the buffer is large
enough for the longest the queue gets, elements go round it indefinitely.
Reserve that length up front to do all the allocation at startup.

**Inserting and removing in the middle** move the elements on the shorter side
of the index, so the cost depends on the distance from the nearer end.

**Rotating left** moves the front element to the back, `count` times: the
element that was at index `count` becomes index 0. A full ring rotates by moving
its start, without touching an element. A ring of clients rotated left by one
gives the next client its turn.

**A ring is not a queue between threads.** It has no lock. A bounded queue for
handing work from one thread to another is a different structure.

## List

`tb_list` is an intrusive doubly linked list: the links live inside the
elements. An element joins a list by embedding a `tb_list_link` in its own
structure, so the list allocates nothing and takes no allocator. Adding and
removing an element rewrites a few pointers, and removal needs only the element
itself, not a search for it.

```c
typedef struct projectile
{
    vector position;
    tb_list_link link; /* membership of the active or the free list */
} projectile;

tb_list active = TB_LIST_INIT(active);

tb_list_push_back(&active, &shot->link);
for (tb_list_link *link = tb_list_first(&active); link != NULL;
     link = tb_list_next(&active, link))
{
    projectile *const each = TB_LIST_ENTRY(link, projectile, link);
    ...
}
tb_list_remove(&shot->link); /* constant time, from wherever it is */
```

| Need | Use |
| --- | --- |
| Make a list empty | `TB_LIST_INIT(list)`, `tb_list_init(&list)` |
| Add at an end | `tb_list_push_front(&list, &link)`, `tb_list_push_back(&list, &link)` |
| Add next to an element | `tb_list_insert_before(&position, &link)`, `tb_list_insert_after(&position, &link)` |
| Remove an element | `tb_list_remove(&link)` |
| Remove from an end | `tb_list_pop_front(&list)`, `tb_list_pop_back(&list)` |
| Walk | `tb_list_first`, `tb_list_last`, `tb_list_next`, `tb_list_previous` |
| Get the element of a link | `TB_LIST_ENTRY(link, type, member)` |
| Measure | `tb_list_is_empty(&list)`, `tb_list_length(&list)`, `tb_list_link_is_linked(&link)` |
| Whole lists | `tb_list_append(&list, &other)`, `tb_list_reverse(&list)`, `tb_list_to_array(&list, links, capacity)` |

**Use it for membership, not storage.** A list suits objects that move between
sets: projectiles between "active" and "free", entities waiting on an event.
Walking a list follows pointers through memory, which is slow for anything
visited every frame. Values that are stored and scanned belong in a dynamic
array.

**One link, one list.** A link is in at most one list at a time. An element that
must be in several lists at once embeds one link for each, and `TB_LIST_ENTRY`
names which.

**Initialize before use.** A list is made empty with `TB_LIST_INIT` or
`tb_list_init`; a zero-initialized `tb_list` is not a valid list. A link starts
zero-initialized, which means "in no list", and `tb_list_remove` returns it to
that state. Builds with assertions use this to catch a link added to a list
twice, removed twice, or used with a list that was never initialized.

**The list borrows its elements.** Remove an element before releasing or
reusing its memory. Do not move or copy a `tb_list` while it has elements: they
point back at it.

**Removing while walking.** Take the next link before removing the current one:

```c
tb_list_link *link = tb_list_first(&active);

while (link != NULL)
{
    tb_list_link *const next = tb_list_next(&active, link);

    if (has_expired(TB_LIST_ENTRY(link, projectile, link)))
    {
        tb_list_remove(link);
    }
    link = next;
}
```

**The length is counted.** `tb_list_length` walks the list. A caller that needs
the length often keeps its own count.
