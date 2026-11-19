# Codecs

The `codec` module turns bytes into other bytes: checksums, compression, and
encodings. Its code lives in `src/codec/`, and each part
has its own public header. Everything is the engine's own C; no
codec library is used.

## Base64

`badger/base64.h` writes bytes as Base64 text and reads them back (RFC 4648),
as JSON, configuration files, and glTF data URIs carry binary data. Flags
choose the form; zero is the standard, padded one:

| Flag | Effect |
| --- | --- |
| `TB_BASE64_URL_SAFE` | `-` and `_` in place of `+` and `/` |
| `TB_BASE64_NO_PADDING` | No `=`: encoding writes none, decoding accepts none |
| `TB_BASE64_SKIP_WHITESPACE` | Decoding skips space, tab, and line breaks anywhere |

`tb_base64_encode` and `tb_base64_decode` work in buffers of the caller's;
`tb_base64_encoded_size` gives the exact length of the text, and
`tb_base64_decoded_size_bound` a capacity always enough to decode. The `_alloc`
forms return memory of their own, with a NUL after it.

**Decoding is strict**, because the text is untrusted: a character outside the
alphabet (including the other alphabet's two), padding where it does not
belong or of the wrong amount, a last group of one character, and non-zero
bits after the last byte are all `TB_BASE64_INVALID`, with the offset of the
first offending character. Every byte string thus has exactly one text that
decodes to it in each form.

**Nothing is written on failure.** Decoding checks and measures all of the
text before writing a byte. A buffer too small is `TB_BASE64_BUFFER_TOO_SMALL`
with the size it needs, so a call with no room measures.

The tests check the vectors of RFC 4648 and encodings made by Python's
`base64` module at development time (`tools/generate_base64_vectors.py`).
Hexadecimal and streaming forms are not here yet; nothing needs them.

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

## SHA-256

`badger/sha256.h` computes SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104),
in one call or in pieces of any size. The engine uses them:

- **SHA-256** turns a password into a key, and names content, as a cache of
  cooked assets does.
- **HMAC-SHA256** derives one key from another and a salt, as the package
  format does for each package.

Unlike a checksum, SHA-256 resists tampering: no one knows how to find two
inputs with the same digest.

- **Secrets are cleared.** A finished state, and HMAC's padded keys, are
  cleared through a volatile pointer, which the compiler may not remove.
- **The tests** compare every input length from 0 to 300 bytes, which crosses
  every place padding can fall, with Python's `hashlib`. They also run the
  examples of FIPS 180-2, including a million `a`.
- **HMAC** is checked against the seven cases of RFC 4231, and against keys of
  every length from 0 to 130 bytes, around the 64-byte block, compared with
  Python's `hmac`.
- **Pieces:** every split of the input, and pieces of every size, give the same
  digest.

`tb_bench sha256` times a hash of 1 MiB and one HMAC.

## ChaCha20

`badger/chacha20.h` is the ChaCha20 stream cipher of RFC 8439, a 256-bit key
and a 96-bit nonce made into a keystream that is XORed with the data. The same
call obfuscates and restores. The engine uses it to obfuscate packages.

- **Every position costs the same.** Each 64-byte block of keystream is made
  from its own index, so `tb_chacha20_xor` starts at any byte offset, and many
  threads can restore different parts of one package at once.
- **A stream applies it as bytes pass.** `tb_chacha20_open_stream` wraps
  another stream, and byte p of the inner stream always meets byte p of the
  keystream. Reads, reads at an offset, writes, and reads after a seek all line
  up by themselves.
  - It offers exactly what the inner stream offers, so a caller's check of
    what a stream can do stays true.
  - It starts at the inner stream's position.
  - Closing it clears its copy of the key.
- **The keystream** of one key and nonce is 256 GiB long. Positions past it
  are `TB_STREAM_TOO_LARGE`, and passing them to the function directly is a
  contract violation.
- **Never use a nonce twice under one key.** The XOR of two results would be
  the XOR of their inputs, so take a fresh nonce from `tb_random_bytes` each
  time.

**Obfuscation is not security.** ChaCha20 alone hides data but does not detect
changes to it. A key that ships with a game can be recovered by anyone who runs
the game under a debugger. Obfuscation deters casual extraction, and no more.

The tests check the block function and encryption against RFC 8439's vectors,
which `tools/generate_chacha20_vectors.py` reads from the RFC's own text. They
also check that every range of the data, combined from its own offset, matches
the whole.

`tb_bench chacha20` times 1 MiB in one call and through a stream.

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

## Compression

The same header compresses, in the same three framings, at a level from 0 to
9: 0 only stores, 1 is fastest, `TB_DEFLATE_DEFAULT_LEVEL` (6) balances size
and speed, and `TB_DEFLATE_BEST_LEVEL` (9) is smallest. The three ways in
mirror decompression:

- **`tb_deflate`** compresses bytes into one stream in one call.
  `tb_deflate_bound` gives a capacity that is always enough.
- **A deflater** (`tb_deflater_create`, `tb_deflater_run`) takes input and
  room in pieces of any size. `TB_DEFLATE_SYNC_FLUSH` writes out everything so
  far, ending on a byte with an empty stored block, so a reader can decode it
  now; `TB_DEFLATE_FINISH` ends the stream with its checksum.
- **A compressing stream** (`tb_deflate_open_stream`) compresses what is
  written to it into another stream. Its flush is a sync flush; closing it
  writes the end of the compressed stream, and reports a failure to do so.

**The same input gives the same bytes**, at the same level and in the same
framing, however it is split into calls. The matcher works only with a full
lookahead of 262 bytes, or at a flush; the window slides at a fixed position;
and blocks end at fixed points. A cooked asset therefore compresses to the
same package bytes on every machine and every run.

### How it compresses

Input gathers in a window of 64 KiB. A hash of the next three bytes chains
each position to earlier ones with the same hash, and the matcher follows the
chain for the longest match within the last 32 KiB. Levels follow zlib's table
of effort: how long a match must be to shorten the search, to end it, and to
stop looking one byte ahead, and how many links to follow. Levels 1 to 3 take
the first good match; levels 4 to 9 look one byte ahead for a longer one.

A block holds up to 16383 symbols and never spans a slide of the window, so
its bytes are still there to store. It is written in whichever of three forms
is smallest: stored, the fixed code, or codes built for it. Codes are built
with the in-place algorithm of Moffat and Katajainen and limited to 15 bits
(7 for the code-length code), and every code has at least two symbols, so any
decoder accepts it.

A deflater holds about 600 KiB: the window, its hash chains, a block of
symbols, and room for an encoded block.

The tests compress empty, tiny, random, repetitive, and text inputs, and sizes
on each side of the window and of a stored block, at every level and in every
framing, and decompress them again. During development every stream was also
decompressed by zlib itself (Python's `zlib` and `gzip` modules), and its sizes
compared with zlib's at each level.

`tb_bench deflate` times levels 1, 6, and 9 on text, random bytes, and zeros.
