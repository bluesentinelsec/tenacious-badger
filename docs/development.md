# Tenacious Badger

A dependency-free C17 foundation for the `badger` game-engine shared library
and its separate `burrow` showcase executable. The same modules also build into
the `badger_static` static library. [Library consumption](consuming.md) covers
FetchContent and the published Debug/Release SDKs. The binding design and engineering
standards are in [AGENTS.md](../AGENTS.md). The earlier
[vision draft](vision.md) is preserved.

This bootstrap implements checked signed addition, developer assertions, and one
native platform interface. `burrow` calls the shared library and prints `2 + 3 = 5`. Rendering,
windowing, gameplay, vendor integrations, and the other planned modules remain
future work.

The [release and distribution requirements](../AGENTS.md#releases-and-distribution)
define desktop exports and the later browser and mobile targets. See
[release instructions](releasing.md) for native archive generation and publishing.

## Requirements

- CMake 3.25 or newer, and a C17 compiler: MSVC, GCC, or Clang.
- The compiler's native SDK and standard C runtime. Windows needs Visual Studio
  with the MSVC C compiler tools and the Windows SDK.
- Ninja for the convenience presets; plain CMake commands below also work with
  the platform's default build tool.
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

The core tests cover checked addition (tables of ordinary and limit operands,
overflow rejection, and agreement with wide arithmetic on random operands) and
the assertion macros. Test checks are independent of `TB_ASSERT`, so they still
run in Release, where assertions are compiled out; cases that need an active
assertion skip themselves there. The framework has its own tests under
`tests/framework/selftest/`.

Integration tests exercise `burrow` through the shared library and probe the
selected platform adapter. On Windows, the native performance counter must be
monotonic. On Linux/macOS, a subprocess must exit with the exact fatal-stub
message and name the planned native API. Another subprocess checks that a failed
assertion reports its location and aborts.

`TB_ENABLE_SANITIZERS=ON` adds standalone Debug test executables that compile the
engine sources directly and link an instrumented copy of the test framework.
MSVC uses AddressSanitizer; GCC and Clang use AddressSanitizer plus
UndefinedBehaviorSanitizer. The shipping library and `burrow` remain free of
sanitizer instrumentation. Use Debug when running these tests.
`TB_BUILD_TESTS=OFF` builds only the library and consumer.

## CI

Windows CI explicitly selects the `Visual Studio 18 2026` generator, `x64`, and
MSVC. CMake builds the generated Visual Studio projects through MSBuild, including
the external consumer projects and standalone sanitizer executables. The Ninja
presets are local convenience settings and do not select the CI generator.

The CI workflows run on pushes to `main`, pull requests, manual dispatch, and
release validation:

- [CI](../.github/workflows/ci.yml): Debug and Release on Windows/MSVC,
  macOS/AppleClang, and Linux with both GCC and Clang; C formatting is checked
  separately with the runner's preinstalled clang-format 22. Release jobs also
  package, extract, and launch each native desktop export.
- [Sanitizers](../.github/workflows/sanitizers.yml): standalone Debug sanitizer tests
  on the same four toolchains, including fatal-stub checks.

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
- `tests/framework/`: the in-house `tb_test` unit test framework and its tests.
- `tests/support/`: the entry point shared by engine test executables.
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

The scaffold was created with `cppboot --name tenacious-badger --shared --no-git
--no-fmt --no-codespaces`, then adapted to C17. Its Apache-2.0
[license](../LICENSE) is retained.
