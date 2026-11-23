# Shaders

Shaders are written once, in HLSL, and run on every backend, so players can
switch backends mid-game without stranding a shader. Where a convention is
open, the engine follows Unreal's. `badger/shader.h` is the compiler.

```c
const tb_shader_loader loader = tb_shader_vfs_loader(vfs); /* Engine/ and Game/ are mounted */
const tb_shader_define defines[] = {{"NUM_LIGHTS", "4"}, {"SM6_PROFILE", NULL}};
tb_shader_preprocess_request request = {0};
request.path = "/Engine/Private/DeferredLighting.hlsl";
request.defines = defines;
request.define_count = 2;
request.loader = &loader;

tb_shader_preprocessed output = {0};
if (tb_shader_preprocess(&request, arena, &output) != TB_SHADER_OK)
{
    for (size_t index = 0; index < output.diagnostic_count; index++)
    {
        char line[512] = {0};
        (void)tb_shader_diagnostic_format(&output.diagnostics[index], line, sizeof(line));
        (void)tb_log_printf(log, TB_LOG_ERROR, "shader", "%s", line);
    }
}
```

## How shaders reach the GPU

**One compiler, two times.** The engine's shader module is the compiler.
`badger-shaderc`, our counterpart of Unreal's ShaderCompileWorker, drives it
ahead of time, from CMake and when cooking. The engine calls it at run time:
for hot reload in development, for shaders a game makes while it runs, and for
whatever a game's shader libraries lack, where the game allows it. Both
produce the same bytes, and share one cache keyed on the sources, the defines,
the compiler's version and the target.

**What each backend gets.** The compiler reads HLSL into a form of its own,
and writes each backend's input from that:

| Backend | The compiler writes | Then |
| --- | --- | --- |
| D3D12 | HLSL | DXC to DXIL: the Windows SDK's `dxc` when building, `dxcompiler.dll` at run time |
| D3D11 | HLSL, Shader Model 5 | `D3DCompiler_47.dll`, part of Windows |
| Vulkan | SPIR-V | the driver |
| OpenGL | GLSL | the driver |
| Metal | Metal Shading Language | the OS |
| Software | the compiler's own form | the software renderer, on the CPU |

DXC is the only compiler that writes DXIL, so D3D12 goes through it, and gets
each new shader model as Microsoft ships it. SPIR-V, GLSL and Metal are
written by our compiler, so compiling at run time works on Linux, the Steam
Deck and macOS with no SDK installed.

**Shipping.** A packaged game carries a shader library for each backend its
platform builds, as Unreal's packaged games do, so switching backends loads
shaders instead of compiling them.

**Feature levels.** Each shader records what it needs: the portable core
(raster, compute, wave operations and bindless resources), inline ray
queries, ray-tracing pipelines, mesh shaders, or features only D3D12 has. A
backend that lacks one reports the shader unavailable, and the renderer falls
back to what it can run, saying why.

## Files and virtual paths

As in Unreal, a shader with entry points is a `.hlsl` file (Unreal's `.usf`),
and a file it includes is a `.hlsli` file (Unreal's `.ush`). Every file is
named by an absolute virtual path:

| Root | Holds |
| --- | --- |
| `/Engine/Public/` | Headers games include, such as `Common.hlsli` |
| `/Engine/Private/` | The engine's own shaders |
| `/Engine/Generated/` | Files a compile makes, such as a material's code, supplied in memory |
| `/Game/` | A game's shaders |
| `/Plugin/<name>/` | A plugin's shaders |

tb_shader_path_check() says whether a path is valid. A path starts with `/`,
has a root and at least one more component, and ends in `.hlsl` or `.hlsli`.
No component is empty, `.` or `..`, and no byte is a `\`, a `:`, a space or a
control character, so a path means the same thing on every system. The case
of a path matters, as it does in the VFS.

**Where files come from.** A compile reads files through a loader
(tb_shader_loader). tb_shader_vfs_loader() reads them from a VFS, in which
`/Engine/Public/Common.hlsli` is the name `Engine/Public/Common.hlsli`. The
engine mounts its shader directory at `Engine` and a game at `Game`, and a mod
mounted over a game overrides its shaders as it overrides its other files.

A compile can also supply files of its own in memory (`files` in the
request). They win over the loader's, and suit generated files such as
`/Engine/Generated/Material.hlsli`, and shaders a game builds at run time.

## The preprocessor

tb_shader_preprocess() reads a main file and everything it includes, and
applies the directives and macros, as DXC's preprocessor does. Its output is
the preprocessed text, every file the compile read, and its diagnostics.

**Defines** come from the request, as Unreal's compilation environment sets
them, and are defined before the main file is read: `{"NUM_LIGHTS", "4"}` is
`#define NUM_LIGHTS 4`, a value of NULL is `1`, and `""` defines the macro as
empty. A value may not hold a line break.

**Includes** name a virtual path in quotes, or a path relative to the
including file, in which `.` and `..` apply: `#include "../Public/Common.hlsli"`.
A path that climbs above its root is an error. `#include <...>` is an error
too, because a shader include always names a virtual path. An include may be
a macro that expands to a string. `#pragma once` and include guards both
keep a file from being read twice.

**Macros** are object-like and function-like, with `#` to make a string of an
argument, `##` to paste two tokens, `...` and `__VA_ARGS__`, and
`, ## __VA_ARGS__`, which drops the comma when there are no variadic
arguments. A macro never expands inside its own expansion. `__LINE__` and
`__FILE__` say where they are. Redefining a macro differently is a warning;
redefining it the same is allowed.

**Conditionals** are `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else` and `#endif`,
with `defined X` and `defined(X)`. A conditional ends in the file that opens
it. The arithmetic of `#if` is on signed 64-bit integers, in decimal, `0x`
hexadecimal, `0b` binary and `0` octal, with any suffix of `u` and `l`.
Overflow, division by zero and a shift by a negative count or more than 63 are
errors, except in operands that `&&`, `||` and `?:` do not evaluate. A name
that is not a macro is 0, and `true` is 1, as in HLSL. A floating-point number
is an error.

**Other directives.** `#line N` and `#line N "name"` say where the following
lines are reported from, as generated code needs. `#error` fails the compile
with its text and `#warning` adds a warning. Every other `#pragma`, such as
`#pragma pack_matrix(row_major)`, is passed on in the text for the compiler.
An unknown directive is an error. A group that a conditional skips is not
read, so its directives and its text need not be valid.

**The text** keeps each token on the line it came from, and starts at line 1,
so a later compiler's line numbers match the source. Comments are removed and
space between tokens is one space. A jump of more than a few lines, or into
another file, is one line break, or with `TB_SHADER_LINE_MARKERS` a `#line`
that names the file and line.

**Dependencies** are the paths of every file the compile read, the main file
first, each once, in the order it was first read: what the shader cache keys
on, and what hot reload watches.

## The parser

The parser reads the preprocessed tokens into a syntax tree, which type
checking (#229) reads next. The tree records what the source says, not yet
what it means. The parser reads HLSL 2021:

- **Declarations:** structs and classes, with methods, bases and bit fields;
  interfaces; enums and `enum class`; functions, with their parameters,
  default values and semantics; variables; `cbuffer` and `tbuffer`;
  `ConstantBuffer<T>` and every resource type; `typedef`; namespaces; and
  templates of structs and functions.
- **What follows a `:`:** semantics, `register(t0, space1)`,
  `packoffset(c1.y)`, and a ray payload's `read(...)` and `write(...)`.
- **Attributes** before declarations and statements, as `[numthreads(8, 8, 1)]`,
  `[shader("closesthit")]` and `[unroll(4)]`, and as `[[vk::binding(0)]]`.
- **Modifiers**, such as `static`, `groupshared`, `row_major`, `nointerpolation`
  and `inout`. Some words are modifiers only before a declaration's type:
  geometry shaders' `triangle`, mesh shaders' `indices`, `vertices`,
  `primitives` and `payload`, and interpolation's `linear`, `centroid`,
  `noperspective` and `sample`. Elsewhere they are ordinary names, so
  `float sample = 1;` is a variable.
- **Every statement and expression**, with C's precedence; casts,
  constructors such as `float4(v, 1)`, swizzles, initializer lists, and
  template arguments on calls, as in `buffer.Load<float4>(offset)`.
- **Pragmas** the preprocessor passes on, where they are, in order.

**Types and values.** As in C, the parser tells a type from a value by what
each name was declared as, scope by scope: `(T)x` is a cast when `T` is a
type, and `T(x)` builds a `T`; when `T` is a variable, `(T)` is a value and
`T(x)` a call. A local name hides a type of the same name.

**Errors** name the file, line and column of the token at fault:
`expected ';', found 'float'`. After one, the parser skips to the end of the
statement or declaration, and reads on, so one compile reports what it can.

**Numbers.** Integers are decimal, `0x` hexadecimal, `0b` binary or `0`
octal, with a suffix of at most one `u` and two `l`s; one that does not fit
in 64 bits is an error. Floating-point numbers take a suffix of `f`, `h` or
`l`. A character constant is an error, because HLSL has none.

## Diagnostics

Each error and warning has a virtual path, a line and a column, counted from
1, with columns in bytes. tb_shader_diagnostic_format() writes one as Visual
Studio and other editors read it:

```
/Engine/Private/Lighting.hlsl(12,5): error: macro 'SHADE' takes 2 arguments, but 1 was given
```

An error about the request itself, such as a main path that is not valid, has
no path and is written `error: ...`. After a `#line`, diagnostics report the
lines and file it gives. A compile stops after `TB_SHADER_MAX_ERRORS` errors,
with a last error that says so.

**Results.** OK means the text is complete; warnings may be among the
diagnostics. FAILED means the sources have errors, and the text is empty.
INVALID means the request is malformed: a main path that is not a valid `.hlsl`
path, a define that is not an identifier, or a file without a valid path.
NO_MEMORY means the caller's arena could not hold the output; the compile's
own work comes from an arena of its own, and running out of memory there ends
the process, as allocation does.

## Limits

Sources are untrusted: a mod or a downloaded shader is read by the same code.
Every way a source could make the compiler do unbounded work is bounded:

| Limit | Value |
| --- | --- |
| Files included inside one another, the main file counting as one | `TB_SHADER_MAX_INCLUDE_DEPTH`, 64 |
| Bytes of one file | `TB_SHADER_MAX_FILE_SIZE`, 16 MiB |
| Tokens the preprocessor makes, from files and macros alike | `TB_SHADER_MAX_TOKENS`, about a million, or the request's `max_tokens` |
| Macro invocations nested in one another's arguments, and parentheses in `#if` | 256 |
| Statements, expressions and types nested in one another | 256 |
| The height of an expression's tree, operators chained without parentheses included | 1024 |
| Errors before a compile stops, the preprocessor's and the parser's together | `TB_SHADER_MAX_ERRORS`, 64 |

A file that holds a NUL byte is refused.

## Threading

tb_shader_preprocess() is thread-safe when each call has its own arena and the
loader is thread-safe. The VFS loader is, once mounting is done, because the
VFS reads from any number of threads at once.
