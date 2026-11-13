# Using the engine as a C17 library

All reusable logic belongs to focused modules with documented public APIs under
`include/badger/`. Both library variants expose the same `tb_` C ABI. The game is
an independent consumer, like an application using SDL. Platform implementations
are selected by the build; shared code contains no native OS headers.

## From source with CMake FetchContent

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyGame LANGUAGES C)
include(FetchContent)
FetchContent_Declare(tenacious_badger
    GIT_REPOSITORY git@github.com:bluesentinelsec/tenacious-badger.git
    GIT_TAG v0.1.0)
FetchContent_MakeAvailable(tenacious_badger)
add_executable(my_game main.c)
target_link_libraries(my_game PRIVATE TenaciousBadger::badger)
set_target_properties(my_game PROPERTIES
    C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
```

Embedded builds default to libraries only: `TB_BUILD_BURROW` and `TB_BUILD_TESTS`
are off. The engine has no package dependencies or dependency downloads. Source
consumption fetches only this project. For static linking, use
`TenaciousBadger::badger_static` instead; the target supplies `TB_STATIC` to headers.

## From a published SDK

Download the SDK for your OS, architecture, and Debug/Release configuration,
extract it, and set `CMAKE_PREFIX_PATH` to its root. Replace the FetchContent
section above with:

```cmake
find_package(TenaciousBadger CONFIG REQUIRED)
```

The SDK includes both library variants, all public/generated headers, CMake
package/target files, license notices, and build metadata. Windows SDKs also
include the dynamic import library. Debug SDKs contain available PDB/dSYM or
embedded debug information. Use the matching build configuration; keep different
SDK configurations in separate prefixes.

On Windows, static consumers must match the SDK's static C runtime (`/MT` for
Release, `/MTd` for Debug):

```cmake
set_property(TARGET my_game PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
```

When using the dynamic library, deploy its DLL beside the Windows executable.
On macOS/Linux, give your application the appropriate runtime library path and
bundle the engine with your distribution. The exported CMake target carries
include paths and linkage; it does not package the application's runtime files.
See [desktop releases](releasing.md) for Burrow's reference layouts.

## API example and interoperability checks

```c
#include "badger/core.h"

int main(void)
{
    int32_t result = {0};
    if (!tb_sum_i32(20, 22, &result) || result != 42)
    {
        return 1;
    }
    return 0;
}
```

Every desktop CI toolchain tests installed-library calls from Python `ctypes`, an
independently configured C17 program linked to the installed static library, and
a separate FetchContent consumer linked to the dynamic library. These tests run
in both Debug and Release. SDK archives are extracted and their C ABI is checked
again outside the build tree. Downstream programs can use the same documented
ABI without the engine's internal sources or a third-party wrapper.
