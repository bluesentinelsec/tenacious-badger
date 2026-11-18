# Codecs

The `codec` module turns bytes into other bytes: checksums now, compression
and encodings as they arrive. Its code lives in `src/codec/`, and each part
has its own public header. Everything is the engine's own C; no codec library
is used.

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
