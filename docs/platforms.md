# Platform interfaces and bootstrap scope

The binding architecture is in [AGENTS.md](../AGENTS.md). The existing
[vision.md](vision.md) is an earlier draft. Only the time-counter and
abnormal-termination interfaces exist so far; the other platform services and
rendering backends are still planned. No renderer, window, vendor SDK, or game loop is implemented.

## Time counter

Public declaration: `include/badger/platform.h`, `tb_platform_ticks(void)`.
Returns an opaque monotonic tick count on Windows. The epoch and tick duration
are unspecified; this API currently supports ordering readings within one
process. It owns no resources and is safe to call from any thread.

| Platform | Selected implementation | Current behavior | Native runtime requirement |
| --- | --- | --- | --- |
| Windows | `src/platform/win32/time.c` | Calls `QueryPerformanceCounter`; native failure is fatal | Windows inbox Kernel32 API and standard C runtime |
| Linux / Steam Deck | `src/platform/linux/time.c` | Fatal stub naming `clock_gettime` and the implementation path | Standard C runtime for the stub; no graphics or window-system packages |
| macOS | `src/platform/macos/time.c` | Fatal stub naming `mach_absolute_time` and the implementation path | Standard C runtime for the stub; no framework dependencies |

CMake selects exactly one adapter. All OS-specific headers and native symbols
stay in that adapter. The public visibility header is generated from the selected
platform template; core code and public interfaces do not detect the OS.
Windows consumers import the DLL API, and the library exports it. Only documented
API symbols have default visibility on Linux/macOS.

The fatal stubs print the interface (`platform.time`), function, platform, planned
native API, and next action, then exit with failure. CTest launches each stub in
a child process and checks both its exit status and diagnostic. Calling the
portable arithmetic API or running the bootstrap `burrow` demo does not invoke
native time services.

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

## Future interfaces and capabilities

For each new platform interface, add its public contract and all three adapters
together. Linux/macOS adapters may initially be fatal stubs, but must compile and
have subprocess tests. Planned services include windowing/events, input, clocks
and pacing, threads, virtual memory, files, dynamic libraries, audio devices, and
system/crash information. Native APIs and future RHI backends are catalogued in
the binding vision; their implementation is outside this bootstrap milestone.

An unimplemented required interface is fatal. An optional hardware capability
or missing vendor redistributable must instead report itself unavailable and
allow the engine's documented fallback. Do not introduce vendor headers,
redistributables, SDKs, or platform packages without the author's approval.

## Builds

Use the cross-platform commands in [README.md](../README.md). Windows requires
MSVC's C compiler and the Windows SDK. Linux uses GCC or Clang, and macOS uses
AppleClang with its native SDK. Building the current stub platforms requires no
Vulkan, Metal, Wayland, X11, or audio development packages. CI compiles the shared
library and executable, runs the core tests, and checks the selected adapter on
every platform in Debug and Release. Standalone sanitizer tests run separately.
