# The shader cache and shader libraries

A shader compiled ahead of time and the same shader compiled at run time give
the same bytes, and share one cache, as Unreal's ShaderCompileWorker and its
editor share the derived data cache. `badger/shader_cache.h` names what a
compile writes by a key, and keeps compiled shaders on disk between runs.
`badger/shader_library.h` holds the compiled shaders a game ships, one library
for each backend, found by the same keys. See [Shaders](shaders.md) for the
compiler itself.

```c
tb_shader_key shader = {0};
if (tb_shader_key_make(&request, &shader) == TB_SHADER_OK)
{
    const void *code = NULL;
    size_t size = 0;
    if (tb_shader_library_find(library, &shader, &code, &size))
    {
        /* Shipped with the game: no compiler needed. */
    }
    tb_shader_key entry = {0};
    tb_shader_cache_key(&shader, tb_dxc_version(dxc), &entry);
    if (tb_shader_cache_get(cache, &entry, arena, &code, &size) != TB_SHADER_CACHE_OK)
    {
        /* A miss, or a damaged entry: compile, then keep what was compiled. */
        if (tb_shader_compile(&request, arena, &compiled) == TB_SHADER_OK)
        {
            (void)tb_shader_cache_put(cache, &entry, compiled.code, compiled.code_size);
        }
    }
}
```

## Keys

Every key is a SHA-256 digest (`badger/sha256.h`) of 32 bytes. There are two.

**A shader key** (`tb_shader_key_make`) names what a compile is asked to do,
whatever compiler does it. It hashes:

| Input | Why |
| --- | --- |
| The source, preprocessed with `#line` markers | A change to the main file or any file it includes changes what is compiled; so does moving code to another line, which DXC's debug information and messages record |
| The defines, in order; one without a value as `1` | A define the source does not read still asks for another compile |
| The entry point and the stage | One source holds many shaders |
| The target and the shader model, its default applied | One shader compiles for many backends and models |
| The compile's flags | A debug compile writes other bytes |
| `TB_SHADER_COMPILER_REVISION` | A change to what the engine's compiler writes for the same source |

A shader key is made without a compiler: it preprocesses the source, as a
compile does first, and nothing else. A game finds a shader in its libraries by
it, on a machine with no compiler at all. A request that cannot be read has no
key: INVALID, or FAILED for a source with errors, which only a compile reports.

**A cache key** (`tb_shader_cache_key`) adds the compiler that did the work, as
it names itself: `tb_dxc_version` for DXC, such as
`dxcompiler 1.8 (4792, 23992152) dxil 1.8`, and `tb_d3dcompiler_version` for
D3DCompiler, such as `d3dcompiler_47 10.0.26100.1`. A new compiler therefore
compiles again rather than reuse what an old one wrote. Each key hashes a tag of its own
before its inputs, and each string after its length, so no key can be another
key, and no two different lists of inputs hash the same bytes.

**`TB_SHADER_COMPILER_REVISION`.** Raise it in a change that makes the compiler
write different bytes for the same sources, such as a change to the text the
compiler gives DXC. Libraries and caches written before the change are then no
longer found, and their shaders compile again.

## The cache

A cache is a directory with one file for each cache key, named by the key in
lower-case hexadecimal with the extension `.tbsc`. `tb_shader_cache_open`
makes the directory, and its parents, where they are missing.

- **A get** reads an entry and checks all of it: its header, its key, its size
  and its checksum. It gives the code, copied into the caller's arena at an
  alignment of 8, which a DXIL container's words need. A missing entry is a
  MISS; one that cannot be trusted, damaged, truncated, longer than it says,
  of another version, or another key's, is DAMAGED, which a caller treats as a
  miss. A cache never gives code it has not checked: the code goes to a GPU
  driver as it is.
- **A put** writes the entry whole beside its place, then renames it into
  place (`tb_fs_replace_file_cached`). A reader sees the old entry or the new
  one, never part of either; a put after a DAMAGED get replaces the damaged
  entry. A put that fails leaves the cache as it was, and the caller goes on:
  a cache that cannot be written is slower, not wrong.
- **Sharing.** Any number of threads may share a cache, and any number of
  programs its directory: the engine and `badger-shaderc`, or two editors.
  Two that put one key at once write the same bytes, and the last rename
  wins. On Windows a rename can lose to a reader that has the entry open; the
  put fails, and the entry stays as it was.
- **Size.** An entry holds 1 byte to `TB_SHADER_CACHE_MAX_CODE` (64 MiB) of
  code. Nothing is ever removed: delete the directory to empty a cache.

### An entry's format

Every number is little-endian.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | `TBSC` |
| 4 | 4 | The format's version: 1 |
| 8 | 32 | The cache key, which must be the file's name |
| 40 | 8 | Bytes of the code, which must be the rest of the file |
| 48 | 4 | The code's CRC-32 (`badger/checksum.h`) |
| 52 | 4 | Reserved: 0 |
| 56 | the size | The code |

The CRC-32 finds damage; it is not security. A cache directory is trusted as
the build directory is.

## Shader libraries

A library is the compiled shaders a game ships for one backend, such as
`Shaders/d3d12.tbsl`, loaded through the VFS
(`tb_shader_library_load`) or from memory (`tb_shader_library_open`). It is
found by shader keys, which do not name the compiler: the library records the
compiler that wrote it once, for all of its shaders
(`tb_shader_library_compiler`). A shader whose sources changed after its
library was written has another key, so it is not found, and the engine
compiles it where it may.

**Checked whole.** A library is checked when it is opened, every byte of it:
its header, its compiler's name, its table, each code's checksum, and the
padding between, which must be zero. A library that is damaged anywhere,
truncated or longer than it says is DAMAGED, and one of another version of the
format is VERSION; either is refused whole, never read in part.

**Writing one.** `tb_shader_library_writer_add` adds a key and its code; a key
added twice with the same code is kept once, since two entries of a manifest
may compile alike, and with other code is refused. `tb_shader_library_writer_finish`
writes the entries in the order of their keys, so the same shaders give the
same bytes in whatever order they were compiled.

### A library's format

Every number is little-endian. The library is:

1. a header of 32 bytes;
2. the name of the compiler, UTF-8 without a NUL, then zeros to a multiple of 8;
3. a table of one entry of 48 bytes for each shader, in ascending order of
   their keys, each key once;
4. each shader's code, in the table's order, each at the first multiple of 8
   after the one before, with zeros between; the file ends where the last code
   does.

The header:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | `TBSL` |
| 4 | 4 | The format's version: 1 |
| 8 | 4 | Entries of the table |
| 12 | 4 | Bytes of the compiler's name: at most `TB_SHADER_LIBRARY_MAX_COMPILER`, 1024 |
| 16 | 8 | Bytes of the file |
| 24 | 4 | The CRC-32 of the compiler's name, its padding and the table |
| 28 | 4 | Reserved: 0 |

An entry of the table:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 32 | The shader key |
| 32 | 8 | Where its code starts, from the start of the file |
| 40 | 4 | Bytes of its code: from 1 to `TB_SHADER_CACHE_MAX_CODE` |
| 44 | 4 | Its code's CRC-32 |

A library is at most `TB_SHADER_LIBRARY_MAX_SIZE`, 1 GiB. Its code stays
aligned to 8 bytes in memory, for as long as the library is open.

## Finding a shader, or compiling it

`tb_shader_compile_cached()` is what the renderer and `badger-shaderc` both
do with a compile request, so that both give the same bytes:

1. A request for no target (`TB_SHADER_TARGET_NONE`) writes no code, and is
   compiled for its reflection whatever else is asked.
2. The shader key is made. A source that cannot be read has none; it is
   compiled for its reflection, which says what is wrong.
3. The library, if one is given, is searched by the shader key.
4. If the lookup is library-only, as `shaders.runtime_compile` off asks, a
   shader the library lacks is `TB_SHADER_UNAVAILABLE`, and a diagnostic says
   so.
5. The cache, if one is given, is searched by the cache key, of the
   compiler the request's target names: DXC for DXIL, D3DCompiler for DXBC.
   A request without that compiler is `TB_SHADER_UNAVAILABLE`.
6. Otherwise the shader is compiled, and what is compiled is put in the cache.

A shader from a library or a cache has its code only; its reflection comes
with a compile. `tb_shader_origin` says where each shader came from.

## badger-shaderc

`badger-shaderc`, in `tools/shaderc/`, is our counterpart of Unreal's
ShaderCompileWorker. It reads a manifest, compiles every shader it lists for
each backend asked for, in parallel on the job system (`badger/jobs.h`),
through `tb_shader_compile_cached()` and the shader cache, and writes one
library for each backend, named after it: `d3d12.tbsl`, `d3d11.tbsl`.

```
badger-shaderc --root C:/game/shaders --backend d3d12 --output bin/Shaders shaders/Game/Shaders.manifest
```

| Option | Meaning |
| --- | --- |
| `MANIFEST` | The shaders to compile |
| `--root DIRECTORY` | An absolute directory whose files are shaders by their virtual paths: `DIRECTORY/Engine/Private/Sky.hlsl` is `/Engine/Private/Sky.hlsl`. May be given more than once; a later root's file wins, as a mod's does |
| `--backend NAME` | A backend to write a library for, as `graphics.renderer` names it: `d3d12` (DXIL, through DXC) or `d3d11` (DXBC, through D3DCompiler) so far. With none, nothing is compiled |
| `--output DIRECTORY` | Where the libraries are written; the current directory by default |
| `--cache DIRECTORY` | The shader cache; the user's by default (`tb_shader_cache_default_directory()`), which the engine shares |
| `--no-cache` | Compile without the cache |
| `--debug` | Compile for debugging, as `TB_SHADER_COMPILE_DEBUG` |
| `--quiet` | Say only what fails |

Each error names its file, line and column, as editors read them; a shader
that does not compile names its line of the manifest. Any error fails the
run, and no library is written. A backend fails the run where its compiler is
unavailable: DXC for `d3d12`, D3DCompiler for `d3d11`.

### Manifests

A manifest lists one shader on each line: its virtual path, its stage as
`[shader("...")]` names it, its entry point, and any defines, `NAME` or
`NAME=VALUE`, separated by spaces or tabs. A `#` begins a comment, and a line
with nothing else is ignored. A shader has at most
`TB_SHADER_MANIFEST_MAX_DEFINES`, 32, defines. `tb_shader_manifest_read()`
reads one.

```
# path                           stage     entry point  defines
/Engine/Private/Sky.hlsl         vertex    MainVS
/Engine/Private/Sky.hlsl         pixel     MainPS
/Engine/Private/Blur.hlsl        compute   MainCS       RADIUS=4 HORIZONTAL
```

### The engine's shaders in the build

The engine's shaders are the files under `shaders/` of its source tree.
`cmake/EmbedShaders.cmake` builds them into the engine library, which the
renderer reads them from (`tb_shader_engine_loader()`), and
`tools/shaderc/CMakeLists.txt` runs `badger-shaderc` on them as part of the
build, with the manifest `shaders/Engine/Shaders.manifest`, into
`bin/Shaders/`, beside the executables. Both read the same files, so the
library holds what the renderer would compile.

**To add a shader to the library**, add a line to
`shaders/Engine/Shaders.manifest` for each entry point; the next build
compiles it. A change to a shader file, or to the manifest, compiles the
library again; shaders that did not change come from the cache.

| Platform | Libraries |
| --- | --- |
| Windows | `d3d12.tbsl`, through DXC: the one in the Windows SDK, or beside `badger-shaderc`; and `d3d11.tbsl`, through D3DCompiler, part of Windows |
| macOS | `metal.tbsl`, cooked from HLSL into MSL and copied to `Burrow.app/Contents/Resources/Shaders/`. Metal compiles each MSL stage on the device. |
| Linux | None yet: no backend there needs compiled shaders, and the build makes no library |

`TB_BUILD_SHADERS` (on when the engine is built on its own, off when a game
builds it in its tree) builds `badger-shaderc` and the libraries.

A test on Windows compiles every shader of the manifest at run time and
compares it, byte for byte, with the library the build wrote
(`tests/shader/shader_library_d3d12_test.c`), and makes a renderer that draws
with the library alone.

## The setting

`shaders.runtime_compile` says whether a game may compile at run time the
shaders its libraries lack: on in developer builds, off in release builds. A
game passes it to its renderer, negated, as `shader_library_only`. Off, a
shipped game draws with its libraries' shaders alone, as Unreal's packaged
games do, and a shader they lack is an error that names it, rather than a
compile on a player's machine.

## Threading

Every function of `badger/shader_cache.h` is thread-safe. An open library is
immutable, and every function that reads one is thread-safe; a writer is
externally synchronized.
