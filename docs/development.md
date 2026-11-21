# Tenacious Badger

A dependency-free C17 foundation for the `badger` game-engine shared library
and its separate `burrow` showcase executable. The same modules also build into
the `badger_static` static library. [Library consumption](consuming.md) covers
FetchContent and the published Debug/Release SDKs. The binding design and engineering
standards are in [AGENTS.md](../AGENTS.md). The earlier
[vision draft](vision.md) is preserved.

The foundation layer so far, one public header per module under
`include/badger/`:

| Header | Provides |
| --- | --- |
| `assert.h` | Developer assertions with a replaceable failure handler |
| `core.h`, `size.h` | Checked signed addition and checked size arithmetic |
| `xp.h`, `ap.h` | [Arithmetic](arithmetic.md): unsigned integers of a fixed number of digits, and signed integers that grow as needed |
| `memory.h` | [Memory allocation](memory.md) that fails fast, with a replaceable allocator |
| `arena.h` | [Arenas](memory.md#arenas) for objects that share a lifetime |
| `allocator.h` | [Allocators chosen per object](memory.md#choosing-the-allocator-of-one-object) |
| `pool.h` | [Pools](memory.md#pools): objects of one size, allocated and freed one at a time in constant time |
| `frame_memory.h` | [Frame memory](memory.md#frame-memory): an arena for each thread, released when its frame's memory comes round again |
| `array.h`, `bit.h`, `ring.h`, `list.h`, `table.h`, `set.h`, `sort.h` | [Collections](collections.md): a dynamic array, a bit vector, a ring, an intrusive list, a hash table, a set, and sorting |
| `str.h`, `text.h`, `fmt.h`, `atom.h`, `hash.h` | [Strings](strings.md): C strings, text values, formatted output, atoms, and hash functions |
| `decimal.h` | [Floating-point text that reads back](strings.md#floating-point-text-that-reads-back): the shortest text of a `double`, and reading text into one |
| `platform.h` | [Native platform interfaces](platforms.md): the clock, sleeping and pacing, abort, writing to stderr without the C library, and atomic values |
| `thread.h` | [Threads, mutexes, condition variables, semaphores, and thread-local values](platforms.md#threads-mutexes-and-condition-variables) |
| `system.h` | [The processors the process may run on](platforms.md#processors): how many, and which |
| `random.h` | [Random bytes from the operating system](platforms.md#random-bytes), for keys and nonces |
| `rng.h` | [Random numbers that repeat from a seed](random.md), the same on every platform |
| `library.h` | [Shared libraries loaded at run time](platforms.md#shared-libraries), and the functions and data they export |
| `stack.h` | [The call stack of a thread](platforms.md#call-stacks), and what an address is part of |
| `crash.h` | [Crashes](platforms.md#crashes) that the system raises, reported with the stack of the code that crashed |
| `fatal.h` | [Fatal errors](platforms.md#fatal-errors): the one way the engine ends the process, with the stack that led there |
| `jobs.h` | [The job system](jobs.md): worker threads, jobs with dependencies and priorities, and loops that run on every thread |
| `memory_debug.h` | [Memory functions that check how memory is used](memory.md) |
| `virtual_memory.h` | [Reserved address space and committed pages](virtual-memory.md) |
| `utf8.h`, `path.h` | UTF-8 and UTF-16 conversion, and [paths](paths.md) as text |
| `fs.h`, `file.h` | [Files and directories](files.md): `fs.h` is the module's header, and includes the native interface in `file.h` |
| `log.h` | [Logging](logging.md) with replaceable sinks |
| `cli.h` | [Command lines](command-line.md): options, arguments, subcommands, help, and errors |
| `process.h` | [The process's command-line arguments](platforms.md#command-line-arguments), as UTF-8 on every platform |
| `base64.h` | [Base64](codecs.md#base64): bytes as text and back, strictly |
| `checksum.h` | [Checksums](codecs.md#checksums): CRC-32 and Adler-32 |
| `sha256.h` | [SHA-256 and HMAC-SHA256](codecs.md#sha-256), for keys and content |
| `chacha20.h` | [ChaCha20](codecs.md#chacha20): a keystream from any offset, and a stream that applies it |
| `deflate.h` | [Compression](codecs.md#compression) and [decompression](codecs.md#decompression) of DEFLATE, raw or in zlib or gzip framing |
| `tar.h` | [TAR archives](archives.md#tar), read and written in order through streams |
| `package.h` | [Packages](packages.md): the engine's archive of cooked assets, compressed and optionally obfuscated |
| `zip.h` | [ZIP archives](archives.md#zip): entries found by name and read as streams, and written stored or deflated |
| `vfs.h` | [The virtual file system](vfs.md): directories, archive files, and archives in memory, mounted into one namespace |
| `json.h` | [JSON](serialization.md): a reader of events, documents in arenas, and a writer |
| `stream.h` | [Streams](streams.md): one interface for reading and writing bytes, over memory, files, and ranges of other streams |
| `window.h` | [Windows and displays](windows.md): windowed, borderless and fullscreen windows, displays and their scale, and window events |
| `settings.h`, `engine_settings.h` | [Settings](settings.md): the options players and developers choose, in layers from defaults to run time, and the engine's catalog of them |

What these headers do the same way, such as how a function reports failure,
is in [the conventions of the public API](conventions.md).

`burrow` calls the shared library, loads its [settings](settings.md#burrow),
and prints `2 + 3 = 5`; `burrow settings` lists every setting, and
`burrow --help` and `burrow --version` answer as a command-line program does.
Rendering, windowing,
gameplay, vendor integrations, and the other planned modules remain future work.

The [release and distribution requirements](../AGENTS.md#releases-and-distribution)
define desktop exports and the later browser and mobile targets. See
[release instructions](releasing.md) for native archive generation and publishing.

## Requirements

- CMake 3.25 or newer, and a C17 compiler: MSVC, GCC, or Clang.
- The compiler's native SDK and standard C runtime. Windows needs Visual Studio
  with the MSVC C compiler tools and the Windows SDK.
- Ninja for the convenience presets; plain CMake commands below also work with
  the platform's default build tool.
- Python 3 to run the tests: the source check that keeps code on the
  foundation layer is a Python script. Building the libraries alone does not
  need it.
- Optional developer tools: clang-format 22, Doxygen, and Universal Ctags.

There are **no third-party library or package dependencies**. Configure, build,
and tests work offline. Nothing in the build or CI installs packages or downloads
libraries. Compiler, SDK, and developer tools must already be available.

## Build and test

These commands use the platform's default generator and work on all three OSes:

```sh
cmake -S . -B build/local -DCMAKE_BUILD_TYPE=Debug
cmake --build build/local --config Debug --parallel
ctest --test-dir build/local -C Debug --output-on-failure --no-tests=error
```

For Release, use a separate `build/local-release` directory and replace both
`Debug` arguments with `Release`. Debug builds disable optimization and include
symbols. Release builds enable optimization (`-O3` on GCC/Clang, `/O2` on MSVC)
and strip the shipping Unix binaries; MSVC Release omits debug information.
The Release shared library is also optimized across its source files when it
is linked, on every toolchain that can do so. That is automatic: there is no
option for it and nothing to do differently.
Warnings are errors for all project code.

The executable is `build/local/bin/burrow` (`burrow.exe` on Windows). On macOS
it is `build/local/bin/burrow.app/Contents/MacOS/burrow`, with the engine in the
bundle Frameworks directory. Linux keeps the engine under `bin/lib`. Runtime
paths are relative so the exported layout works outside the build tree.

With Ninja available, use the committed presets:

```sh
make test
make release
make sanitizer
make thread-sanitizer
make fmt
make check-format
```

The equivalent portable commands are `cmake --preset debug`,
`cmake --build --preset debug`, and `ctest --preset debug`; substitute `release`,
`sanitizer`, or `thread-sanitizer` for the other configurations. On Windows, run `build.bat test`,
`build.bat release`, or `build.bat sanitizer` from an **x64 Native Tools Command
Prompt** so Ninja can find MSVC and the sanitizer runtime.

Windows builds also self-sign project executables and DLLs automatically, using
a locally generated certificate. See [Windows signing controls](releasing.md#windows-self-signing)
for an existing-certificate override and the signing opt-out.

## Tests and sanitizers

Unit tests use the in-house `tb_test` framework. [Testing](testing.md) explains
how to write, run, filter, and debug test cases, and how IDE test explorers pick
them up. Every case is its own CTest test, such as
`core_sum.adds_representable_operands`, and all of them are part of one executable,
`tb_tests`, which can run any selection of cases in any order.

The core tests cover checked addition and checked size arithmetic (tables of
ordinary and limit operands, overflow rejection, and agreement with wide
arithmetic on random operands) and the assertion macros. Test checks are
independent of `TB_ASSERT`, so they still run in Release, where assertions are
compiled out; cases that need an active assertion skip themselves there. The
framework has its own tests under `tests/framework/selftest/`.

Integration tests exercise `burrow` through the shared library. The selected
platform adapter has tests of its own, built without the rest of the engine:
on every platform the clock must never go backward and must agree with the C
library's clock about the length of an interval. A subprocess checks that a
failed assertion reports its location and aborts. Another checks that every
function of the default platform adapter reports "not implemented", and a
script checks that an unsupported platform stops the build.

`TB_ENABLE_SANITIZERS=ON` adds standalone Debug test executables that compile the
engine sources directly and link an instrumented copy of the test framework.
MSVC uses AddressSanitizer; GCC and Clang use AddressSanitizer plus
UndefinedBehaviorSanitizer. The shipping library and `burrow` remain free of
sanitizer instrumentation. Use Debug when running these tests.

`TB_ENABLE_THREAD_SANITIZER=ON` builds the same executables with
ThreadSanitizer instead, which finds data races. It needs GCC or Clang, so it
runs on Linux and macOS; MSVC has no ThreadSanitizer, and the option stops the
configuration there with a message. The two options cannot be on together.
[Testing](testing.md#sanitizers) has the details.
`TB_BUILD_TESTS=OFF` and `TB_BUILD_BENCHMARKS=OFF` build only the library and
consumer.

## Static analysis

Each compiler ships an analyzer that follows the paths through a function,
including the ones no test takes, and reports a null pointer that may be
dereferenced, memory that may be read past its end, or a resource that may
leak. `TB_ENABLE_STATIC_ANALYSIS=ON` runs it over the engine's own targets:
the two libraries and `burrow`. A finding fails the build, as a warning does.

```sh
cmake --preset analyze
cmake --build --preset analyze
```

| Compiler | Analyzer | How it runs |
| --- | --- | --- |
| MSVC | `/analyze` | As it compiles: the build is the analysis |
| GCC | `-fanalyzer` | As it compiles |
| Clang, AppleClang | `clang --analyze` | The `tb_analyze` target, which runs `tools/analyze_with_clang.py` over the compilation database and compiles nothing: add `--target tb_analyze` |

An adapter is analyzed where it is compiled: the Windows adapter by MSVC, the
Linux adapter by GCC and Clang, the macOS adapter by AppleClang. The tests are
not analyzed. They violate contracts on purpose, and the sanitizers watch
them as they run.

A finding is fixed in the code. Where the code is right and the analyzer is
not, the finding is suppressed on the one statement it names, with the reason
beside it, and the pull request says so: a suppression needs the same
approval as any other warning that is silenced.

## Link-time optimization

Release builds of the shared library are optimized across source files at
link time, where the toolchain supports it (`INTERPROCEDURAL_OPTIMIZATION`
in `src/CMakeLists.txt`). The static library is not: an archive meant for
link-time optimization can only be linked by the same version of the same
compiler, and the SDK gives that archive to others.

It is kept on, with one measured cost. `tb_bench`, Release, MSVC 19.50,
i9-14900HX, the library built with and without it, run in turn three times;
nanoseconds an operation:

| Case | Without | With |
| --- | --- | --- |
| Hash a 64-bit integer | 0.88 to 0.90 | 0.88 to 0.90 |
| Table of 1,000 pointers: remove one key and add another | 19.1 to 20.0 | 15.3 to 15.9 |
| Table of 1,000,000 pointers: remove one key and add another | 76.5 to 77.0 | 85.4 to 86.7 |

Small operations that cross source files are faster with it, as they were
when it was turned on (#76). The table of a million entries is 12% slower,
and the cause is not in the code of the table. The disassembly of
`tb_table_put` and `tb_table_remove` in the two libraries differs only in
that the optimized one computes the hash in place, where the other calls a
function for it: the loops that search and that close a gap are the same
instruction for instruction, and the optimized functions are the shorter.
At a million entries every operation waits for memory that is not in a
cache, and what decides its speed then is how the processor overlaps those
waits, which a difference outside the loops can change. Telling which needs
the processor's own counters, and a profiler that reads them needs rights
that the development machine does not give. The cost is in one benchmark of
a size the engine has no use for yet, and the gain is in every call that
crosses a source file. The "benchmarks against the base" job of CI will show
whether a change moves either.

## Benchmarks

`tb_bench` times the public operations of the foundation modules and prints
the nanoseconds each takes; [Benchmarks](benchmarks.md) explains how to run it,
how to compare two builds, and how to add a case. A change made for speed
reports the numbers before and after. The tests run every benchmark once at a
small size, so that none can rot, and never depend on a timing.

## CI

Windows CI explicitly selects the `Visual Studio 18 2026` generator, `x64`, and
MSVC. CMake builds the generated Visual Studio projects through MSBuild, including
the external consumer projects and standalone sanitizer executables. The Ninja
presets are local convenience settings and do not select the CI generator.

The CI workflows run on pushes to `main`, pull requests, manual dispatch, and
release validation:

- [CI](../.github/workflows/ci.yml): Debug and Release on Windows/MSVC,
  macOS/AppleClang, and Linux with both GCC and Clang; C formatting is checked
  separately with the runner's preinstalled clang-format 22, and so is
  [use of the foundation layer](#the-foundation-check). Release jobs also
  package, extract, and launch each native desktop export. One job builds the
  engine with the [default platform adapter](platforms.md#unsupported-platforms),
  and four run [static analysis](#static-analysis) with the analyzer of each
  compiler. Three measure [coverage](testing.md#coverage), one on each
  platform, and fail when the tests run less than 80% of the lines of any
  source of the engine.
- [Sanitizers](../.github/workflows/sanitizers.yml): standalone Debug sanitizer tests
  on the same four toolchains, including the platform adapter's tests, and the
  same tests under ThreadSanitizer on Linux with GCC and Clang and on macOS.

OS labels are explicitly pinned to `windows-2025-vs2026`, `macos-26`, and
`ubuntu-26.04`, the current stable hosted images checked on November 12, 2026.
See GitHub's [runner image inventory](https://github.com/actions/runner-images)
and [Ubuntu GA announcement](https://github.com/actions/runner-images/issues/14747).
Checkout is pinned to the commit for
[actions/checkout v7.0.1](https://github.com/actions/checkout/releases/tag/v7.0.1).
Runner software receives GitHub's normal image updates; review these pins when
adopting a newer stable OS. CI uses only tools already on those images. The [release workflow](../.github/workflows/release.yml)
publishes verified native exports after all tagged-build checks pass.

## Layout and development

- `include/badger/`: documented public C APIs, with `tb_` / `TB_` prefixes.
- `src/core/`: platform-independent library logic.
- `src/jobs/`: the [job system](jobs.md), which runs work on every core.
- `src/platform/{win32,linux,macos}/`: CMake-selected native implementations.
- `src/platform/default/`: the adapter for [any other platform](platforms.md#unsupported-platforms),
  whose every function reports "not implemented".
- `src/settings/`: [settings](settings.md) and the engine's catalog of them.
- `src/main.c`, `src/burrow_settings.c`: the `burrow` entrypoint and its
  settings; `src/burrow_defaults.cfg` is Burrow's default settings file, which
  the build embeds in the executable.
- `tests/`: unit tests and platform integration checks.
- `benchmarks/`: `tb_bench`, which times the operations of the foundation
  modules. [Benchmarks](benchmarks.md) explains how to run it and how to read it.
- `tests/framework/`: the in-house `tb_test` unit test framework and its tests.
- `tests/support/`: the test entry point, which turns the engine's fatal
  conditions into test results and checks every case for leaks.
- `tools/check_foundation.py`: fails when C code uses the C library where the
  foundation layer has an equivalent. See [the foundation check](#the-foundation-check).
- `VERSION`: the single version definition; generated headers live in `build/`.

[Platform contracts](platforms.md) explain the implemented interface and the
future platform work. [AGENTS.md](../AGENTS.md) and
[CONTRIBUTING.md](../CONTRIBUTING.md) describe contribution requirements.
`make doc` generates API docs when Doxygen is already installed; `make tags`
uses an existing Universal Ctags installation. Formatting can also run directly:

```sh
cmake -DTB_FORMAT_MODE=write -P cmake/Format.cmake
cmake -DTB_FORMAT_MODE=check -P cmake/Format.cmake
```

Formatting finds every `.c`, `.h`, and `.h.in` file under `benchmarks`, `cmake`,
`include`, `src`, and `tests`, so a new source or header is covered without
registering it.
Those directories are named in `cmake/FormatSources.cmake`. The check fails when
git tracks a C source or header outside them; add the new directory there.

## The contract check

`tools/check_contracts.py` reads every header under `include/badger/` and
fails on a public function that states no
[threading contract](conventions.md#threading): thread-safe, stage-owned, or
externally synchronized. CI runs it with the foundation check, and it is the
`source_checks.contracts` test.

## The foundation check

The foundation modules give the engine one implementation of allocation,
strings, formatted output, assertions, and the platform services: one place
that counts, checks, and reports, with the same behavior on every platform.
That holds only while code uses them, so `tools/check_foundation.py` fails on
any use of a C library or operating system function that a foundation module
replaces:

| In place of | Use | From |
| --- | --- | --- |
| `malloc`, `calloc`, `realloc`, `free`, `strdup`, `alloca`, `aligned_alloc`, `HeapAlloc`, `VirtualAlloc`, `mmap`, and their relatives | `tb_malloc`, `tb_calloc`, `tb_realloc`, `tb_free`, `tb_aligned_alloc`, `tb_str_dup` | [`memory.h`](memory.md) |
| `printf`, `fprintf`, `puts`, `fputs`, `putchar`, `perror`, and their relatives | `tb_fmt_print`, `tb_fmt_print_error`, `tb_fmt_write` | [`fmt.h`](strings.md#formatting) |
| `sprintf`, `snprintf`, `vsnprintf`, and their relatives | `tb_fmt_buffer`, `tb_fmt_string` | [`fmt.h`](strings.md#formatting) |
| `fflush` | `tb_fmt_flush` | [`fmt.h`](strings.md#formatting) |
| `strlen`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr`, `strspn`, `strcspn`, `strpbrk` | `tb_str_length`, `tb_str_equal`, `tb_str_compare`, `tb_str_find_char`, `tb_str_find`, `tb_str_span`, `tb_str_find_any` | [`str.h`](strings.md#c-strings) |
| `strcpy`, `strncpy`, `strcat`, `strncat`, `strtok`, and their relatives | `tb_str_dup`, `tb_str_sub`, `tb_str_cat`, `tb_str_join`, `tb_fmt_buffer` | [`str.h`](strings.md#c-strings) |
| `assert`, `static_assert`, `_Static_assert`, `<assert.h>` | `TB_ASSERT`, `TB_ASSERT_MSG`, `TB_STATIC_ASSERT` | `assert.h` |
| `abort` | `TB_ASSERT`, or `tb_platform_abort` after reporting | [`platform.h`](platforms.md#abnormal-termination) |
| `timespec_get`, `clock_gettime`, `QueryPerformanceCounter`, `mach_absolute_time`, and other native clocks | `tb_platform_clock_nanoseconds` | [`platform.h`](platforms.md#clock) |
| `<stdatomic.h>`, `_Atomic`, `Interlocked...`, `__atomic_...`, `__sync_...` | `tb_platform_atomic_size`, `tb_platform_atomic_u32`, `tb_platform_atomic_pointer` | [`platform.h`](platforms.md#atomic-values) |
| `<pthread.h>`, `<threads.h>`, `pthread_...`, `thrd_...`, `mtx_...`, `cnd_...`, `CreateThread`, the Win32 lock and condition variable functions | `tb_thread`, `tb_mutex`, `tb_condition` | [`thread.h`](platforms.md#threads-mutexes-and-condition-variables) |
| `thread_local`, `__thread`, `tss_...`, `TlsAlloc`, `FlsAlloc` and their relatives | The C keyword `_Thread_local`, or `tb_thread_local` for a place made at run time | [`thread.h`](platforms.md#values-that-each-thread-has-for-itself) |
| `<semaphore.h>`, `sem_...`, `CreateSemaphoreW`, `dispatch_semaphore_...` | `tb_semaphore` | [`thread.h`](platforms.md#semaphores) |
| `_mm_pause`, `YieldProcessor`, `sched_yield`, `SwitchToThread` | `tb_thread_relax`, `tb_thread_yield` | [`thread.h`](platforms.md#waiting-without-sleeping) |
| `Sleep`, `nanosleep`, `clock_nanosleep`, `usleep`, waitable timers | `tb_platform_sleep`, `tb_platform_wait_until` | [`platform.h`](platforms.md#sleeping-and-pacing) |
| `GetSystemInfo`, `GetLogicalProcessorInformationEx`, `sched_getaffinity`, `get_nprocs`, `_SC_NPROCESSORS_ONLN`, and other native counts of processors | `tb_system_get_processors` | [`system.h`](platforms.md#processors) |
| `BCryptGenRandom`, `RtlGenRandom`, `rand_s`, `getrandom`, `getentropy`, `arc4random` and its relatives, `SecRandomCopyBytes` | `tb_random_bytes` | [`random.h`](platforms.md#random-bytes) |
| `rand`, `srand`, `rand_r`, `random`, `srandom`, and the `drand48` family | `tb_rng` and its functions | [`rng.h`](random.md) |
| `qsort`, `qsort_s`, `qsort_r`, `bsearch`, `bsearch_s` | `tb_sort`, `tb_sort_stable`, `tb_sort_lower_bound` | [`sort.h`](collections.md#sorting) |
| `<search.h>` | `tb_table`, `tb_set`, `tb_list` | [`table.h`](collections.md) |

`python tools/check_foundation.py --list` prints every name. The check reads
every C source and header that clang-format maintains, in the directories that
`cmake/FormatSources.cmake` names, and reports each use like a compiler error,
with what to use instead:

```
src/core/table.c:88: error: strlen is not allowed here; use tb_str_length (see docs/strings.md)
```

It runs in CI as the job "Foundation layer use" and as the CTest test
`source_checks.foundation`, so `ctest --preset debug` catches a violation
locally. Comments and string literals are ignored. The names are reserved
everywhere else: a variable or structure member called `free` or `abort` is
reported too, so pick another name.

**What is not reported.** A function is reported only when the foundation has
an equivalent today. `memcpy`, `memset`, `memcmp`, `strtoul`, `qsort`, `exit`,
and the file functions have none, and stay allowed. When a module gains the
equivalent, add its rule to `RULES` in the script in the same change, with the
self-test cases that go with it.

**Exceptions.** A file that must use a replaced function is authorized by
name in the `AUTHORIZED` table of the script, with its reason. There is no
in-source suppression. The table holds three kinds of entry:

- The module that implements the replacement: `src/core/memory.c` on `malloc`,
  `src/core/str.c` on `strlen`, `src/core/fmt.c` on `fflush`, and
  `src/platform/` on the native clocks, atomics, threads, locks, and `abort`. The platform
  adapters are the layer beneath the foundation modules, built and tested
  without them, so an adapter also reports a failure with `fputs`.
- A benchmark or test that sets a module beside the C library on purpose: the
  `snprintf` and `strstr` columns of `tb_bench`, and the tests that check
  formatting, strings, and the clock against an implementation that shares no
  code with them.
- The test framework under `tests/framework/`, which cannot link the engine it
  tests.

An authorization that a file no longer uses fails the check, so an exception
ends when its reason does.

The scaffold was created with `cppboot --name tenacious-badger --shared --no-git
--no-fmt --no-codespaces`, then adapted to C17. Its Apache-2.0
[license](../LICENSE) is retained.

Foundation APIs are also checked together by an independent C17 file-copy
utility built against the installed shared and static SDKs. CI runs its Unicode,
binary I/O, reserved-arena, logging, and replacement scenarios in Debug and
Release, and reruns them against the extracted SDK archive. See the
[consumer example](../tests/consumers/foundation/README.md).
