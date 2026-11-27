# Platform interfaces and bootstrap scope

The binding architecture is in [AGENTS.md](../AGENTS.md). The existing
[vision.md](vision.md) is an earlier draft. The sections below describe the implemented
platform interfaces and the native services behind them.

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

### Sleeping and pacing

Two functions make a thread wait for time to pass, and they differ in what
they spend to be exact.

| Need | Use |
| --- | --- |
| Let time pass and use no processor meanwhile | `tb_platform_sleep(nanoseconds)` |
| Reach a moment exactly, as a frame loop must | `tb_platform_wait_until(deadline)` |

`tb_platform_sleep` sleeps for at least the time asked and wakes when the
system gets round to it. `tb_platform_wait_until` waits for a reading of the
clock: it sleeps until shortly before, and then runs, watching the clock, to
the moment itself. A frame loop adds the length of a frame to a deadline each
frame and waits for it, so that a frame that wakes late does not make the
next one late:

```c
uint64_t deadline = tb_platform_clock_nanoseconds();

for (;;)
{
    run_a_frame();
    deadline += frame_nanoseconds;
    tb_platform_wait_until(deadline);
}
```

| `tb_bench time`, what asking for a length of time gives | `tb_platform_sleep` | `tb_platform_wait_until` |
| --- | --- | --- |
| 100 microseconds | 551 to 571 µs | 100.000 µs |
| 1 millisecond | 1.51 to 1.61 ms | 1.000000 ms |
| A frame at 60 a second, 16.667 ms | 17.01 to 17.04 ms | 16.66701 ms |

Release, MSVC 19.50, i9-14900HX, Windows 11; each figure is the average of 60.
A sleep on Windows wakes about half a millisecond late whatever its length. A
wait is as exact as the clock, and pays for it with a processor for the last
stretch before its moment.

| Platform | Sleep | How late it may wake, which a wait spends awake |
| --- | --- | --- |
| Windows | A waitable timer of high resolution, which is not bound to the 15.6 ms timer interrupt as `Sleep` is | 1 ms |
| Linux / Steam Deck | `clock_nanosleep` to a moment on `CLOCK_MONOTONIC`, so that a signal does not shorten it | 200 µs |
| macOS | `nanosleep`, repeated while the clock says there is time left | 1 ms |

**A sleep cannot be ended early.** A thread that may have to be woken, such as
a worker that must stop when the program does, waits on a condition or a
semaphore with a time limit.

**Pacing is the platform's wait and nothing more.** The engine has no pacer
object: a deadline is a number that its owner adds to.

The tests check that a sleep lasts at least as long as asked, that a wait
never ends early, and that 120 paced frames end when 120 frame lengths have
passed. They allow two seconds of lateness, because a hosted CI machine can
keep a thread waiting for a processor that long; the figures above are what a
machine with nothing else to do gives.

CMake selects exactly one adapter. All OS-specific headers and native symbols
stay in that adapter. The public visibility header is generated from the selected
platform template; core code and public interfaces do not detect the OS.
Windows consumers import the DLL API, and the library exports it. Only documented
API symbols have default visibility on Linux/macOS.

The adapters are the layer beneath the foundation modules and use none of
them. Their tests (`tb_platform_tests`) are built from the selected adapter's
sources alone, which keeps that true and lets a test call a function that is
private to an adapter.

What an adapter takes from above is two reports. It states its contracts
with `TB_ASSERT`, and a failed one calls `tb_assert_fail`, which
`src/core/assert.c` defines. And a system call that it cannot do without, and
that failed, calls `tb_fatal_message`, which `src/core/fatal.c` defines. In
the adapters' tests `tests/support/platform_assert.c` defines both instead,
as traps of the test framework, so `TB_TEST_EXPECT_TRAP` works there as it
does everywhere else.

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
function that the [default adapter](#unsupported-platforms) implements. On Linux and macOS it
restores the system's handling of `SIGABRT` before calling `abort()`, so that
a process that reported its failure and is ending is not reported again as a
crash by [`tb_crash_catch`](#crashes).

A child process checks in CI that a failed assertion reports its location and
ends the process promptly on every platform.

## Call stacks

Public declaration: `include/badger/stack.h`. `tb_stack_capture` records where
the calling thread is and how it got there, as the addresses that its calls
return to, innermost first. `tb_stack_describe` says what one address is part
of: the function, the module and the offset within it, and the source file
and line. Capturing is cheap and allocates nothing; describing is for a
report that someone will read.

| Platform | Selected implementation | Capture | What an address can be given |
| --- | --- | --- | --- |
| Windows | `src/platform/win32/stack.c` | `RtlCaptureStackBackTrace` | Module always; function, file and line from the program's `.pdb` files, through DbgHelp |
| Linux / Steam Deck | `src/platform/posix/stack.c` | `backtrace` | Module and offset always; the function when the module exports it or kept its name |
| macOS | `src/platform/posix/stack.c` | `backtrace` | The same |

What cannot be named is left empty, never guessed. The module and offset are
what `addr2line` or `atos` need to name the rest afterwards.

DbgHelp is part of Windows. It is loaded when the first address is described,
so a program that never describes one does not load it, and one lock
serializes every call into it, because it is not safe to use from two
threads.

[Fatal errors](#fatal-errors) are the first user: every report of one carries
the stack that led to it.

## Fatal errors

Public declaration: `include/badger/fatal.h`. A condition that no caller can
handle ends the process, and every such condition in the engine ends it in
one place: `tb_fatal`.

```c
tb_fatal("render: the built-in shaders could not be loaded (%s)", reason);
```

By default the message and the call stack are written to stderr, and the
process ends with `tb_platform_abort`:

```
render: the built-in shaders could not be loaded (file not found)
    #0 tb_fatal (badger.dll+0x112d0) src\core\fatal.c:116
    #1 start_renderer (burrow.exe+0x2f1c) renderer.c:212
    #2 main (burrow.exe+0x1108) main.c:40
```

`tb_fatal_set_handler` replaces that, for an application that wants the
report in its log, in a dialog, or sent somewhere. A handler that only wants
to add something writes its own part and calls `tb_fatal_write` for the rest.

| What goes wrong | How it arrives |
| --- | --- |
| A failed `TB_ASSERT` | Through the handler of `assert.h`; here when none is installed |
| Exhausted memory | Through the handler of `memory.h`; here when none is installed |
| A fault found by the checking memory functions | Through the handler of `memory_debug.h`; here when none is installed |
| A system call that an adapter cannot do without, such as starting a thread | Here |
| More unfinished jobs than the scheduler can hold | Here |
| A crash the system raises, after `tb_crash_catch` | Here, through `tb_fatal_with_stack` (see [Crashes](#crashes)) |

The platform adapters are beneath the foundation modules and use none of
them, with exceptions that are all reports: a broken contract goes to
`tb_assert_fail`, a system call that failed to `tb_fatal_message`, and a crash
to `tb_fatal_with_stack`.
In the adapters' own tests both are traps of the test framework.

The default adapter for an unsupported platform is the exception: it reports
"not implemented" with nothing but standard C, because on such a platform
there is no stack to capture.

**A report allocates nothing of the engine's**, so exhausted memory can be
reported like anything else. Naming the frames does allocate inside the
system's library on Windows. Each line is written with
`tb_platform_write_error`, the system's own call (`WriteFile`, `write`),
which takes no lock of the C library's.

## Crashes

Public declaration: `include/badger/crash.h`. After `tb_crash_catch`, a crash
that the system raises is reported as a fatal error is: to the fatal handler,
or to stderr as a line that says what happened and where, then the stack of
the code that crashed. Then the process ends. Burrow calls it first thing.

```
crash: access violation writing 0x0000000000000000, at 0x00007ff7070b7d39
    #0 crash_probe_write_through_null (tb_probe.exe+0x7d39) crash_probe.c:25
    #1 tb_test_body_crash_probe__a_null_pointer_is_reported_with_its_stack (tb_probe.exe+0x7dc4) crash_probe.c:51
```

| Platform | Source | What is caught | The stack |
| --- | --- | --- | --- |
| Windows | `src/platform/win32/crash.c` | Any exception no handler of the program's handles (`SetUnhandledExceptionFilter`), and `abort()` (a handler of `SIGABRT`) | Unwound from the registers of the code that raised it (`RtlVirtualUnwind`, on x64) |
| Linux / Steam Deck | `src/platform/posix/crash.c` | `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE`, `SIGABRT` (`sigaction`) | `backtrace` in the handler, from the instruction that raised the signal |
| macOS | `src/platform/posix/crash.c` | The same | The same |

**What a crash handler may do.** The heap may be damaged and the crashed
thread may hold any lock, so a report allocates nothing, formats its first
line with code of the adapter's own, and writes without the C library.
Naming the frames asks the system: on Windows through DbgHelp, whose lock a
thread that crashed holding it skips, reporting its stack without names.

- **Running out of stack.** On Linux and macOS the thread that called
  `tb_crash_catch` reports on a stack of its own (`sigaltstack`); other
  threads that run out of stack are not reported. On Windows that thread is
  guaranteed 64 KiB for the report (`SetThreadStackGuarantee`).
- **Installed only when asked.** A library must not take over a program's
  crashes, so nothing is caught until the program calls `tb_crash_catch`.
- **`abort()` and the C library.** A handler of `SIGABRT` belongs to one copy
  of the C library. The engine's DLL is built with MSVC's static runtime, so
  a program's `abort()` reaches the program's own copy, and the program also
  calls `signal(SIGABRT, tb_crash_report_abort)`, as Burrow does.
- **Crash dumps.** After `tb_crash_write_dumps`, a crash on Windows also
  writes a minidump (`MiniDumpWriteDump`, from DbgHelp, loaded at run time)
  to the directory given, or to the system's temporary one, named after the
  program and the process (`burrow-1234.dmp`). The report's first line ends
  with `; dump: ` and the path. A thread cannot reliably dump itself, so a
  thread that `tb_crash_write_dumps` starts waits to write it, and the
  crashed thread hands it the exception and waits up to a minute. Only the
  first crash is dumped. The dump holds every thread's stack, the memory
  they point at, and the loaded and unloaded modules: a few hundred
  kilobytes, which Visual Studio and WinDbg open beside the program's
  symbols. On Linux and macOS the function returns false: the system writes
  a core file where its settings allow (`ulimit -c`, and on Linux
  `/proc/sys/kernel/core_pattern`). Burrow asks for dumps.
- **Not done:** a stack for each block of the checking memory functions,
  left optional by the issue that asked for crash reports.

`tests/core/crash_probe.c` crashes on purpose in child processes, and
`tests/ExpectCrashReport.cmake` checks each report: a null pointer, `abort()`,
a crash on another thread, a crash inside a log's sink while the log holds
its lock, and a fatal error after `tb_crash_catch`, which is reported once
and not again as a crash. Two more crash after `tb_crash_write_dumps`, by a
null pointer and by `abort()`: on Windows the report names the dump, and the
file there is a minidump (it begins `MDMP`), which the script then deletes;
elsewhere the report mentions no dump.

## Atomic values

Public declaration: `include/badger/platform.h`. A value that any thread may
read and write without a lock. Every operation is one indivisible step, and
all of them are sequentially consistent: the threads agree on one order in
which they happened.

| Type | Holds | Operations |
| --- | --- | --- |
| `tb_platform_atomic_size` | A `size_t` | `_add`, `_subtract`, `_load`, `_store`, `_exchange`, `_compare_exchange` |
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
| Windows | `src/platform/win32/atomic.c` | Interlocked operations from the Windows API, and its acquire reads for the loads. |
| Linux / Steam Deck | `src/platform/linux/atomic.c` | The compiler's atomic builtins (GCC and Clang). |
| macOS | `src/platform/macos/atomic.c` | The compiler's atomic builtins (AppleClang). |

It is a platform interface because MSVC does not offer C11 `<stdatomic.h>` in
C17 mode without an experimental switch. Every allocation depends on it. Weaker orderings, and arithmetic on
the 32-bit value, wait for the code that needs them.

**A load is a read, and writes nothing.** That is so on every platform, and on
Windows it had to be made so. An interlocked operation that changes nothing,
such as adding zero, returns the right value, but it is a write to the
processor: it takes the value's cache line away from every other thread that
is reading it. Threads that only look at a value then get in each other's way
as much as threads that change it, which matters to a thief that looks at the
ends of every other worker's queue. The Windows adapter reads with
`ReadSizeTAcquire`, `ReadAcquire`, and `ReadPointerAcquire`: a plain read on
x86, and a read with a barrier on Arm. Every other operation there is
interlocked, which is a full barrier, so a read stays in step with them.

## Threads, mutexes, and condition variables

Semaphores, values that each thread has for itself, and the hint for a loop
that waits are in the same header, and described at the end of this section.

Public declaration: `include/badger/thread.h`.

| | Functions |
| --- | --- |
| A thread | `tb_thread_create`, `tb_thread_create_with_options`, `tb_thread_join`, `tb_thread_current_id`, `tb_thread_current_name` |
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

**A thread has the system's default stack** unless it asks for another: usually
1 MB on Windows, 8 MB on Linux, and 512 KB on macOS.

**A thread can be given a name, a stack size, and a priority** when it is
created, with `tb_thread_create_with_options`:

```c
const tb_thread_options options = {.name = "badger-worker-3",
                                   .stack_size = TB_THREAD_STACK_SIZE_DEFAULT,
                                   .priority = TB_THREAD_PRIORITY_BACKGROUND};

tb_thread_create_with_options(&thread, &options, run_worker, worker);
```

| Option | Asks for nothing when | What the thread gets |
| --- | --- | --- |
| `name` | `NULL` or empty | A name that debuggers, profilers, and crash reports show. |
| `stack_size` | `0` | At least that many bytes of stack. `TB_THREAD_STACK_SIZE_DEFAULT` is 1 MiB, the same on every platform. |
| `priority` | `TB_THREAD_PRIORITY_NORMAL` | The system's nearest to `TB_THREAD_PRIORITY_BACKGROUND` or `TB_THREAD_PRIORITY_HIGH`. |

The thread gives itself its name and priority before it runs its function,
because macOS names only the calling thread. The name is copied into the
`tb_thread`, so the caller's text may go away.

**A stack size is always honored; a name and a priority are requests.** The
system rounds a stack size up to its page size and its own minimum. A name
or a priority that the system does not grant is not an error, and the thread
runs without it.

| Platform | Name | Bytes of a name the system keeps | `BACKGROUND` | `HIGH` |
| --- | --- | --- | --- | --- |
| Windows | `SetThreadDescription` | All 31 that the engine keeps | `THREAD_PRIORITY_BELOW_NORMAL` | `THREAD_PRIORITY_ABOVE_NORMAL` |
| Linux / Steam Deck | `pthread_setname_np` | 15 | Niceness 10 | Niceness -5, where the process may raise a priority; most may not, and the thread stays normal |
| macOS | `pthread_setname_np` | All 31 | `QOS_CLASS_UTILITY` | `QOS_CLASS_USER_INTERACTIVE` |

**A name is cut between characters.** The engine keeps the first 31 bytes of
a name, and Linux the first 15. A cut that would fall inside a UTF-8
character drops the whole character.

`tb_thread_current_name` reads the calling thread's name back from the
system, which is the name a debugger shows. A thread that was given none has
what the system gives it: nothing on Windows and macOS, and the program's name
on Linux.

On macOS the class a thread is given also decides which kind of core it runs
on, where the machine has two: utility work goes to the efficiency cores.

A thread can be confined to some processors with
`tb_thread_options::processors`; see [Confining a thread](#confining-a-thread).

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

### Semaphores

A `tb_semaphore` is a count of signals. `tb_semaphore_signal` adds to it and
`tb_semaphore_wait` takes one, waiting while there is none. Unlike a
condition, it remembers a signal that nobody was waiting for, which makes it
the way to hand units of work to a thread, or to share a pool of a few things
between many threads.

```c
tb_semaphore work = {0}; /* No signals yet. */

/* A producer, for each unit of work: */
tb_semaphore_signal(&work, 1);

/* A consumer: */
tb_semaphore_wait(&work);
```

| Need | Use |
| --- | --- |
| Add signals | `tb_semaphore_signal(semaphore, count)` |
| Take one, waiting for it | `tb_semaphore_wait(semaphore)` |
| Take one if there is one | `tb_semaphore_try_wait(semaphore)` |
| Take one, waiting a limited time | `tb_semaphore_wait_for(semaphore, nanoseconds)` |

Like a mutex and a condition, a semaphore is ready when it is all zeros and
needs no destruction. That is why it is built from those two, once, in
`src/platform/shared/semaphore.c`, and not from each system's own semaphore:
every one of those must be destroyed, and macOS has none that can live inside
another object. The cost is a lock for each signal and each wait.

### Values that each thread has for itself

`_Thread_local` gives every thread its own copy of a variable, and is what to
use when the variable is known as the program is compiled. A `tb_thread_local`
is for the two things it cannot do: a place that is made at run time, and a
value that needs something done with it when its thread ends.

```c
static tb_thread_local scratch = {0};

tb_thread_local_create(&scratch, release_scratch); /* Once, at startup. */

/* On any thread: */
if (tb_thread_local_get(&scratch) == NULL)
{
    tb_thread_local_set(&scratch, make_scratch());
}
```

`release_scratch` is called with a thread's value as the thread ends, on that
thread, when the value is not NULL. It runs for a thread that
`tb_thread_create` started and whose function returned; for other threads the
systems differ. Destroy a place only when no thread has a value in it.

| Platform | Implementation |
| --- | --- |
| Windows | Fiber-local storage, `FlsAlloc`: the only kind on Windows that calls a function with a thread's value as the thread ends |
| Linux, macOS | `pthread_key_create` |

### Waiting without sleeping

A loop that waits for another thread to change something calls
`tb_thread_relax` each time round. It is the processor's own instruction for
this (`pause` on x86, `yield` on ARM): the loop uses less power, takes less
from a thread that shares its core, and leaves faster when the wait is over.
`tb_thread_yield` gives up the rest of the thread's turn to another thread
that is ready, for a loop that has waited longer than that.

The job scheduler's search for work calls `tb_thread_relax` between two looks
at queues that were empty. Submitting a job and waiting for it went from 208
to 243 nanoseconds to 177 to 202 with it (`tb_bench jobs`, Release, MSVC
19.50, i9-14900HX); the other cases did not change by more than they vary.

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

### The list of processors

`tb_system_list_processors` lists the same processors, one entry each: the
core it is part of, numbered from zero, and whether that core is of the
fastest kind. A processor is known by its place in that list. That is the
engine's name for it, and the one a thread is confined by; the systems' own
numbers stay in the adapters, and so do Windows' groups of 64.

```c
tb_system_processor list[256] = {{0}};
const size_t count = tb_system_list_processors(list, 256);
```

On the development laptop the list is 16 processors of the fast kind, two to
a core (cores 0 to 7), then 16 efficient ones, a core each (cores 8 to 23).
The list agrees with the counts asked at the same moment: as many entries, as
many distinct cores, as many distinct fast cores. Each adapter builds the list
and counts it, so the described machines that test the counting test the
list too.

macOS says how many processors of each kind there are, but not which is
which. There each processor is listed as a core of its own, of the fast kind,
and since threads cannot be confined there, nothing is lost.

### Confining a thread

```c
const size_t first_fast_core[] = {0};
const tb_thread_options options = {.processors = first_fast_core, .processor_count = 1};

tb_thread_create_with_options(&thread, &options, run, argument);
```

| Platform | How | Where the thread is |
| --- | --- | --- |
| Windows | Created suspended, given a group and a mask with `SetThreadGroupAffinity`, then started | `GetCurrentProcessorNumberEx` |
| Linux / Steam Deck | `pthread_attr_setaffinity_np` on the attributes it is created with | `sched_getcpu` |
| macOS | Not possible: `tb_thread_can_be_confined` is false and the option is ignored | `tb_thread_current_processor` says it does not know |

A place that is not in the list when the thread is made is left out, and with
none left the thread runs where it would have anyway. On Windows a thread runs
within one group, that of the first processor named.

**The job system does not confine its workers.** The system's scheduler knows
which processors are busy, which are fast, and which are about to sleep, and
moving a thread between them is cheap. A worker that may run on only one
processor waits whenever that one is taken. Confinement is offered for the
cases where something else has to be kept off a processor, and should be
measured where it is used.

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

## Random bytes

Public declaration: `include/badger/random.h`, `tb_random_bytes(void *,
size_t)`. It fills a buffer with bytes from the system's generator for
cryptography, for keys, nonces, and salts: what no one may guess or repeat.

| Platform | Source |
| --- | --- |
| Windows | `BCryptGenRandom` with the system's preferred generator, from `bcrypt.dll`, loaded through `badger/library.h` the first time it is needed so no program links `bcrypt.lib` |
| Linux | `getrandom`, which waits only until the kernel's generator is seeded at boot |
| macOS | `arc4random_buf`, which cannot fail |
| Other | "not implemented": standard C has no source fit for a key |

It returns false only when the system has no generator to give, which no
supported system lacks. It is thread-safe and needs no setup.

These bytes cannot be seeded, so nothing that must repeat (a test, a
simulation, a replay) may use them; those use a [generator](random.md) and
record its seed.

## Command-line arguments

Public declaration: `include/badger/process.h`. `tb_process_get_arguments`
gives a program its arguments as UTF-8, the program's name first, and
`tb_process_release_arguments` lets them go.

| Platform | Source |
| --- | --- |
| Windows | `GetCommandLineW`, split by `CommandLineToArgvW` and converted from UTF-16; an unpaired surrogate becomes U+FFFD |
| Linux, macOS | `main`'s own `argc` and `argv`, unchanged: the bytes the program was started with, which for a file name need not be UTF-8 |
| Other | "not implemented" |

On Windows the C runtime gives `main` its arguments in the ANSI code page,
so a name outside it arrives as question marks; this is the way around that.
`CommandLineToArgvW` splits quotes and backslashes as the C runtime does in
all but rare cases. A program passes the result to `tb_cli_parse()`
([command lines](command-line.md)), as Burrow does.

## Shared libraries

Public declaration: `include/badger/library.h`. `tb_library_open` loads a
shared library at run time, `tb_library_find_function` and
`tb_library_find_data` find what it exports, `tb_library_bind` fills a table
of functions at once, and `tb_library_close` lets it go.

**What goes through it.** The engine calls the platform's graphics,
windowing, and audio libraries (Direct3D, DXGI, Vulkan, OpenGL, Metal, X11,
Wayland, PipeWire, and others) through this interface rather than linking
them, and later the vendor libraries (DLSS, FSR, XeSS, Reflex). Most of
these are part of the system already, so the engine ships none of them, and
a program that links the engine starts on a machine that lacks one: the
backend that needs it reports itself unavailable, and the engine falls back.
The foundation check (`tools/check_foundation.py`) refuses `LoadLibrary`,
`GetProcAddress`, `dlopen`, and `dlsym` outside `src/platform/`.

**Every load says where it looks**, and none uses the system's default
search order:

| Search | Windows | Linux | macOS |
| --- | --- | --- | --- |
| `TB_LIBRARY_SEARCH_SYSTEM` | The system directory, by full path | The dynamic linker's search, by a file name such as `libX11.so.6` | `/usr/lib/<name>`, or `/System/Library/Frameworks/<Name>.framework/<Name>` |
| `TB_LIBRARY_SEARCH_APPLICATION` | The executable's directory, by full path | The same | The same |
| `TB_LIBRARY_SEARCH_PATH` | One absolute path | The same | The same |

- **No planted library is loaded.** A name is never looked for in the
  working directory or `PATH`. On Windows every load is by full path, so a
  module of the same name already loaded from elsewhere is not taken for the
  one meant; the libraries the loaded one depends on come from beside it or
  from the system directory. A relative path is refused.
- **Linux names libraries by their major version.** `libX11.so.6` is the
  library a player's system has; `libX11.so` belongs to the development
  packages, and may be a version the engine was not written for. The
  dynamic linker's search includes `LD_LIBRARY_PATH`, as the Steam runtime
  expects.
- **A library is complete or not loaded.** Linux and macOS open with
  `RTLD_NOW`, so a symbol the library needs and no loaded library has fails
  the open, not a call in the middle of a frame; and with `RTLD_LOCAL`, so
  its symbols do not take the place of another library's. Windows shows no
  dialog box for a damaged library.
- **An exact path is exact.** Windows adds `.dll` to a name without an
  extension; the adapter prevents it.

**Every failure is the caller's to handle.** Nothing in the interface ends
the process. A call returns a `tb_library_result` and, when asked, fills a
`tb_library_error` with a message that names the library or symbol and gives
the system's reason:

| Result | Meaning |
| --- | --- |
| `TB_LIBRARY_INVALID` | The name cannot be used with its search; nothing was looked for |
| `TB_LIBRARY_UNAVAILABLE` | The library is missing, or there but unusable: another architecture, a missing dependency, an unresolved symbol |
| `TB_LIBRARY_SYMBOL_MISSING` | The library does not export the symbol; `tb_library_bind` names every required one that is missing |
| `TB_LIBRARY_NO_MEMORY` | The system ran out of memory or another resource |

```
libwayland-client.so.0: missing wl_proxy_marshal_flags, wl_proxy_get_version
```

Functions and data are found by separate calls, because standard C converts
neither kind of pointer to the other. A function comes back as
`tb_library_function`, which the caller converts to the function's real
type.

Opening and finding are thread-safe; the system counts the opens of a
library, and each is closed. What was found in a library stays valid until
it is closed. The tests load a library the build puts beside them
(`tests/platform/library_fixture.c`) on every platform, and one of the
system's on each.

## DXC

Public declaration: `include/badger/dxc.h`. DXC, Microsoft's HLSL compiler,
is the only compiler that writes DXIL, the bytecode D3D12 runs.
`tb_dxc_open` loads it, `tb_dxc_compile` compiles HLSL into a signed DXIL
container, `tb_dxc_release` lets a compile's output go, and `tb_dxc_close`
unloads it. `tb_dxc_version` names the DXC loaded, its compiler's version and
commit and its validator's version, as `IDxcVersionInfo2` tells them
(`dxcompiler 1.8 (4792, 23992152) dxil 1.8`): the
[shader cache](shader-cache.md) keys on it. The shader module compiles for D3D12 through it (see
[Shaders](shaders.md#compiling-for-d3d12-dxil)); most code calls
`tb_shader_compile` instead.

| Platform | Selected implementation | Behavior |
| --- | --- | --- |
| Windows | `src/platform/win32/dxc.c` | Loads `dxcompiler.dll` and `dxil.dll` at run time, and calls `IDxcCompiler3` through COM's C binding |
| Linux / Steam Deck | `src/platform/posix/dxc.c` | Unavailable: DXIL runs only on D3D12 |
| macOS | `src/platform/posix/dxc.c` | The same |

**Where Windows finds DXC.** DXC is not part of Windows, and the engine does
not ship it. Both libraries come from one directory, the first that has both:

1. the executable's directory (`TB_LIBRARY_SEARCH_APPLICATION`), where a game
   or a tool puts a copy of its own;
2. the Windows SDK's redistributable directory, `Redist\D3D\x64` (`arm64` on
   ARM) under the SDK's root, by its full path. The root is the registry's
   `KitsRoot10` value of `HKLM\SOFTWARE\Microsoft\Windows Kits\Installed
   Roots`, read with `RegGetValueW` from `advapi32.dll`, itself loaded from
   the system directory.

`dxil.dll` is loaded first. `dxcompiler.dll` loads it by name to sign what it
compiles, and so finds the one already loaded, from the same directory.
D3D12 refuses DXIL that is not signed. Development builds therefore compile
shaders at run time wherever the Windows SDK is installed. A shipped game,
whose shaders are compiled when it is built, needs no DXC.

**Unavailable, never fatal.** A machine without DXC, or a directory with one
library and not the other, gives `TB_DXC_UNAVAILABLE`. Its message says where
DXC was looked for and what to do, and the renderer falls back to a backend
that needs no DXIL. On Linux and macOS DXC is always unavailable, and this is
not "not implemented": DXIL runs only on D3D12, which only Windows has. Those
systems lack the capability; no work is missing, as for DLSS on macOS. (DXC
builds for Linux too; compiling DXIL there, to cook a Windows game, can come
later.)

**The COM interface from C.** The Windows SDK's `dxcapi.h` declares DXC's
interfaces only for C++. `src/platform/win32/dxc_api.h` declares the part the
engine calls for C, as COM does: a struct whose first member points to the
table of functions, in the order of `dxcapi.h`, with its GUIDs.

**A compile** makes a compiler of its own, so compiles run on any number of
threads with one `tb_dxc`. It reads no file (its source has no `#include`)
and is always HLSL 2021. It is optimized (`-O3`), or with `TB_DXC_DEBUG`
unoptimized, with the debug information embedded (`-Od -Zi -Qembed_debug`).
The output holds the container and DXC's messages until it is released.

## D3DCompiler

Public declaration: `include/badger/d3dcompiler.h`. D3DCompiler, the HLSL
compiler that is part of Windows, compiles HLSL of Shader Model 5 into DXBC,
the bytecode D3D11 runs. `tb_d3dcompiler_open` loads it,
`tb_d3dcompiler_compile` compiles HLSL into a DXBC container,
`tb_d3dcompiler_release` lets a compile's output go, and
`tb_d3dcompiler_close` unloads it. `tb_d3dcompiler_version` names the library
loaded by its file version (`d3dcompiler_47 10.0.26100.1`), which the
[shader cache](shader-cache.md) keys on. The shader module compiles for
D3D11 through it (see [Shaders](shaders.md#compiling-for-d3d11-dxbc)); most
code calls `tb_shader_compile` instead.

| Platform | Selected implementation | Behavior |
| --- | --- | --- |
| Windows | `src/platform/win32/d3dcompiler.c` | Loads `D3DCompiler_47.dll` from the system directory at run time, and calls its `D3DCompile` |
| Linux / Steam Deck | `src/platform/posix/d3dcompiler.c` | Unavailable: DXBC runs only on D3D11 |
| macOS | `src/platform/posix/d3dcompiler.c` | The same |

**Where Windows finds it.** `D3DCompiler_47.dll` is part of every Windows
since 8.1, and is loaded from the system directory alone
(`TB_LIBRARY_SEARCH_SYSTEM`): the engine ships no copy, and a copy beside the
executable is never looked at. Its version is read from the version
resource of the module that holds `D3DCompile`, in place, so nothing more is
loaded to name it. The SDK's `d3dcompiler.h` does not compile as C with every
warning on, so the adapter declares the entry point and the flags it passes.

**Unavailable, never fatal.** A system directory without the library gives
`TB_D3DCOMPILER_UNAVAILABLE`, which the renderer reports and falls back
from. On Linux and macOS it is always unavailable, and this is not "not
implemented": DXBC runs only on D3D11, which only Windows has.

**A compile** is thread-safe, and reads no file (its source has no
`#include`). It is strict (`D3DCOMPILE_ENABLE_STRICTNESS`) and optimized
(`/O3`), or with `TB_D3DCOMPILER_DEBUG` unoptimized with debug information
(`/Od /Zi`). The output holds the container and the compiler's messages,
`name(line,column): error X3004: ...`, until it is released.

## Windows and gamepads

Two interfaces are built on the foundation modules rather than beneath
them, and each has a page of its own: [windows and displays](windows.md)
(`window.h`) and [gamepads](gamepad.md) (`gamepad.h`). Each is a shared
front, `src/platform/shared/window.c` and `gamepad.c`, over a backend of each
adapter: Win32, and XInput with raw HID, on Windows; Wayland, X11 and evdev on
Linux; Cocoa and the GameController framework on macOS.

## Audio output

The audio device interface pulls interleaved float samples from the game.
It uses WASAPI on Windows, PipeWire or ALSA on Linux, and Core Audio on macOS.
See [audio devices](audio.md) for device selection, callback lifetime and latency.

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
without a probe, fails the test. A function the engine implements once for
every platform, as a result's name or `gamepad.h`'s buttons made from
values, needs nothing of an adapter and is left out: the script is given the
sources that define them (`src/core/gamepad.c`). `tests/ExpectPlatformSelection.cmake` checks
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
services include asynchronous files and system information. Each loads the system libraries it needs through
`badger/library.h`.
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
