# Platform interfaces and bootstrap scope

The binding architecture is in [AGENTS.md](../AGENTS.md). The existing
[vision.md](vision.md) is an earlier draft. Only the clock,
abnormal-termination, atomic-value, and thread interfaces exist so far; the
other platform services and rendering backends are still planned. No renderer, window, vendor SDK, or game loop is implemented.

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

This interface is implemented on all three platforms, not stubbed: the
benchmarks time with it everywhere, and each adapter is one call to the system.
A native clock that cannot be read ends the process with a diagnostic that
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

This interface is implemented on all three platforms, not stubbed: a fatal stub
itself needs a way to end the process. Crash dumps and stack traces belong to
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
C17 mode without an experimental switch. It is implemented on all three
platforms: every allocation depends on it. Weaker orderings, and arithmetic on
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

**A mutex is not recursive.** A thread that locks one it already holds never
returns. There is no reader/writer form yet.

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

This interface is implemented on all three platforms, not stubbed. A lock that
ends the process guards nothing, so a stub would take the tests of every
module that locks off Linux and macOS.

Linux and macOS share one implementation of POSIX threads. They differ only in
how a condition variable waits for a limited time by a clock that is never
set, and each supplies that part.

The Windows objects are ready when they are all zeros. The POSIX ones have to
be made by a function, so on Linux and macOS the first word of a `tb_mutex` or
`tb_condition` records whether that has happened, and the first thread to use
the object makes it. That is why the two types are larger there: 72 and 56
bytes, against 8 and 8 on Windows. `badger/thread_storage.h`, which the build
selects as it selects `badger/api.h`, tells the public header the size.

A system call that fails, such as one that cannot start another thread, ends
the process with a diagnostic that names the interface (`platform.thread`),
the function, and the platform. Joining a thread twice does the same.

On Linux the thread functions must be part of the C library, as they are from
glibc 2.34 on; the build links no separate thread library.

## Future interfaces and capabilities

For each new platform interface, add its public contract and all three adapters
together. Linux/macOS adapters may initially be fatal stubs, but must compile and
have subprocess tests. A fatal stub prints its interface, function, platform,
the native API it will use, and the next action, then exits with failure; a
CTest script launches it in a child process and checks both the exit status
and the diagnostic. No interface is stubbed at present. Planned services
include windowing/events, input, sleep and frame pacing, virtual memory, files,
dynamic libraries, audio devices, and system/crash information. Native APIs
and future RHI backends are catalogued in the binding vision; their
implementation is outside this bootstrap milestone.

An unimplemented required interface is fatal. An optional hardware capability
or missing vendor redistributable must instead report itself unavailable and
allow the engine's documented fallback. Do not introduce vendor headers,
redistributables, SDKs, or platform packages without the author's approval.

## Builds

Use the cross-platform commands in [README.md](../README.md). Windows requires
MSVC's C compiler and the Windows SDK. Linux uses GCC or Clang, and macOS uses
AppleClang with its native SDK. Building on Linux or macOS requires no
Vulkan, Metal, Wayland, X11, or audio development packages. CI compiles the shared
library and executable, runs the core tests, and checks the selected adapter on
every platform in Debug and Release. Standalone sanitizer tests run separately.
