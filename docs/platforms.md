# Platform interfaces and bootstrap scope

The binding architecture is in [AGENTS.md](../AGENTS.md). The existing
[vision.md](vision.md) is an earlier draft. Only the clock,
abnormal-termination, and atomic-counter interfaces exist so far; the other
platform services and rendering backends are still planned. No renderer, window, vendor SDK, or game loop is implemented.

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

## Atomic counter

Public declaration: `include/badger/platform.h`, `tb_platform_atomic_size` with
`tb_platform_atomic_size_add`, `tb_platform_atomic_size_subtract`, and
`tb_platform_atomic_size_load`. A `size_t` that any thread may update without a
lock; every operation is sequentially consistent. The allocation functions
count live blocks with it.

| Platform | Selected implementation | Current behavior |
| --- | --- | --- |
| Windows | `src/platform/win32/atomic.c` | Interlocked operations from the Windows API. |
| Linux / Steam Deck | `src/platform/linux/atomic.c` | The compiler's atomic builtins (GCC and Clang). |
| macOS | `src/platform/macos/atomic.c` | The compiler's atomic builtins (AppleClang). |

It is a platform interface because MSVC does not offer C11 `<stdatomic.h>` in
C17 mode without an experimental switch. It is implemented on all three
platforms: every allocation depends on it. It is the first piece of the planned
threading interface, which will grow the remaining atomic types and operations.

## Future interfaces and capabilities

For each new platform interface, add its public contract and all three adapters
together. Linux/macOS adapters may initially be fatal stubs, but must compile and
have subprocess tests. A fatal stub prints its interface, function, platform,
the native API it will use, and the next action, then exits with failure; a
CTest script launches it in a child process and checks both the exit status
and the diagnostic. No interface is stubbed at present. Planned services
include windowing/events, input, sleep and frame pacing, threads, virtual
memory, files, dynamic libraries, audio devices, and system/crash information. Native APIs and future RHI backends are catalogued in
the binding vision; their implementation is outside this bootstrap milestone.

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
