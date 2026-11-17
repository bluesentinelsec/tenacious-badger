# The job system

The job system runs work on every core: a pool of worker threads, and tasks
that they take from one another. [AGENTS.md](../AGENTS.md) puts it first among
the engine's parallel machinery: ECS systems, render-graph recording, culling,
and asset decoding are all to be tasks.

It is being built in pieces. This page describes what exists.

| Piece | State |
| --- | --- |
| The queue that a worker keeps its work in | Here: `src/jobs/deque.h` |
| The scheduler: workers, dependencies, priorities | Next |
| Loops whose iterations run in parallel | After the scheduler |

## The queue that workers steal from

Each worker thread has a queue of its own. It adds the tasks it creates to the
bottom and takes its next task from the bottom, newest first, so the task it
runs next is the one whose data it touched last. A worker that has run out of
work becomes a thief: it takes the oldest task from the top of some other
worker's queue.

```
 top                      bottom
  v                         v
  [ oldest | ... | newest ]
  ^ thieves steal here      ^ the owner pushes and pops here
```

**Nothing locks.** The owner works at one end and thieves at the other, and
they meet only when the queue is down to its last item. A worker therefore
pays for the other threads only when one of them is idle, which is what lets a
scheduler scale with the number of cores.

This is the deque of Chase and Lev, *Dynamic Circular Work-Stealing Deque*
(2005), with a fixed capacity. It is private to the engine: `src/jobs/deque.h`
is not a public header, and the scheduler will be the only thing that uses it.

| Function | Who may call it | What it does |
| --- | --- | --- |
| `tb_job_deque_init` | Before the queue is shared | Makes an empty queue over storage that the caller owns. |
| `tb_job_deque_push` | The owning thread | Adds the newest item. Returns false when the queue is full. |
| `tb_job_deque_pop` | The owning thread | Takes the newest item, or returns NULL. |
| `tb_job_deque_steal` | Any thread | Takes the oldest item. Returns NULL when the queue was seen empty, or when another thread took the item first. |
| `tb_job_deque_count` | Any thread | Items in the queue, as a reading that may already be out of date. |

Every item that is pushed comes out exactly once, to the owner or to one
thief.

### How it stays correct

The queue is two counts and an array. `top` counts the oldest item and
`bottom` one past the newest; an item's place in the array is its count modulo
the capacity, which is a power of two.

- **Only the owner writes `bottom`.** It sees it exactly.
- **`top` only rises.** A thief raises it to take the oldest item, and the
  owner raises it to take the last one. Both do so with a compare-and-exchange,
  so of any number of threads after the same item, one succeeds.
- **The owner's view of `top` is never too high.** It may be out of date, so
  the owner can find the queue full or empty a moment early, never late.

The one hard case is the last item, which the owner at the bottom and a thief
at the top both reach for:

1. The owner lowers `bottom` past the item, **then** reads `top`.
2. A thief reads `top`, **then** reads `bottom`.

One of the two must see what the other did. If the thief read `bottom` before
the owner lowered it, the owner's later reading of `top` shows the thief's
claim, or the two meet at the compare-and-exchange on `top` and one loses. If
the owner lowered `bottom` first, the thief sees a queue with nothing left for
it. This is the one place where the queue needs the full strength of
sequentially consistent operations, and the comment in `tb_job_deque_pop` marks
it.

**A full queue refuses.** The capacity is fixed, and `tb_job_deque_push`
returns false when there is no room. The scheduler will run such a task at
once in place of queueing it. A queue that grew would have to keep its old
array alive for as long as a thief might still be reading it; refusing avoids
that question.

**The counts wrap around.** They are unsigned and all arithmetic on them is
modulo their size, so nothing depends on where they start or on their never
overflowing. The tests start them just below the largest count to cross it.

**An item is a pointer that is not NULL.** NULL is what "nothing" looks like.

### How it is tested

`tests/jobs/deque_test.c`, in `tb_internal_tests`:

- **Against a queue too simple to be wrong.** 20,000 random pushes, pops, and
  steals on one thread, compared answer by answer with an array and two
  counts, at every capacity from 1 to 64, and with the counts starting at
  seven places including each side of the wrap.
- **An owner and seven thieves at once.** The owner pushes 200,000 items,
  popping half of them back, while the thieves steal. Every item must have
  been received exactly once, and some must have been stolen. The same is run
  with an owner that never pops by choice and one that pops after nearly every
  push.
- **A queue of one slot,** where every item is the last one and the owner and
  the thieves race for each.

CI runs all of it under [ThreadSanitizer](testing.md#data-races) on Linux and
macOS.

`tb_internal_tests` is the executable for what the engine library does not
export. It is built from the engine's sources in place of linking the library,
so a case there can include a private header such as `jobs/deque.h`.
