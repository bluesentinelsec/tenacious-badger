# Benchmarks

`tb_bench` times the public operations of the foundation modules. It is how a
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

## What is not tested

No test depends on a timing: neither a developer's machine in the middle of a
build nor a hosted CI runner is quiet enough. CTest runs `tb_bench --quick`,
which fails when a benchmark crashes or stops working, on every platform.

## Adding a benchmark

The benchmarks of a module are in `benchmarks/core/<module>_bench.c`, behind
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

For a new module, add its file to `benchmarks/CMakeLists.txt`, and its
function to `benchmarks/harness/tb_bench.h` and to the table of modules in
`benchmarks/harness/tb_bench.c`.
