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
| `memory.h` | [Memory allocation](memory.md) that fails fast, with a replaceable allocator |
| `arena.h` | [Arenas](memory.md#arenas) for objects that share a lifetime |
| `allocator.h` | [Allocators chosen per object](memory.md#choosing-the-allocator-of-one-object) |
| `array.h`, `bit.h`, `ring.h`, `list.h`, `table.h`, `set.h` | [Collections](collections.md): a dynamic array, a bit vector, a ring, an intrusive list, a hash table, and a set |
| `str.h`, `text.h`, `fmt.h`, `atom.h`, `hash.h` | [Strings](strings.md): C strings, text values, formatted output, atoms, and hash functions |
| `platform.h` | Three native platform interfaces: time, abort, and an atomic counter |

`burrow` calls the shared library and prints `2 + 3 = 5`. Rendering, windowing,
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
make fmt
make check-format
```

The equivalent portable commands are `cmake --preset debug`,
`cmake --build --preset debug`, and `ctest --preset debug`; substitute `release`
or `sanitizer` for the other configurations. On Windows, run `build.bat test`,
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
failed assertion reports its location and aborts.

`TB_ENABLE_SANITIZERS=ON` adds standalone Debug test executables that compile the
engine sources directly and link an instrumented copy of the test framework.
MSVC uses AddressSanitizer; GCC and Clang use AddressSanitizer plus
UndefinedBehaviorSanitizer. The shipping library and `burrow` remain free of
sanitizer instrumentation. Use Debug when running these tests.
`TB_BUILD_TESTS=OFF` and `TB_BUILD_BENCHMARKS=OFF` build only the library and
consumer.

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
  package, extract, and launch each native desktop export.
- [Sanitizers](../.github/workflows/sanitizers.yml): standalone Debug sanitizer tests
  on the same four toolchains, including the platform adapter's tests.

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
- `src/platform/{win32,linux,macos}/`: CMake-selected native implementations.
- `src/main.c`: thin `burrow` entrypoint.
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
| `<stdatomic.h>`, `_Atomic`, `Interlocked...`, `__atomic_...`, `__sync_...` | `tb_platform_atomic_size` | [`platform.h`](platforms.md#atomic-counter) |
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
  `src/platform/` on the native clocks, atomics, and `abort`. The platform
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
