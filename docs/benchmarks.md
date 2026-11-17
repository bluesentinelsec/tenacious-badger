# Benchmarks

`tb_bench` times the public operations of the foundation modules and of the
platform's threads, locks, and atomic values. It is how a
change that claims to make something faster shows it, and how a change that
makes something slower is noticed. It is built with the project, in C17, with
no dependencies, like everything else here.

## Running

Benchmark a Release build. A Debug build measures its assertions.

```sh
cmake -S . -B build/local-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/local-release --config Release --parallel
build/local-release/bin/tb_bench            # every module
build/local-release/bin/tb_bench table fmt  # only these
```

Each line of output is a module, a case, who performed it, and the nanoseconds
one operation took, separated by tabs:

```
fmt	a log line: 2 strings, 3 ints	badger	124.30
fmt	a log line: 2 strings, 3 ints	snprintf	226.66
table	1,000 pointers: find a key that is present	badger	5.53
```

| Option | Effect |
| --- | --- |
| `--repeats N` | Run every case `N` times; the default is 9. |
| `--quick` | Run every case once at a small size, to check that it runs. |
| `--help` | List the options and the modules. |

## Reading the numbers

**The fastest run is reported.** Other programs, interrupts, and a processor
that slows itself down can only make a run slower, so the fastest of several
runs is the best estimate of what the code costs.

**Compare cases that ran together.** Cases that are meant to be compared, an
engine function and the C library function it stands in for, are run in turn,
round after round, so that a change in the speed of the machine falls on both.
Their ratio is trustworthy on a machine where the absolute numbers are not.

**Compare two builds on one machine, back to back.** To measure a change, build
the benchmark before and after it and run the two alternately a few times,
keeping the fastest number of each. Numbers from different machines, or from
the same machine an hour apart, say little about each other.

**Laptops and busy machines mislead.** A machine on battery, a hot one, or one
with a game running can halve the speed of a benchmark between two runs. If
the numbers of one build change from run to run by more than a few percent,
the machine is not quiet enough for small differences to mean anything.

## Comparing two builds

`tools/compare_benchmarks.py` does what the paragraph above asks: it runs two
builds of `tb_bench` alternately, round after round, keeps the fastest time of
each case, and lists the cases that are slower or faster by more than a
margin, and the ones that are new or gone.

```sh
python3 tools/compare_benchmarks.py --before old/bin/tb_bench --after build/local-release/bin/tb_bench table fmt
```

| Option | Effect |
| --- | --- |
| `--rounds N` | Run each build `N` times in turn; the default is 3. |
| `--repeats N` | Passed to `tb_bench`; the default is 3. |
| `--margin R` | Report a case whose time changed by more than this ratio; the default is 1.25. |
| `--summary FILE` | Append the report to a file as Markdown. |
| `--fail-above R` | End with a failure status when a case is slower by more than this ratio. |
| `--results A B` | Compare two saved outputs of `tb_bench` instead of running anything. |

**CI compares every pull request with the branch it goes into.** The job
"Benchmarks against the base" builds both on one runner, on Linux with GCC and
on Windows with MSVC, and writes the comparison to the page of the run. It
does not fail: a hosted runner is shared with other work, and one case in a
hundred moves by a quarter between two runs of the same code. A change that
was meant to be faster or slower says so in its pull request, with numbers
from a quiet machine.

No baseline is kept in the repository. A recorded number is a number from one
machine at one time, and the next runner is another machine.

## Cases on several threads

A case can run on several threads at once: eight threads that all want one
mutex, or that all allocate. Its name says how many.

```
thread	lock and unlock a mutex nobody else wants	badger	10.58
thread	lock and unlock a mutex on 8 threads at once	badger	315.20
```

**The number is still the time one thread took for one operation.** Each
thread performs the same count of operations, the harness lets all of them go
at the same moment, and the time until the last has finished is divided by
what one thread did. So a case on eight threads that reports the same number
as the case on one has threads that do not get in each other's way, and one
that reports eight times as much is doing no more work than one thread would.
Above eight times, the threads are slower together than one after another.

Two things follow:

- **The contended numbers move from run to run** far more than the others,
  by a factor of two on a quiet machine. Which thread gets the lock next is
  the system's choice. Read them for their size, not their digits.
- **The machine needs as many free cores as the case has threads.** With
  fewer, the number measures the system's scheduler.

## What is not tested

No test depends on a timing: neither a developer's machine in the middle of a
build nor a hosted CI runner is quiet enough. CTest runs `tb_bench --quick`,
which fails when a benchmark crashes or stops working, on every platform.

## Adding a benchmark

The benchmarks of a module are in `benchmarks/core/<module>_bench.c`, or in
`benchmarks/platform/` for a platform interface, behind
one function that `benchmarks/harness/tb_bench.c` calls. A case is a function
that performs a known number of operations:

```c
static void run_push(void *context)
{
    const array_state *const state = context;
    tb_array *const array = TB_ARRAY_CREATE(NULL, int);

    for (size_t index = 0; index < state->elements; index++)
    {
        TB_ARRAY_PUSH(array, int, (int)index);
    }
    tb_bench_keep(tb_array_length(array));
    tb_array_destroy(array);
}
```

- **Take sizes from `tb_bench_size()`,** which gives the full size, or a small
  one in quick mode.
- **Give every result to `tb_bench_keep()`,** or the compiler may remove the
  work that produced it.
- **Prepare outside the timed function.** A case has an optional setup and
  teardown that run around every timed run and are not timed.
- **Measure rivals in one call** of `tb_bench_measure()`, and unrelated cases
  in separate calls.
- **Time enough work.** A run should take milliseconds, not microseconds:
  repeat a small operation until it does.
- **To run a case on several threads, set `threads`.** `operations` is then
  what one thread performs. Every thread is given the same context, and
  `tb_bench_thread_number()` tells a thread which one it is. `tb_bench_keep()`
  may be called from any of them; the setup and teardown still run once.

For a new module, add its file to `benchmarks/CMakeLists.txt`, and its
function to `benchmarks/harness/tb_bench.h` and to the table of modules in
`benchmarks/harness/tb_bench.c`.
