# Streams

`badger/stream.h` is one interface for a source or a sink of bytes. The
codecs, archives, packages, and virtual file system that follow it all read
and write through it, so they can be stacked: a package file, a range of it
that is one entry, that entry decompressed. A stream is a value: a pointer to
a constant table of functions, and a pointer to the state they work on. That
is the shape of an [allocator](memory.md#choosing-the-allocator-of-one-object),
and a new kind of stream is added the same way, by filling in a table, without
changing the interface.

## The operations

| Operation | Meaning |
| --- | --- |
| `tb_stream_read` | Up to a number of bytes at the position. Zero bytes, with success, is the end. |
| `tb_stream_write` | Up to a number of bytes at the position; at least one, or a failure. |
| `tb_stream_seek`, `tb_stream_tell` | Move or report the position, from 0 to `INT64_MAX`. |
| `tb_stream_size` | The size in bytes. |
| `tb_stream_read_at` | Up to a number of bytes at an offset, without moving the position. |
| `tb_stream_flush` | Pass written bytes on, to storage or to the stream below a codec. |
| `tb_stream_close` | Release the stream and what it owns, once. |

Each member of the table except `release` may be NULL. A stream offers what it
can do, and asking it for anything else returns `TB_STREAM_UNSUPPORTED`: a
memory stream cannot be written, and a decompressor will not seek. A flush of
a stream that keeps nothing back succeeds.

Transfers can be short, as for files. The helpers loop for a caller that wants
the whole length:

- `tb_stream_read_exact` and `tb_stream_read_exact_at` read exactly a length,
  and return `TB_STREAM_END` when the stream ends first.
- `tb_stream_write_all` writes all of a buffer.
- `tb_stream_copy` copies until the source ends, through a buffer of the
  caller's, and stops with `TB_STREAM_TOO_LARGE` after a limit. A limit is how
  a caller stops a stream built to expand without end, such as a hostile
  compressed entry.

The operations check what an implementation returns as well as what the caller
passes: a stream that reports more bytes than it was asked for, or a write
that reports success and transfers nothing, fails an assertion where it does
so, not later in the caller.

## The streams of this header

| Opened by | Reads | Writes | Seeks, size | Reads at an offset |
| --- | --- | --- | --- | --- |
| `tb_stream_open_memory` | borrowed bytes | no | yes | yes, thread-safe |
| `tb_stream_open_buffer` | what was written | grows; gaps are zeros | yes | yes |
| `tb_stream_open_file` | with `TB_FILE_READ` | with `TB_FILE_WRITE` | yes | with `TB_FILE_READ`, thread-safe |
| `tb_stream_open_range` | a window of its parent | no | yes | as thread-safe as its parent |

Each one allocates its state from the allocator it is given, or the engine's
heap, and `tb_stream_close` releases it. Allocating the state fails fast, as
allocation does. A buffer stream's bytes are data, not state: a write that
needs more memory than there is returns `TB_STREAM_NO_MEMORY` and writes
nothing. `tb_stream_buffer_bytes` borrows them until the next write or close.

A file stream owns its handle and keeps a position of its own. It reads and
writes with `tb_file_read_at` and `tb_file_write_at` ([files](files.md)), so
the handle's cursor plays no part: reads at an offset do not disturb the
position, and many threads can read one file stream at once. Its `open`
returns the `tb_fs_result` of opening the file, so a caller can tell a missing
file from a refused one; its operations afterwards return stream results.

A range borrows its parent, which must stay open until the range is closed,
and reads only through the parent's reads at an offset. It needs no seek of
the parent, so any number of ranges can share one parent: one per entry of
an archive, each with its own position. Its window is not checked against the
parent's size; where the parent ends first, so does the range.

## Results

`tb_stream_result` has the shared names of [the conventions](conventions.md)
and four of its own:

| Result | Named | Meaning |
| --- | --- | --- |
| `TB_STREAM_UNSUPPORTED` | `"not supported"` | The stream does not offer the operation. |
| `TB_STREAM_END` | `"unexpected end"` | The stream ended before the length that was required. |
| `TB_STREAM_NO_SPACE` | `"no space left"` | The sink is full. |
| `TB_STREAM_MALFORMED` | `"malformed data"` | The bytes are not a valid encoding. No stream of this header returns it; it is there so codecs report bad input the same way. |

`TB_STREAM_ACCESS` passes on a refusal from the system. Other failures of a
file pass on as `TB_STREAM_OS_ERROR`.

## Threading

A stream is externally synchronized: the caller serializes every call that
names it. Reads at an offset from the memory, file, and range streams of this
header are the exception: any number run together, as long as nothing writes,
seeks, or closes the stream meanwhile. That is what lets loading jobs share
one open package. Another implementation states whether its reads at an offset
are thread-safe. `tests/core/stream_test.c` reads every built-in kind from
eight threads at once, and ThreadSanitizer checks the claim on Linux and macOS.
