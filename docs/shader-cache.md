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
`dxcompiler 1.8 (4792, 23992152) dxil 1.8`. A new DXC therefore compiles again
rather than reuse what an old one wrote. Each key hashes a tag of its own
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
`Engine/Shaders/D3D12.tbsl`, loaded through the VFS
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

## Threading

Every function of `badger/shader_cache.h` is thread-safe. An open library is
immutable, and every function that reads one is thread-safe; a writer is
externally synchronized.
