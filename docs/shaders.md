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

## Checking

Checking binds every name to what it names, decides the type of every
expression, and reports what HLSL does not allow, as DXC does. It writes
every implicit conversion into the tree, so what reads the tree next sees
each one. Types are named as HLSL writes them: `float4`, `half2x3`,
`Texture2D<float4>`, `int[3]`.

**Literals.** An integer or floating-point literal without a suffix has a
literal type, as in DXC, which takes the type its use asks for: in
`float x = 1;` the 1 is a float, and in `half h = 2 * h0;` the 2 is a half.
Integer literals have 64 bits. `u` makes a `uint`, `l` an `int64_t`, `ul` a
`uint64_t`; `f` a `float`, `h` a `half`, `l` a `double`.

**Conversions.** Numbers convert implicitly to one another, bools included.
A scalar converts to any vector or matrix, filling it. A vector converts to a
shorter one, and a matrix to a smaller one, with a warning that components
are dropped; never to a longer one. A vector and a matrix of as many
components convert to each other. A cast converts more: a struct or an array
from a scalar or from anything with as many components, component by
component. Converting a floating-point constant with a fraction to an
integer is a warning.

**Operators.** Arithmetic converts both operands to their common type:
floating point over integers, wider over narrower, unsigned over signed of
the same width. A scalar combines with a vector or a matrix; two vectors of
different lengths combine as the shorter, with a warning. Bitwise operators
take integers, and `%` takes floats too. In HLSL 2021, `&&`, `||` and `?:`
take scalar conditions; vectors use `and()`, `or()` and `select()`.
Comparisons give bools shaped as their operands.

**Overloads.** A call chooses the function whose parameters its arguments
convert to most cheaply, argument by argument: the same type, then a wider
type of the same kind, then a narrower one, then another kind; filling a
vector costs more than any of those, dropping components more, and dropping
all but one most. A call that no function is best for is ambiguous, as
`g(1)` is between `g(float)` and `g(double)`. An `out` or `inout` argument
must be assignable.

**What may be assigned.** A global that is not `static` is one of the
shader's inputs, as in DXC, and cannot be assigned; nor can a `const`, a
read-only resource's element, or a swizzle that repeats a component.

**Constants.** Array sizes, case values and template arguments must be
constant integers. Constant expressions fold as HLSL computes them: integers
wrap at their width, shift counts are masked to it, and casts truncate
toward zero.

**Statements.** Conditions are scalars. `break`, `continue`, `case` and
`default` must be where they belong, and a case value appears once in a
switch. A function that returns a value must return one on every path.

**Intrinsic functions.** HLSL's intrinsics are a table
(`src/shader/shader_intrinsics.h`): each one's name and the signatures of its
overloads, written as templates, as DXC's own table writes them. `lerp` is
`$1(float#1<a1>, $1, $1)`: three arguments of one type, any shape, of a
floating-point component. A call's arguments decide each template as DXC
decides it:

- The components of a template's arguments combine as an operator's operands
  do, so `max(int, uint)` is a `uint` and `min(half, float)` a `float`.
- A component outside the template's class becomes the class's fallback, so
  `sin(int)` is a `float` and `countbits(float)` takes a `uint`. Some classes
  take nothing else: `asfloat(uint64_t)` is an error.
- Shapes combine to the smallest vector or matrix among the arguments, and a
  scalar takes any shape: `lerp(float3, float3, float)` is a `float3`, and
  `max(float3, float4)` a `float3`, with a warning. A vector does not combine
  with a matrix. A parameter written as a scalar takes only a scalar, not
  even a `float1`: `WaveReadLaneAt(f, uint2(0, 0))` is an error.
- A literal stays literal where the class takes one: `max(1, 2)` is a
  literal int.

Then each argument converts to its parameter, and the overloads of a name are
chosen among as any call's are. `mul` has nine overloads, one for each pair of
a scalar, a vector and a matrix. The table covers arithmetic and
trigonometry, vectors and matrices, logic, bits and reinterpretation,
derivatives and pixel-shader attributes, barriers and atomics, wave and quad
operations, ray tracing, mesh shaders, `NonUniformResourceIndex` and
`printf`. A function of the shader's hides an intrinsic of its name.

**Methods of objects.** The methods of textures, buffers, streams and ray
queries are a second table (`src/shader/shader_methods.h`) in the same
language, following DXC's. A row names a method, the traits an object needs
to have it, and its overloads; an object has every row whose traits it has.
`Texture2D` and `TextureCube` share the rows of a sampled texture, `Texture2D`
adds those with offsets, and `TextureCube` those that take a clamp or a status
in an offset's place. Three things in a method's signatures come from its
object:

- `T` is the object's element, so `Texture2D<float>.Sample` returns a `float`
  and `StructuredBuffer<S>.Load` an `S`; `t` is the element's component, so
  `Texture2D<uint2>.Gather` returns a `uint4`.
- `P`, `O` and `L` are the sizes of a location to sample, of an offset or a
  gradient, and of a location to load: `Texture2DArray.Sample` takes a
  `float3` and an `int2`, and its `Load` an `int4`. An argument does not
  change them; a larger one is truncated, with a warning.
- `U` is the type a call names, `ByteAddressBuffer.Load<float4>(0)`, or else
  the type of `Store`'s value, a literal as a `uint` or a `float`, or else
  `uint`. Only `Load` and `Store` of a byte-address buffer name a type.

A texture with mip levels has a `mips` member, and a multisampled texture a
`sample` member: `t.mips[2][p]` is the texel at `p` of level 2, and
`ms.sample[3][p]` that of sample 3. The level or sample is an integer; the
location has exactly as many components as the texture's, as DXC requires,
so a `Texture2D` takes a `uint2` (or an `int2`, converted), never a `uint3`
or a `uint`. Each step has a type of its own, named as DXC names it:
`Texture2D<float>::mips_type`, then `Texture2D<float>::mips_slice_type`. A
texel read so is read-only, except a writable texture's sample.

**Not yet.** Templates come later, and so do these intrinsics: the
tessellation factor functions, the packed 8-bit types and their `pack_` and
`unpack_` functions, and the legacy `tex2D` family. The methods of
work-graph nodes and `HitObject` come with the shader models that bring them.

## The compiler's own form

A checked shader is lowered into the compiler's own form
(`src/shader/shader_form.h`), from which every backend's output is written:
HLSL for D3D12 and D3D11, SPIR-V, GLSL and Metal, and what the CPU runs for
the software renderer. The form follows Naga's and Tint's, which serve the
same backends:

- **Structured statements.** A function's body is a tree of blocks, `if`,
  `loop` and `switch`, never arbitrary jumps. HLSL, GLSL and Metal are written
  from it almost line for line, SPIR-V's structured control flow maps onto
  it, and it tells the CPU where lanes that diverged meet again. A `loop` has
  a continuing block, where `continue` goes and where a `do` loop tests its
  condition (`break if`); a `for` loop tests its condition at the top of its
  body. A `switch`'s cases each say whether they fall through.
- **Typed expressions, each computed once.** A function's expressions are an
  array; each has a type, and is a value or a pointer to a place (a local, a
  global, an argument passed `out`, or an element or member of one). An EMIT
  statement says where each is evaluated, and it may be used only after,
  inside the block that evaluated it.
- **Indices, not pointers.** Types, variables, functions, expressions and
  statements refer to each other by 32-bit indices, so a form is cheap to
  copy, to serialize for the shader cache, and to compare in tests.
- **HLSL's matrices.** A `float3x4` has 3 rows of 4 columns, and indexing it
  gives a row. Each backend converts to its own convention, so the HLSL that
  DXC compiles needs none.

Lowering writes out what HLSL does implicitly, so no backend handles it:

- every conversion, literals folded to their new type; a conversion that
  changes a shape (`(float2x2)v`, `S s = {1, 2, 3}`, `(S)0`) takes the value
  apart into its scalars, in HLSL's order, and builds the new one;
- `a += b`, `++` and `--` load, operate and store;
- a store through a swizzle (`v.xz = w`, `m._m01_m10 = w`) stores each
  component;
- `&&`, `||` and `?:` evaluate their second operand only when it decides,
  as HLSL 2021 does: as an operator or a select when it has no effect, by a
  branch when it calls something;
- an argument passed `out` or `inout` goes through a local, copied back after
  the call, and a default argument is passed where one was left out;
- a parameter passed by value becomes a local the function may change;
- a method becomes a function whose first parameter, `this`, points to its
  object, which it may change; a static method has none;
- a static variable, a function's included, becomes a global, and the form's
  initializer, a function each entry point calls first, stores the first
  value of each that has one, in the order of the source;
- a string literal, which only `printf` takes, is kept as written.

A shader's inputs are globals of their own spaces, each with its register
class, and the register and space `register(t3, space1)` gives it:

- a `cbuffer`, a `tbuffer` and a `ConstantBuffer<S>` are each a global in
  the uniform space, read-only, whose type is the struct of their members;
  a uniform declared alone joins `$Globals`, a constant buffer of its own, as
  in DXC;
- a texture, a buffer, a sampler or an acceleration structure, or an array of
  them, sized or not, is a global in the handle space, which a load gives
  the resource's handle from;
- `t[p]`, `b[i]` and `t.mips[m][p]` are a texel: a pointer, from a handle and
  a location (and a level or a sample), to what the resource holds there,
  which loads, stores and atomics take; a store to a resource that is not
  written is an error of the form;
- a method of a resource is an expression of the handle when it has no
  effect, and a call otherwise; a `RayQuery` is a local its methods change,
  so each of them is a call given a pointer to it.

`tb_shader_form_verify()` checks every rule of the form: indices in range,
operand types that agree, expressions evaluated before they are used,
`break` and `continue` where they belong. Every form lowering makes in the
tests is verified, and so will be every pass's output in debug builds.
`tb_shader_form_print()` writes a form as text, one statement to a line,
which the tests compare against.

Entry points are functions the form lists with their stage: each function
with `[shader("pixel")]`, and, later, the one a compile names. A function
keeps its attributes as the source writes them (`[numthreads(8, 8, 1)]`,
`[domain("tri")]`, `[patchconstantfunc("constants")]`), each argument a
literal or a name. Its parameters, its result and the members of structs keep
their semantics (`: SV_Position`, `: TEXCOORD0`) and the modifiers that say
how a value passes between stages (`nointerpolation`, `centroid`, a geometry
shader's `triangle`, a mesh shader's `vertices`). A geometry shader's stream
is a handle its methods change, given as a pointer; a tessellation shader's
patch is a handle indexed as a buffer is.

**Not yet.** Arrays of constant buffers, and a base struct's method called on
a derived struct, are not lowered yet. An attribute's argument must be a
literal or a name, not an expression such as `4 * 2`. Next, each entry point
takes only what it reaches, which reflection describes. Then each entry point takes only what it reaches, which
reflection describes.

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
