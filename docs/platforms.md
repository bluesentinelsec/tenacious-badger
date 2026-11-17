# Platform interfaces and bootstrap scope

The binding architecture is in [AGENTS.md](../AGENTS.md). The existing
[vision.md](vision.md) is an earlier draft. Only the clock,
abnormal-termination, atomic-value, thread, and processor-count interfaces
exist so far; the other platform services and rendering backends are still planned. No renderer, window, vendor SDK, or game loop is implemented.

Every interface is implemented on Windows, Linux, and macOS.
[Any other platform](#unsupported-platforms) stops the build, and reports
"not implemented" if it is built anyway.

## Clock

Public declaration: `include/badger/platform.h`,
`tb_platform_clock_nanoseconds(void)`. Returns a reading of a monotonic clock
in nanoseconds. The clock counts from a moment that is fixed while the process
runs and otherwise unspecified, so only the difference between two readings
means anything. It never goes backward, and a change to the date or the time
of day does not move it. It owns no resources and is safe to call from any
thread.

| Platform | Selected implementation | Native clock | While the system is suspended |
| --- | --- | --- | --- |
| Windows | `src/platform/win32/time.c` | `QueryPerformanceCounter`, converted with `QueryPerformanceFrequency` | Keeps counting |
| Linux / Steam Deck | `src/platform/linux/time.c` | `clock_gettime(CLOCK_MONOTONIC)` | Stops |
| macOS | `src/platform/macos/time.c` | `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)`, the counter that `mach_absolute_time` reads | Stops |

The benchmarks time with it everywhere, and each adapter is one call to the
system. A native clock that cannot be read ends the process with a diagnostic that
names the interface (`platform.time`), the function, and the platform.

Only the Windows counter has a frequency of its own, 10 MHz on current
systems, so only that adapter converts. The conversion is exact for as long as
the nanoseconds fit in 64 bits, which is more than 500 years; multiplying a
count by a billion before dividing it would overflow half an hour after the
system starts. A counter faster than 18 GHz cannot be converted this way and
is refused with a diagnostic.

CTest runs the adapter's tests on every platform: readings never go backward,
they advance, and an interval measured with the clock is as long as the C
library's clock says, within a tenth. On Windows the conversion is also
checked against 128-bit arithmetic, at the frequencies counters have had and
at random ones.

CMake selects exactly one adapter. All OS-specific headers and native symbols
stay in that adapter. The public visibility header is generated from the selected
platform template; core code and public interfaces do not detect the OS.
Windows consumers import the DLL API, and the library exports it. Only documented
API symbols have default visibility on Linux/macOS.

The adapters are the layer beneath the foundation modules and use none of
them. Their tests (`tb_platform_tests`) are built from the selected adapter's
sources alone, which keeps that true and lets a test call a function that is
private to an adapter.

The one thing an adapter takes from above is the report of a broken contract:
it states its contracts with `TB_ASSERT`, and a failed one calls
`tb_assert_fail`, which `src/core/assert.c` defines. In the adapters' tests
`tests/support/platform_assert.c` defines it instead, as a trap of the test
framework, so `TB_TEST_EXPECT_TRAP` works there as it does everywhere else.

## Abnormal termination

Public declaration: `include/badger/platform.h`, `tb_platform_abort(void)`.
Ends the process at once after an unrecoverable failure has been reported; the
default assertion handler calls it. It stops in an attached debugger, never
waits on an error dialog, runs no exit handlers, and does not return.

| Platform | Selected implementation | Current behavior |
| --- | --- | --- |
| Windows | `src/platform/win32/abort.c` | Breaks into an attached debugger, then terminates the process with exit status 3. The C runtime's `abort()` is avoided because its debug build opens a modal error window. |
| Linux / Steam Deck | `src/platform/linux/abort.c` | Calls `abort()`: `SIGABRT`, and a core dump where the system is configured for one. |
| macOS | `src/platform/macos/abort.c` | Calls `abort()`: `SIGABRT`, and a crash report where the system is configured for one. |

Reporting any failure needs a way to end the process, so this is also the one
function that the [default adapter](#unsupported-platforms) implements. Crash dumps and stack traces belong to
the planned system-information interface and will hook in here.

A child process checks in CI that a failed assertion reports its location and
ends the process promptly on every platform.

## Atomic values

Public declaration: `include/badger/platform.h`. A value that any thread may
read and write without a lock. Every operation is one indivisible step, and
all of them are sequentially consistent: the threads agree on one order in
which they happened.

| Type | Holds | Operations |
| --- | --- | --- |
| `tb_platform_atomic_size` | A `size_t` | `_add`, `_subtract`, `_load` |
| `tb_platform_atomic_u32` | A `uint32_t` | `_load`, `_store`, `_exchange`, `_compare_exchange` |
| `tb_platform_atomic_pointer` | A `void *` | `_load`, `_store`, `_exchange`, `_compare_exchange` |

Zero-initialize one, then touch it only through its functions once it is
shared. The allocation functions count live blocks with the first.

**Compare and exchange** writes a value only where the caller's expected value
still is, and otherwise reports what it found. It is the strong form: it fails
only when the value differs. With pointers, finding the expected address does
not prove that nothing happened in between, because an object can be released
and another allocated in its place.

**The pointer functions move a pointer and nothing else.** When the object it
points to may be released, while another thread may still be reading it, is
for the caller to arrange.

| Platform | Selected implementation | Current behavior |
| --- | --- | --- |
| Windows | `src/platform/win32/atomic.c` | Interlocked operations from the Windows API. |
| Linux / Steam Deck | `src/platform/linux/atomic.c` | The compiler's atomic builtins (GCC and Clang). |
| macOS | `src/platform/macos/atomic.c` | The compiler's atomic builtins (AppleClang). |

It is a platform interface because MSVC does not offer C11 `<stdatomic.h>` in
C17 mode without an experimental switch. Every allocation depends on it. Weaker orderings, and arithmetic on
the 32-bit value, wait for the code that needs them.

## Threads, mutexes, and condition variables

Public declaration: `include/badger/thread.h`.

| | Functions |
| --- | --- |
| A thread | `tb_thread_create`, `tb_thread_join`, `tb_thread_current_id` |
| A mutex | `tb_mutex_lock`, `tb_mutex_try_lock`, `tb_mutex_unlock` |
| A condition variable | `tb_condition_wait`, `tb_condition_wait_for`, `tb_condition_wake_one`, `tb_condition_wake_all` |

**A mutex and a condition variable are plain objects.** The caller puts one in
a structure or makes it a global, zero-initializes it, and uses it. They hold
no system resource, so there is nothing to create or destroy, and a global one
is ready before any code runs. The engine's allocator can therefore guard
itself with one.

```c
static tb_mutex table_lock = {0};

tb_mutex_lock(&table_lock);
/* ... */
tb_mutex_unlock(&table_lock);
```

**A mutex is not recursive.** A thread that locks one it already holds has
broken the contract. There is no reader/writer form yet.

**Contracts are checked in builds with assertions**, the same on every
platform. Each of these fails a `TB_ASSERT` before anything is changed, where
the system's own answer is undefined and ranges from a corrupted lock to a
thread that never returns:

| Violation | Assertion that fails |
| --- | --- |
| A null pointer where one is required, in any thread or atomic function | `mutex != NULL`, and its like |
| Unlocking a mutex that nobody holds, or that another thread holds | the calling thread holds the mutex |
| Locking a mutex that the calling thread holds | the calling thread does not hold the mutex already |
| Waiting on a condition without holding the mutex | the calling thread holds the mutex |
| Joining a thread that was never created, or twice | the thread was created and is not joined yet |
| Creating a thread over one that is not joined | the thread object holds no thread that is not joined |

To check them, a mutex records which thread holds it. No system says, so the
record is a word of the `tb_mutex`, written as one step because threads that
do not hold the mutex read it. A build without assertions keeps the word and
never touches it: a `tb_mutex` is the same size in Debug and Release, which
one generated header for both requires. Trying a mutex that the caller holds
is allowed, and is refused like any other attempt on a held mutex.

Not checked: a thread that joins itself, a mutex or condition that is moved
while in use, and threads that wait on one condition with different mutexes.

**A wait can end with nobody having woken it.** Look at the state the mutex
guards, in a loop, before going on. `tb_condition_wait_for` adds a time limit,
counted on the engine's [clock](#clock): it reports the limit only when at
least that long has passed, whatever the system's own timer does.

**A variable for each thread is the C keyword `_Thread_local`.** MSVC, GCC,
Clang, and AppleClang all provide it, so it needs no interface of its own. It
starts at zero in every new thread. Nothing runs when a thread ends: a thread
that gives its variable something to release must release it before it
returns.

**A thread has the system's default stack**, which is usually 1 MB on Windows,
8 MB on Linux, and 512 KB on macOS. Stack sizes, names, priorities, and processor
affinity are left for the job system to ask for.

| Platform | Selected implementation | Thread | Mutex | Condition variable |
| --- | --- | --- | --- | --- |
| Windows | `src/platform/win32/thread.c` | `CreateThread` | Slim reader/writer lock, held exclusively | `SleepConditionVariableSRW` |
| Linux / Steam Deck | `src/platform/posix/thread.c`, `src/platform/linux/condition.c` | `pthread_create` | `pthread_mutex_t` | `pthread_cond_t` on `CLOCK_MONOTONIC` |
| macOS | `src/platform/posix/thread.c`, `src/platform/macos/condition.c` | `pthread_create` | `pthread_mutex_t` | `pthread_cond_t`, timed with `pthread_cond_timedwait_relative_np` |

Linux and macOS share one implementation of POSIX threads. They differ only in
how a condition variable waits for a limited time by a clock that is never
set, and each supplies that part.

The Windows objects are ready when they are all zeros. The POSIX ones have to
be made by a function, so on Linux and macOS the first word of a `tb_mutex` or
`tb_condition` records whether that has happened, and the first thread to use
the object makes it. That is why the two types are larger there: 80 and 56
bytes, against 16 and 8 on Windows, the record of who holds the mutex
included. `badger/thread_storage.h`, which the build
selects as it selects `badger/api.h`, tells the public header the size.

A system call that fails, such as one that cannot start another thread, ends
the process with a diagnostic that names the interface (`platform.thread`),
the function, and the platform. Joining a thread twice does the same in a
build without assertions.

On Linux the thread functions must be part of the C library, as they are from
glibc 2.34 on; the build links no separate thread library.

## Processors

Public declaration: `include/badger/system.h`,
`tb_system_get_processors(tb_system_processors *)`. It counts the processors
that the process may run on, which is what a pool of worker threads is sized
from.

| Field | Counts |
| --- | --- |
| `logical_count` | Logical processors: what the system schedules a thread on. |
| `core_count` | Cores that have at least one of them. A core may present two logical processors. |
| `performance_core_count` | Cores of the fastest kind, on a machine that has several kinds. |

```c
tb_system_processors processors = {0};

tb_system_get_processors(&processors);
```

| Machine | `logical_count` | `core_count` | `performance_core_count` |
| --- | --- | --- | --- |
| The development laptop: 8 fast cores of two threads, 16 efficient cores | 32 | 24 | 8 |
| A Steam Deck: 4 cores of two threads | 8 | 4 | 4 |
| An Apple M1 Max: 8 fast cores, 2 efficient | 10 | 10 | 8 |

**The counts are of what the process may use.** A process that the system
confines to some processors counts only those, and a core counts when at
least one of its logical processors is allowed. When none of the fast cores
is allowed, the cores that are allowed are the fastest the process has, and
all of them are performance cores.

**Every count is at least one**, and
`performance_core_count <= core_count <= logical_count`. Where the system
does not say which logical processors share a core, each is a core. Where it
does not say which cores are fast, all of them are.

**A limit on processor time is not counted.** A container given half the time
of four processors has four.

**The system is asked on every call**, because a processor can be taken
offline and a process can be confined while it runs. Read the counts once
where they are needed.

| Platform | Selected implementation | Which processors | Cores and their kinds |
| --- | --- | --- | --- |
| Windows | `src/platform/win32/system.c` | The process's affinity mask | `GetLogicalProcessorInformationEx`: one record for each core, with its efficiency class |
| Linux / Steam Deck | `src/platform/linux/system.c`, `processors.c` | `sched_getaffinity` of the process's first thread | The kernel's files under `/sys/devices/system/cpu`: `thread_siblings_list` for cores, then `/sys/devices/cpu_core/cpus` or `cpu_capacity` for the fast ones |
| macOS | `src/platform/macos/system.c` | Every available one; macOS confines no process | `sysctl`: `hw.logicalcpu`, `hw.physicalcpu`, `hw.perflevel0.physicalcpu` |

Linux confines threads, not processes, so the first thread of the process
stands for it: a limit set by a container or a launcher is its limit, and
every thread starts with it. A thread that the engine pins later does not
change the answer.

On a Windows machine with more than 64 logical processors the system divides
them into groups. A process confined to one group is counted within it; one
that spans groups may run anywhere in them, and all are counted.

On Linux a file that is missing or cannot be understood is something the
system does not say, never an error: a container without `/sys` has cores of
one kind. A machine with more than 1,024 logical processors is counted from
the number that are online.

The counting is apart from the calls that ask the system, on Windows and on
Linux, and takes a description of a machine. The tests give it the
development laptop, an Arm machine with three kinds of core, a virtual
machine that says nothing, processes confined in several ways, and files that
are malformed. A second test confines the real process, asks the real system,
and gives the process back what it had.

## Unsupported platforms

Windows, Linux, and macOS are the supported platforms, and each implements
every interface above. Any other platform fails in two places.

**The build stops.** Configuring for another system ends with an error that
names it and says what to do:

```
Tenacious Badger does not support the platform 'Haiku'. It supports Windows,
Linux, and macOS.

To port it, add a platform adapter in src/platform/<name>/ that implements
every platform interface, and choose it in cmake/PlatformAdapter.cmake;
docs/platforms.md describes the interfaces.

To build before the adapter is finished, configure with
-DTB_USE_DEFAULT_PLATFORM_ADAPTER=ON. Every function of the default adapter
reports "not implemented" and ends the process.
```

**The default adapter reports "not implemented".** `src/platform/default/`
holds every function of every platform interface, in standard C, so it
compiles anywhere. Each one prints the interface, the function, the platform,
and the next step to the standard error stream, then ends the process with a
failure status:

```
platform.thread: tb_mutex_lock on Haiku: not implemented; add a platform adapter for Haiku under src/platform/
```

A port starts from it. The engine then builds for the new platform at once,
and runs until the first call that the port has not written yet, which says
which one it was. None of the functions pretends: a lock that did nothing
would let every thread in, and an atomic operation written in plain C would
work until two threads met in it.

`tb_platform_abort` is the exception. Standard C can end a process, and the
report of every other function depends on it, so the default adapter
implements it with `_Exit`.

| | With the default adapter |
| --- | --- |
| Selected by | `-DTB_USE_DEFAULT_PLATFORM_ADAPTER=ON`, on any system |
| `badger/api.h` | `TB_API` adds nothing; a port that builds a shared library supplies its own |
| `tb_mutex`, `tb_condition` | One pointer-sized word each, unused |

CTest probes the default adapter on every supported platform
(`tests/platform/default_probe.c`, `tests/ExpectPlatformDefault.cmake`). The
script reads the functions from the public headers and runs each in a child
process, so a function that is added to an interface without its default, or
without a probe, fails the test. `tests/ExpectPlatformSelection.cmake` checks
which adapter each system gets, and that any other system stops the build
with the message above. CI also builds the engine with the default adapter on
Linux.

## Future interfaces and capabilities

For each new platform interface, add in one change:

- its public contract, in a header under `include/badger/`;
- an implementation in all three adapters, `src/platform/win32/`,
  `src/platform/linux/`, and `src/platform/macos/`, with tests that run on
  each;
- each function in the default adapter, `src/platform/default/`, with a case
  in `tests/platform/default_probe.c`;
- a new header in `TB_PLATFORM_INTERFACE_HEADERS` in `tests/CMakeLists.txt`,
  which is the list the default adapter is checked against.

No interface is implemented on fewer than the three platforms. Planned
services include windowing/events, input, sleep and frame pacing, virtual
memory, files, dynamic libraries, audio devices, and system/crash information.
Native APIs and future RHI backends are catalogued in the binding vision.

A rendering backend or a vendor plugin is not a platform interface. One that
is not written yet reports "not implemented" when it is selected, in the same
form, on a platform that is otherwise supported. An optional hardware
capability or missing vendor redistributable must instead report itself
unavailable and allow the engine's documented fallback. Do not introduce vendor headers,
redistributables, SDKs, or platform packages without the author's approval.

## Builds

Use the cross-platform commands in [README.md](../README.md). Windows requires
MSVC's C compiler and the Windows SDK. Linux uses GCC or Clang, and macOS uses
AppleClang with its native SDK. Building on Linux or macOS requires no
Vulkan, Metal, Wayland, X11, or audio development packages. CI compiles the shared
library and executable, runs the core tests, and checks the selected adapter on
every platform in Debug and Release. Standalone sanitizer tests run separately.
