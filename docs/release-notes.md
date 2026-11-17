Development release of Tenacious Badger, a dependency-free C17 engine
foundation and its Burrow showcase executable. This project is under construction:
the demo prints `2 + 3 = 5` and exits; there is no renderer or playable game yet.

Windows builds now automatically create/reuse a local signing identity and sign
project-owned executables and DLLs after linking. An existing certificate can be
selected by thumbprint, and signing can be disabled explicitly. CI verifies
signatures, rejects tampering, and checks both controls.

The engine now provides developer assertions in `badger/assert.h`: `TB_ASSERT`
and `TB_ASSERT_MSG` for invariants that are checked in Debug and compiled out of
Release, `TB_STATIC_ASSERT` for build-time checks, and a replaceable failure
handler.

Memory allocation goes through `badger/memory.h`. `tb_malloc`, `tb_calloc`,
`tb_realloc`, and `tb_aligned_alloc` never return NULL: running out of memory
ends the process through a replaceable handler. The `tb_try_*` forms return NULL
for callers that can recover. An application can supply its own allocator with
`tb_memory_set_functions`.

Sizes are computed safely with `badger/size.h`: `tb_size_add` and
`tb_size_multiply` report overflow instead of wrapping, and `tb_size_grow`
chooses the next capacity of a buffer that grows by doubling.

Objects that share a lifetime can be allocated from an arena with
`badger/arena.h` and released together. An arena grows in chunks or lives in a
caller's buffer, can be reset for reuse or rewound to a mark, and follows the
same fail-fast rule as the other allocation functions. Builds with assertions
fill released memory with a recognizable byte, and AddressSanitizer reports any
use of it.

Where one object gets its memory is chosen with `badger/allocator.h`. An
allocator is a function and a context pointer, passed to the object that uses
it: NULL selects the engine's heap, `tb_arena_allocator` an arena, and an
application can supply its own.

The first collections are in place, each taking the allocator it uses:

- `badger/array.h`: a dynamic array that stores elements of one type by value,
  contiguously, and grows by doubling.
- `badger/bit.h`: a bit vector of fixed length with range changes, searches for
  the next set or clear bit, and the set operations.
- `badger/ring.h`: a queue that adds and removes at both ends in constant time,
  with indexed access and rotation.
- `badger/list.h`: an intrusive doubly linked list, whose elements join and
  leave in constant time without allocation.
- `badger/table.h`: a hash table from keys to values, with atoms, integers,
  strings, or keys of the caller's own kind.
- `badger/set.h`: an unordered set with the same kinds of member, and union,
  intersection, difference, and symmetric difference as new sets.

Strings are UTF-8, handled as bytes without decoding:

- `badger/str.h`: searching, comparing, and building NUL-terminated strings,
  with zero-based half-open ranges and results allocated from any allocator.
- `badger/text.h`: a text is a length and a pointer, so a substring copies
  nothing; it needs no terminator and can be formatted with `%T`.
- `badger/fmt.h`: printf-style formatting to a buffer, a new string, the
  standard streams, or any function, with the same output on every platform
  and conversions an application can add. Floating point is not converted yet.
- `badger/atom.h`: atoms are strings stored once, so that equal names are the
  same pointer and compare in one step. `badger/hash.h` has the hash functions
  behind them: XXH64 for bytes and the SplitMix64 finalizer for integers and
  addresses.

The foundation modules were measured with `tb_bench` and made faster where the
measurements showed waste:

- Adding a key to a hash table or a set hashes the key and searches for it
  once, not twice: about 1.7 times as fast for a table with room, more for
  string keys.
- Bytes are hashed with XXH64 in place of FNV-1a, several bytes at a step: an
  identifier hashes in less than half the time and a long key in a fifteenth,
  which makes every table and set keyed by strings, and every atom, faster.
  The values of `tb_hash_bytes` and `tb_hash_string` changed with it.
- An atom table carves its atoms from shared blocks and searches once for a
  new name: interning a new name is 1.6 to 3 times as fast, and destroying a
  table of 100,000 atoms 16 times.
- Formatting an integer divides by a constant and takes decimal digits two
  at a time, and `tb_fmt_string` formats short text once, not twice: both are
  1.7 times as fast. A custom conversion is now called once for a new string
  shorter than 256 bytes.
- Searching text for a pattern looks for the pattern's rarest byte, not its
  first, and no text makes it slow: six times as fast when the first byte is
  common, as a space is. Searching backward for a byte is five times as fast,
  and trimming a short line twice.
- `tb_bit_next_set` steps through the set bits of a bit vector with a cursor
  that keeps its place, 1.7 to 4.4 times as fast as searching from each
  result, and combining two bit vectors is 2.3 times as fast.
- Inserting into the middle of a ring, or removing from it, moves the elements
  as blocks of memory: 33 times as fast among 4,096 elements.
- The count of live blocks is kept in a piece for each thread, so threads that
  allocate at the same time no longer wait for one another to update it:
  eight threads allocating at once are ten times as fast.
- Release builds of the shared library are optimized across source files at
  link time where the toolchain supports it, which makes small operations
  that cross modules 15% to 80% faster. It is automatic.

Code is held to the foundation layer. `tools/check_foundation.py`, run as a CI
job and with the tests, fails on any use of a C library function that a
foundation module replaces, such as `malloc`, `printf`, `strlen`, `assert`, or
a native clock, and names the replacement. The engine, the benchmarks, and
the tests were converted to pass it: failed assertions and exhausted memory
are now reported through `badger/fmt.h`. `tb_fmt_flush` delivers what has been
printed to the standard streams, which the program's own `fflush` cannot do
where the engine is a shared library with a C library of its own.

The engine has a clock on every platform. `tb_platform_clock_nanoseconds`
reads a monotonic clock in nanoseconds on Windows, Linux, and macOS. It
replaces `tb_platform_ticks`, whose ticks had no stated length and which ended
the process on Linux and macOS. `tb_bench` times with it.

Code can now run on several threads. `badger/thread.h` starts and joins
threads and provides a mutex and a condition variable, on Windows, Linux, and
macOS. A mutex or a condition variable is a plain object that is ready when it
is zero-initialized and needs no destruction, so one can be a global or a
field of any structure. A wait can be given a time limit. For code that must
not lock, `badger/platform.h` adds an atomic 32-bit value and an atomic
pointer, each with load, store, exchange, and compare-and-exchange.

Windows, Linux, and macOS each implement every platform interface. Any other
platform now fails by design: configuring the build stops with a message that
names the platform and says how to port to it, and a default adapter, built
only on request with `-DTB_USE_DEFAULT_PLATFORM_ADAPTER=ON`, reports "not
implemented" from every function and ends the process.

The tests run under ThreadSanitizer in CI, on Linux with GCC and Clang and on
macOS, so a data race in the engine or in a platform adapter fails the build
whether or not it did harm on that run. `-DTB_ENABLE_THREAD_SANITIZER=ON`, or
the `thread-sanitizer` preset, builds them locally. MSVC has no
ThreadSanitizer, so the Windows adapter is not covered.

The thread and atomic functions check their contracts in builds with
assertions, the same on every platform. A null pointer, unlocking a mutex
that the calling thread does not hold, locking one twice, waiting on a
condition without holding the mutex, and joining a thread that is not there
each fail an assertion where they were undefined behavior before. A mutex
records which thread holds it for this, and is one pointer larger in every
build.

The engine can count the processors it may run on. `tb_system_get_processors`
in `badger/system.h` reports the logical processors, the cores, and the cores
of the fastest kind, on Windows, Linux, and macOS, for the process as the
system confines it.

A thread can be given a name, a stack size, and a priority.
`tb_thread_create_with_options` takes them, and `tb_thread_current_name`
reads a thread's name back from the system. Debuggers and crash reports show
the name. A `tb_thread` is 40 bytes larger than before, for the name
it carries to its thread.

`tb_bench` can run a case on several threads at once, and measures what was
only known from experiments that were not kept: a mutex with and without
contention, a turn passed between two threads, the atomic operations, and
allocation from eight threads at once beside the C library's.

Reading an atomic value on Windows no longer writes to it. A load took 6
nanoseconds, and took the value's cache line from every other thread that
was reading it, so that eight threads reading one value each waited 70 to
165 nanoseconds. A load now takes 0.8 nanoseconds, and 1.3 with eight
threads reading at once.

The engine has a job system. `badger/jobs.h` runs jobs on a pool of worker
threads: a job is a function and an argument, can wait for other jobs, and is
foreground or background. Waiting for a job runs other jobs meanwhile, so a
job may wait for the jobs it submitted. A handle to a job that has finished
is harmless, and nothing has to release one. Workers keep their jobs in
queues of their own, with no lock, and idle workers steal from them. An
atomic size can now be stored, exchanged, and compared-and-exchanged, which
those queues need. `tb_jobs_for` runs the iterations of a loop on every
thread, handing out ranges of them as threads become free.

Included foundations:

- `badger` dynamic library and `badger_static` static library with the same
  documented, overflow-checked C17 module API.
- Separate `burrow` consumer and an in-house test harness.
- `tb_bench`, an in-house benchmark of the foundation modules, run at a small
  size by the tests on every platform.
- A monotonic clock in nanoseconds, native on Windows, Linux, and macOS.
- Threads, mutexes, and condition variables, native on the same three.
- Debug/Release CI, formatting, and standalone sanitizer tests on all three OSes.
- Native desktop exports with icons, metadata, relative library paths, and
  extraction-and-launch checks in CI.

Downloads:

- **Windows x86_64:** extract the ZIP and run `burrow.exe` in a terminal.
  Windows 10 or newer; the standard C runtime is statically linked. The executable
  is Authenticode-signed with a self-created local code-signing certificate.
  The public certificate and signing metadata are included; no public trust is implied.
- **macOS Apple silicon:** extract the ZIP and run
  `Burrow.app/Contents/MacOS/burrow` in Terminal to see the output. Requires macOS
  13 or newer. The app is ad-hoc signed, without Developer ID signing or Apple
  notarization; macOS may require explicit approval for a downloaded app.
- **Linux x86_64:** extract the tarball and run `./burrow`. Built on Ubuntu 26.04;
  the archive's `BUILD-INFO.json` records the libc baseline. Desktop entry,
  AppStream metadata, and icons are included for optional desktop integration.

Six SDK downloads provide Debug and Release libraries for all three desktop
platforms, including dynamic/static binaries, public headers, CMake package
metadata, and available debug symbols. Each configuration passes Python ctypes
invocation, external static linking, and FetchContent consumption checks. See
[consumption instructions](https://github.com/bluesentinelsec/tenacious-badger/blob/v0.1.0/docs/consuming.md).

Archives include Apache-2.0 licensing, personal attribution to `bluesentinelsec`,
launch instructions, and build metadata. Verify downloads using `SHA256SUMS`.
Browser, Android, and iOS exports are planned for later milestones.
