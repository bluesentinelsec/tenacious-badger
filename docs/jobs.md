# The job system

The job system runs work on every core: a pool of worker threads, and tasks
that they take from one another. [AGENTS.md](../AGENTS.md) puts it first among
the engine's parallel machinery: ECS systems, render-graph recording, culling,
and asset decoding are all to be tasks.

| Piece | Where |
| --- | --- |
| The scheduler: workers, dependencies, priorities | `include/badger/jobs.h` |
| Loops whose iterations run in parallel | `tb_jobs_for`, in the same header |
| The queue that a worker keeps its work in | `src/jobs/deque.h`, private |

## The scheduler

Public declaration: `include/badger/jobs.h`.

```c
tb_jobs *const jobs = tb_jobs_create(NULL);
tb_job cull = tb_jobs_run(jobs, cull_objects, &scene);
tb_job animate = tb_jobs_run(jobs, animate_skeletons, &scene);

/* Drawing waits for both. */
const tb_job needs[] = {cull, animate};
const tb_job_description draw = {.function = record_draws,
                                 .argument = &scene,
                                 .dependencies = needs,
                                 .dependency_count = 2};

tb_jobs_wait(jobs, tb_jobs_submit(jobs, &draw));
tb_jobs_destroy(jobs);
```

| Function | What it does |
| --- | --- |
| `tb_jobs_create`, `tb_jobs_destroy` | Start a pool of workers; finish every job, stop the workers, and release it. |
| `tb_jobs_submit` | Submit a job: a function, an argument, a priority, and the jobs it waits for. |
| `tb_jobs_run` | Submit a foreground job with no dependencies. |
| `tb_jobs_wait` | Return when a job has finished, running other jobs meanwhile. |
| `tb_jobs_wait_for_all` | Return when nothing is unfinished. |
| `tb_jobs_is_finished` | Whether a job has finished. |
| `tb_jobs_worker_count` | How many worker threads there are. |

Everything is thread-safe and may be called from inside a job, except
creating and destroying a scheduler.

**A job is a function and an argument.** It runs once, on a worker thread or
on a thread that is waiting. Whatever the argument points to must stay valid
until the job has finished.

**A handle never dangles.** `tb_job` names a job by a slot and a generation.
When the job finishes the handle is out of date, and stays so when the slot
is given to a later job: waiting for it returns at once, and a job that
depends on it does not wait. Nothing releases a handle, and it may be copied
and kept. A zeroed handle names no job.

**A job can wait for others.** It starts only when every job it depends on has
finished. A job with no function finishes when its dependencies have, which
is the way to wait for many jobs at once:

```c
const tb_job_description all = {.dependencies = pieces, .dependency_count = piece_count};

tb_jobs_wait(jobs, tb_jobs_submit(jobs, &all));
```

**Two priorities.** A worker takes every foreground job that is ready before
any background one. Background is for loading, streaming, and baking. A
foreground job that depends on a background job waits as long as the
background job does.

**Waiting helps.** `tb_jobs_wait` runs jobs that are ready, any of them, until
the one it waits for has finished, and sleeps only when there is nothing to
run. So a job may wait for jobs it submitted, and every worker may do so at
once, without the pool running out of threads. Two things follow:

- A wait can return some time after its job finished, when the thread was in
  the middle of another job.
- Each job that a waiting thread runs is on top of the waiter's stack. Jobs
  that wait inside jobs that wait use stack for every level; workers have
  1 MiB.

**A job must not wait for itself, or for a job that depends on it.** Neither
can finish. This is not checked.

**Destroying a scheduler finishes its work first**: every job that is
outstanding, and whatever those jobs go on to submit.

| Option | Default | |
| --- | --- | --- |
| `worker_count` | One for each logical processor the process may use, less one, and at least one | The thread that submits and waits runs jobs too, so the count of threads at work is one more. |
| `queue_capacity` | 256 | Jobs a worker keeps to itself for each priority, a power of two. More than that go to a queue that all threads share. |

Workers are named `badger-worker-0`, `badger-worker-1`, and so on, for
debuggers and crash reports.

**Limits.** About a million jobs may be unfinished at once, and four million
dependencies. Beyond that the process ends with a diagnostic, as it does when
memory runs out.

### How it is put together

A job lives in a **slot** from submission until it has finished. Slots are
made in blocks of 1,024 that never move, and a finished job's slot is used
again. Each use has its own generation.

A job that is ready to run is in one of three kinds of place:

- **The queue of the worker that made it ready**, described
  [below](#the-queue-that-workers-steal-from): no lock, newest first for its
  owner, oldest first for thieves.
- **A queue that all threads share**, for jobs made ready by a thread that is
  not a worker, and for what a worker's own queue has no room for.
- **Nowhere**, while it waits for the jobs it depends on. Each of those keeps
  a list of the jobs that wait for it, and makes them ready as it finishes.

There is one of each queue for each priority. A thread that wants a job looks
at its own foreground queue, the shared foreground queue, and the other
workers' foreground queues, and only then at the background ones in the same
order.

**A worker that has work touches nothing that another thread writes.** That
is the aim of the whole design, and where it was missed the benchmarks showed
it at once:

| A worker | So that |
| --- | --- |
| Takes jobs from its own queue | No lock for the common case. |
| Takes the slots for new jobs from a store of its own, 64 at a time | The lock on the shared store is taken once for 64 jobs. |
| Counts what it submits and finishes in counters of its own | No count that every thread writes for every job. Whether anything is unfinished is found by adding them up, when someone asks. |
| Takes jobs from the shared queue 17 at a time | Its lock is taken once for 17 jobs. |

The shared queue is two lists. A thread that makes a job ready adds it to the
*arrivals* with one compare-and-exchange and no lock, so a thread that submits
many jobs is never made to wait by the threads that take them. A thread that
takes a job takes it from the *departures* under a lock, and when those run
out it takes all the arrivals at once and turns them round, oldest first.

What different threads write is kept a cache line apart in the scheduler's
own structures.

### Sleeping and waking

Putting a thread to sleep and waking it costs several microseconds, which is
far more than a small job. The scheduler therefore sleeps reluctantly and
wakes sparingly.

- **A thread with nothing to run goes on looking** through the queues for
  100 microseconds before it sleeps. Between two looks it tells the processor
  that it is waiting, with `tb_thread_relax`.
- **While one thread is looking, a job that becomes ready wakes nobody.** The
  one that is looking will find it.
- **One idle worker looks at a time.** The others sleep as soon as one look
  has found nothing. Idle workers that all keep looking do not find work
  sooner; they get in the way of the thread that is trying to give them some.
- **A thread that finds a job, when nobody else is looking and more jobs are
  ready, wakes one sleeper** to look in its place. A burst of jobs so wakes
  the workers one after another.

No wake is lost. A thread that is about to sleep counts itself, and only then
looks once more for a job. A thread that makes a job ready does that first,
and only then looks at the counts. Whichever looks second sees what the other
did. The tests submit 3,000 jobs one at a time to idle workers, with the test
itself running nothing, and every one of them has to be picked up.

### What a job costs

`tb_bench jobs`, Release, MSVC 19.50, i9-14900HX, seven workers and the
submitting thread on the eight performance cores. Each job adds one to an
atomic counter, so the numbers are nearly all scheduler.

| Case | Nanoseconds for each job |
| --- | --- |
| The work alone, with no scheduler | 5 |
| A tree of jobs that each submit two more, run by 8 threads | 52 to 56 |
| A batch submitted by one thread, run by 8 | 200 to 240 |
| A job submitted and waited for, one at a time | 240 to 260 |
| A chain of jobs that each wait for the one before | 490 to 500 |

The tree is the pattern the scheduler is built for: jobs made by workers,
kept in their own queues, and stolen when a worker is idle. The others pass
every job from one thread to another, which costs the transfer of several
cache lines whatever the scheduler does.

**A job should be worth its cost.** At these numbers a job that does less
than a microsecond of work is mostly overhead. Give a job a batch of small
items, not one.

## Loops

Most parallel work in an engine is one function applied to a range: the
chunks of an archetype, the objects to cull, the rows of an image.
`tb_jobs_for` runs the iterations of such a loop on the workers and on the
calling thread, and returns when all of them are done.

```c
static void scale_some(void *argument, size_t first, size_t count)
{
    float *const values = argument;

    for (size_t index = first; index < first + count; index++)
    {
        values[index] *= 2.0f;
    }
}

tb_jobs_for(jobs, value_count, 0, scale_some, values);
```

**The body is given a range, not one iteration**, so that its call is paid
for once for many. The iterations `0` to `count - 1` are divided into ranges
of at most `grain` iterations, and every iteration is in exactly one.

**Ranges are handed out as threads become free.** The loop keeps a count of
the iterations given out so far, and each thread that takes part claims the
next range by moving it on. A thread that gets slow iterations therefore does
not hold the others up, and nothing is decided in advance about which thread
does what. The calling thread takes part, and so does a job for each worker.

**The grain is the most iterations in one range.** With `0` the scheduler
chooses about four ranges for each thread. Choose it yourself when you know
what an iteration costs: a range should be worth a microsecond or more of
work, and there should be several ranges for each thread.

**The ranges run in no particular order, at the same time.** The body must
not depend on the order, and iterations in different ranges must not write
the same memory. To gather a result, give each range its own place to put it,
or add it up with an atomic value.

**A loop may run inside a job, and inside the body of another loop.** It
waits by running jobs, as every wait in the scheduler does.

| `tb_bench jobs`, nanoseconds for each iteration | Plain loop | `tb_jobs_for` on 8 threads | |
| --- | --- | --- | --- |
| Add up numbers, 4,000,000 iterations | 0.15 | 0.035 | 4.3 times as fast |
| 32 steps of arithmetic, 1,000,000 iterations | 25.3 | 4.4 | 5.7 times as fast |

Release, MSVC 19.50, i9-14900HX, on the eight performance cores. The first
loop is limited by how fast memory can be read, which eight threads share.

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
is not a public header, and the scheduler is the only thing that uses it.

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
returns false when there is no room. The scheduler puts such a job in the
queue that all threads share. A queue that grew would have to keep its old
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
