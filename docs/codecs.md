# Codecs

The `codec` module turns bytes into other bytes: checksums and decompression
now, compression and encodings as they arrive. Its code lives in `src/codec/`,
and each part has its own public header. Everything is the engine's own C; no
codec library is used.

## Checksums

`badger/checksum.h` computes the two checksums that the engine's formats need:

| Function | Used by | Notes |
| --- | --- | --- |
| `tb_crc32`, `tb_crc32_update` | ZIP, gzip, PNG | The reflected polynomial 0xEDB88320, inverted before and after |
| `tb_adler32`, `tb_adler32_update` | zlib | RFC 1950: two sums modulo 65521 |

Each has a one-call form, and an incremental form that starts from
`TB_CRC32_INITIAL` or `TB_ADLER32_INITIAL` and is given the previous result
with each piece. Any split of the input gives the same result as one call.
Both are thread-safe and allocate nothing.

They detect accidental damage, such as a truncated download or a flipped bit.
They do not resist tampering: whoever changes the data can recompute them.

CRC-32 combines eight bytes per step with eight tables of 256 entries
("slicing-by-8"). The tables are constants in `src/codec/crc32_table.h`,
written by `tools/generate_crc32_table.py` and committed, so the engine needs
no initialization and the build needs no Python. A test recomputes every
entry from the polynomial. Adler-32 defers the modulus for runs of 5552 bytes,
the longest that cannot overflow 32 bits.

`tb_bench checksum` compares CRC-32 with the classic loop of one table lookup
per byte.

## Decompression

`badger/deflate.h` decompresses DEFLATE (RFC 1951), the compression of ZIP,
gzip, PNG, and zlib, in any of three framings:

| Format | Framing | Checked |
| --- | --- | --- |
| `TB_DEFLATE_RAW` | None, as in ZIP entries | Nothing beyond the data |
| `TB_DEFLATE_ZLIB` | A two-byte header, as in PNG (RFC 1950) | Header check bits, Adler-32 |
| `TB_DEFLATE_GZIP` | A gzip member (RFC 1952) | Header, its optional CRC-16, CRC-32, and length |

There are three ways in:

- **`tb_inflate`** decompresses one whole stream into a buffer in one call.
  The capacity of the buffer is the most the caller accepts: more is
  `TB_DEFLATE_TOO_LARGE`. Bytes after the end of the stream are
  `TB_DEFLATE_MALFORMED`, and an input that ends early is
  `TB_DEFLATE_TRUNCATED`.
- **An inflater** (`tb_inflater_create`, `tb_inflater_run`) is fed input and
  room for output in ranges of any size. It stops when it needs more of
  either, and continues where it left off, down to a byte at a time.
- **A decompressing stream** (`tb_inflate_open_stream`) reads another
  [stream](streams.md) and decompresses it as it is read. In gzip format it
  reads every member of a file one after another, as `gzip -d` does. Copy it
  into a buffer stream with `tb_stream_copy` and a limit to decompress data of
  unknown size safely.

### Untrusted input

Archives, mods, and downloads reach the decoder, so it treats every stream as
hostile:

- **Memory is fixed when an inflater is created**: a window of 32 KiB and its
  code tables, about 40 KiB in all. Decoding allocates nothing.
- **The output is bounded by the caller**, never by the stream: a stream of
  120 bytes that expands to 100,000 stops at the room or the limit given.
- **Every code is checked** before it is used: over-subscribed and incomplete
  Huffman codes (except the single one-bit code that RFC 1951 allows), a
  block with no end, repeats past the end of the code lengths, symbols with no
  meaning, and distances before the start of the output.
- **A failure is kept.** Every later call returns it until the inflater is
  reset, so a caller cannot read on past damage by accident.
- **The input is consumed exactly**: no byte past the end of the stream is
  taken, so the next gzip member or archive entry is left for the caller.

A zlib stream with a preset dictionary is valid, but this decoder has no way
to be given the dictionary, and reports `TB_DEFLATE_UNSUPPORTED`. The window
is always 32 KiB, whatever smaller window a zlib header declares.

### How it decodes

The decoder is a state machine whose every state can stop for input or room.
Bits are pulled from the input one byte at a time, and only when the next step
needs them. Huffman codes are decoded through a table of the first nine bits,
which holds every code that short; longer codes are decoded a bit at a time
from the counts of each length, as in Mark Adler's `puff.c`.

`tests/codec/deflate_vectors.h` holds streams that zlib wrote, at levels 0 to 9
and with each of its strategies, in all three framings. It is made by
`tools/generate_deflate_vectors.py` at development time; the tests make the
original data again and compare. The tests also decode every truncation of
every vector and every single flipped bit of the small ones, under the
sanitizers, and streams built bit by bit that break each rule above.

`tb_bench inflate` times the decoder on those streams.
