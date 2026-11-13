# Testing

Tenacious Badger tests itself with an in-house C17 framework, `tb_test`, in
`tests/framework/`. It has no dependencies, registers test cases without manual
lists, fails fast on request, and exposes every case to CTest so that IDE test
explorers can list, run, debug, and jump to individual cases.

The API reference is the documented header
[`tests/framework/include/tb_test.h`](../tests/framework/include/tb_test.h).

## Writing a test

```c
#include "badger/core.h"

#include "tb_test.h"

TB_TEST(core_sum, adds_small_numbers)
{
    int32_t result = {0};

    TB_TEST_REQUIRE(tb_sum_i32(2, 3, &result));
    TB_TEST_EXPECT_INT(result, ==, 5);
}
```

`TB_TEST(suite, name)` defines a case; its full name is `suite.name`. Put the
case in a test source that is already built, or add a new source (see
[Adding tests](#adding-tests)), and build. There is nothing else to register.

Every test case in the project is part of one executable, `tb_tests`. It runs
all cases, any selection of them, in any order.

Name a case after the behavior it proves, and open each test file with a comment
that says what is being checked and why. A failing test should tell its reader
whether the code or the test is wrong.

## Checks

Every check has two strengths:

- `TB_TEST_EXPECT*` records a failure and continues, so one run reports
  everything that is wrong.
- `TB_TEST_REQUIRE*` records a failure and ends the case. Use it when the rest
  of the case cannot run meaningfully, such as after a failed setup step. It
  works from helper functions at any depth.

| Check | Compares | Example |
| --- | --- | --- |
| `TB_TEST_EXPECT(condition)` | truth of a condition | `TB_TEST_EXPECT(list_is_empty(list))` |
| `TB_TEST_EXPECT_INT(a, op, b)` | signed integers | `TB_TEST_EXPECT_INT(delta, >=, -1)` |
| `TB_TEST_EXPECT_UINT(a, op, b)` | unsigned integers, sizes, counts | `TB_TEST_EXPECT_UINT(length, <=, capacity)` |
| `TB_TEST_EXPECT_HEX(a, op, b)` | unsigned values, reported in hex | `TB_TEST_EXPECT_HEX(hash, ==, 0xCBF29CE4u)` |
| `TB_TEST_EXPECT_FLOAT(a, op, b)` | floating-point values, exactly | `TB_TEST_EXPECT_FLOAT(scale, >, 0.0)` |
| `TB_TEST_EXPECT_FLOAT_NEAR(a, b, tolerance)` | floating-point values, within a tolerance | `TB_TEST_EXPECT_FLOAT_NEAR(length, 1.0, 1e-6)` |
| `TB_TEST_EXPECT_POINTER(a, op, b)` | addresses | `TB_TEST_REQUIRE_POINTER(node, !=, NULL)` |
| `TB_TEST_EXPECT_STRING(a, op, b)` | NUL-terminated strings | `TB_TEST_EXPECT_STRING(name, ==, "badger")` |
| `TB_TEST_EXPECT_BYTES(a, b, size)` | byte ranges | `TB_TEST_EXPECT_BYTES(output, expected, sizeof(expected))` |
| `TB_TEST_EXPECT_TRAP(body, context, text)` | that code hits a fatal condition | see [Contract violations](#contract-violations) |

`op` is a C comparison operator: `==`, `!=`, `<`, `<=`, `>`, or `>=`. Replace
`EXPECT` with `REQUIRE` for the fatal form. Operands are evaluated exactly once.

Prefer a typed check to `TB_TEST_EXPECT(a == b)`: it reports both values.

```
sum_test.c(31): error: core_sum.adds_small_numbers: TB_TEST_EXPECT_INT(result, ==, 5) failed with 6 == 5
```

Pick the check by the type of the operands. GCC and Clang reject a sign-changing
conversion, so passing a `size_t` to `TB_TEST_EXPECT_INT` fails the build instead
of comparing the wrong values.

Two more macros end a case immediately:

- `TB_TEST_FAIL("unexpected tag %d", tag)` fails it with a formatted message.
- `TB_TEST_SKIP("needs a GPU adapter")` marks it skipped. Skips appear in the
  summary and in CTest, which keeps a case that cannot run here visible.

## Many inputs

Foundation code is tested with many inputs of varied shape. Two tools keep the
resulting failures readable.

**Context.** `TB_TEST_CONTEXT(format, ...)` describes the input now under test.
Every later failure in the case repeats it. Set it at the top of each loop
iteration:

```c
for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
{
    TB_TEST_CONTEXT("row %zu: \"%s\"", index, rows[index].text);
    TB_TEST_EXPECT_UINT(text_length(rows[index].text), ==, rows[index].length);
}
```

```
text_test.c(40): error: text.measures_length: TB_TEST_EXPECT_UINT(...) failed with 4 == 5
    context: row 7: "naïve"
```

**Deterministic random inputs.** `tb_test_random_u64()`,
`tb_test_random_below(bound)`, `tb_test_random_between(minimum, maximum)`, and
`tb_test_random_bytes(buffer, size)` draw from a sequence that depends only on
the run's seed and the case's name. A case sees the same values on every
platform, whichever other cases run, in whatever order.

The seed is fixed by default, so CI is reproducible. A failed case prints the
command-line arguments that replay it:

```
[ FAILED  ] ring.survives_random_operations: 1 of 5210 checks failed; rerun with: ring.survives_random_operations --seed 0x7462746573740001
```

Explore new inputs with `--seed time` or `--repeat 100` (each round adds one to
the seed). Good randomized tests compare the code under test with a simple
reference implementation, and bias some inputs toward limits where decisions
are made. `tests/core/sum_test.c` shows both.

## Contract violations

Engine code states its preconditions with `TB_ASSERT` (see
`include/badger/assert.h`). A test proves that a violation is caught with a trap
check. The violating code goes in its own function:

```c
static void sum_into_null(void *context)
{
    (void)context;
    (void)tb_sum_i32(1, 2, NULL);
}

TB_TEST(core_sum, null_output_violates_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(sum_into_null, NULL, "result != NULL");
}
```

The last argument is text the failure message must contain, which proves the
intended assertion fired rather than some other one. Release builds compile
assertions out, so the case skips itself there.

Engine test executables install an assertion handler that reports a failed
`TB_ASSERT` to the framework (`tests/support/engine_main.c`). An assertion that
no check expected fails the running case, at the location of the assertion, and
the run continues with the next case.

A trap leaves the code under test with a non-local jump. Nothing after the
failing statement runs and nothing it acquired is released, so keep trap bodies
to the one call that violates the contract.

## Adding tests

To add a case, write a `TB_TEST` in a test source. To add a test source, list
it in `TB_TEST_CASE_SOURCES` in `tests/CMakeLists.txt`:

```cmake
set(TB_TEST_CASE_SOURCES
    core/assert_test.c
    core/sum_test.c
    ...)
```

All of them build into `tb_tests`. One executable keeps the build small and
gives one place to run anything; a new module adds a source file, not a program.

Name suites `<module>_<feature>`, such as `core_sum` or `codec_base64`. Suite
names share one namespace, and this form lets a pattern select a module
(`core_*`) or a feature (`core_sum.*`).

Each case is also the CTest test `<suite>.<name>`, for example
`core_sum.adds_small_numbers`.

Two things cannot be cases of `tb_tests`, because they end the process: the
framework's own failure reports and the engine's default assertion handler. Each
has a small helper executable that a CTest script runs as a child process
(`tb_test_fixture` and `tb_probe`). Follow that pattern for other
behavior that must terminate, such as a platform's fatal stubs.

### How cases are found

When a test executable builds, a CMake script scans its `SOURCES` for
`TB_TEST(suite, name)`. From what it finds it generates the registry that the
executable runs and the CTest entry for each case, including the case's file and
line. No compiler extension is involved and the executable is never run to
discover its cases, so registration behaves the same on every compiler and also
works for binaries the build machine cannot execute.

The scan places three rules on a definition:

1. `TB_TEST(` starts its line (indentation is fine).
2. Both names are plain identifiers written on that line. A macro that expands
   to `TB_TEST` is not seen.
3. The definition is not hidden from the compiler by `#if` or a block comment.
   Breaking this rule fails the link with an unresolved `tb_test_case_...`
   symbol. To disable a case, delete it or start it with `TB_TEST_SKIP`.

Two cases with the same `suite.name` in one executable, and an executable with
no cases at all, fail the build with a message.

## Running tests

```sh
ctest --preset debug                       # everything
ctest --preset debug -R '^core_sum\.'      # one suite
ctest --preset debug --stop-on-failure     # stop at the first failed test
```

CTest starts one process per case, which isolates cases from each other and is
what CI runs. Running `tb_tests` directly runs the selected cases in a single
process, which is the fastest loop while working on a module:

```sh
build/debug/bin/tb_tests                            # every case
build/debug/bin/tb_tests 'core_*'                   # one module
build/debug/bin/tb_tests 'core_sum.*'               # one suite
build/debug/bin/tb_tests core_sum.rejects_overflow_and_preserves_the_output
build/debug/bin/tb_tests 'core_assert.*' core_sum.adds_representable_operands
build/debug/bin/tb_tests --list                     # names and source locations
build/debug/bin/tb_tests --fail-fast                # stop at the first failed case
build/debug/bin/tb_tests --help
```

Cases run in the order of the arguments, and in source order within one
pattern, so naming cases is also how to run them in a chosen order. A case that
several arguments match runs once.

| Option | Effect |
| --- | --- |
| `pattern...` | Run cases whose `suite.name` matches any pattern, in the order given. `*` matches any run of characters, `?` exactly one. |
| `--exclude PATTERN` | Leave out matching cases. May be repeated. |
| `--list` | Print the selected cases with their locations and exit. |
| `--fail-fast` | Stop the run at the first failed case. |
| `--break` | Break into the debugger at a failed check. |
| `--seed VALUE` | Seed for random inputs and `--shuffle`: a number, or `time`. |
| `--shuffle` | Run cases in seeded random order, to expose hidden dependencies between them. |
| `--repeat COUNT` | Run the selection `COUNT` times with advancing seeds. |
| `--verbose` | Report every case, not only failures and skips. |

Exit status: `0` passed, `1` a case failed, `2` bad command line or no case
matched, `77` every selected case was skipped.

Output is quiet by default: failures, skips, and a one-line summary.

## IDE integration

The integration rests on three things the framework produces, all standard:

- **One CTest test per case**, named `suite.case`.
- **The source location of each case**, in the CTest property
  `DEF_SOURCE_LINE`, the same property CMake's GoogleTest support sets.
- **Failures in compiler diagnostic form**: `file(line): error: ...` on Windows
  and `file:line: error: ...` elsewhere.

What that gives each tool:

- **VS Code** (CMake Tools): cases appear in the Testing view as a
  `suite > case` tree; each can be run or debugged alone, "Go to Test"
  opens its definition, and a failed check is shown inline at its line. The
  repository's `.vscode/` settings configure the tree, and `launch.json`
  provides debug configurations that stop at the failing check (`--break`).
  Debugging needs the C/C++ extension recommended by the workspace.
- **Visual Studio**: open the folder; Test Explorer lists the CTest cases and
  runs or debugs them. Double-click a failure line in the Output window to open
  its source.
- **CLion**: the CTest cases appear in the "All CTest" run configuration and can
  be run or debugged individually; failure locations in the console are links.
- **Terminals and editors** that understand compiler diagnostics (Vim's
  quickfix, Emacs compilation mode) can step through failures in the output of
  `ctest --output-on-failure` or of a test executable.

The test list refreshes when a build regenerates it, so build after adding a
case.

To debug without an IDE's test integration, start the executable under a
debugger with the case name and `--break`:

```sh
build/debug/bin/tb_tests core_sum.adds_small_numbers --break
```

## Memory in tests

The test executable watches the engine's allocations for every case. The
details are in [Memory allocation](memory.md#in-tests); in short:

- A case that passes its checks but leaves blocks allocated fails with
  `memory leak: 2 blocks allocated by this case and never freed`. A case that
  already failed is not checked, because a failed `REQUIRE` skips the cleanup
  after it.
- `tb_engine_test_fail_allocations_after(n)` makes allocations fail from the
  `n`-th attempt on, to test how code behaves when memory runs out.
- An exhausted fail-fast allocation is a trap whose message starts with
  `out of memory`.

These are built on case hooks: `tb_test_set_case_hooks` registers functions
that the framework runs before and after every case, inside the case, so they
can use checks. The after hook runs however the body ended.

## Sanitizers

`TB_ENABLE_SANITIZERS=ON` (the `sanitizer` preset) builds instrumented copies of
the test executable, the framework, and the engine sources under test. Their
CTest names end in `.sanitized`.

## Testing the framework

The framework is tested in `tests/framework/selftest/`:

- Cases that must pass (`*_test.c`, built into `tb_tests`) cover the checks, the command line, pattern
  matching, and the random generator, including its exact output.
- Behavior that only shows when a case fails is checked from the outside.
  `fixture/fixture_tests.c` contains cases that fail, skip, trap, and crash on
  purpose; each function in `RunScenario.cmake` runs it and verifies the exit
  status and the report. The same script tests the source scanner.

When you change what the framework prints or how it exits, update the matching
scenario in the same change.
