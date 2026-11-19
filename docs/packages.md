# Packages

A package is the file a game's cooked assets ship in: compressed, and
optionally obfuscated, so they are not read by casual extraction tools.
`badger/package.h` writes and reads them. ZIP and TAR (see
[archives](archives.md)) are for exchange with other tools; a package is for
shipping.

- **Compression is per entry.** Each entry is deflated on its own, or stored
  when deflating would not make it smaller.
- **Lookup is fast.** Entries are found by name in O(log n), from an index
  read once when the package opens.
- **Reads run in parallel.** Any entry can be read while others are, from as
  many threads as the source allows (files and memory allow any number).
- **The package is untrusted.** Opening it checks the whole index first:
  - every entry lies between the header and the index, and no two share a byte;
  - names are relative paths, in strict order, so each is unique;
  - an entry claims no more than its codec can produce;
  - the index has a CRC-32.

  An entry's stream then ends at its size and checks its CRC-32 there.

```c
tb_package *package = NULL;
uint8_t key[32];
game_package_key(key); /* From tb_add_package_key: see "The key" below. */
if (tb_package_open(&file, key, NULL, NULL, &package) == TB_PACKAGE_OK)
{
    size_t index = 0;
    if (tb_package_find(package, "textures/rock.tex", 17, &index))
    {
        tb_stream entry = {0};
        tb_package_open_entry(package, index, NULL, &entry);
        /* ... read it ... */
        tb_stream_close(&entry);
    }
    tb_package_close(package);
}
```

Tools write packages with `tb_package_writer_create`, `tb_package_add` (one
entry, whole, at a level from 0 to 9), and `tb_package_writer_finish`.

## Obfuscation

A package written with a key is obfuscated whole, both its index and every
entry, with ChaCha20 (see [codecs](codecs.md#chacha20)). Byte p of the
package meets byte p of the keystream, so any entry is restored from its own
offset, at the same cost as the first.

- **A key per package.** Each package draws a random 16-byte salt
  (`tb_random_bytes`). Its own key is HMAC-SHA256 of the game's key, a label,
  and the salt, so no two packages share a keystream, and the nonce can be
  fixed.
- **A wrong key is reported as one.** The header holds the first 8 bytes of
  HMAC-SHA256 of a label under the package's key. A wrong key is
  `TB_PACKAGE_WRONG_KEY`, not a damaged package. No key at all is
  `TB_PACKAGE_KEY_REQUIRED`.
- **What can still be seen:** the header and the footer, which say how to read
  the rest; the count of entries; and the sizes of the gaps between them.
  Names and data cannot be seen.

## Layout

Version 1, little-endian:

| Bytes | Field |
| --- | --- |
| 0 | **Header**, 64 bytes: magic `TBPK

` (8), version (2), flags (2: bit 0, obfuscated), reserved (4), salt (16), key check (8), reserved (24). Never obfuscated. |
| 64 | **Entries' data**, each from an offset that is a multiple of 16, with zeros between. |
| index offset | **Index**: one record per entry in the order of names, each 32 bytes and then its name: name length (2), codec (1: 0 stored, 1 deflated), reserved (1), CRC-32 (4), offset (8), stored size (8), size (8). |
| end − 48 | **Footer**, 48 bytes: index offset (8), index size (8), entry count (8), index CRC-32 (4), reserved (12), magic `TBPKEND
` (8). Never obfuscated. |

- **Newer packages are refused.** A reader refuses any version but its own,
  and unknown flags, as `TB_PACKAGE_UNSUPPORTED`, so a newer package is never
  misread.
- **Writing never seeks.** The index and footer come last, so a writer needs
  only a stream it can write to.
- **Entries are not split into blocks.** An entry's stream decompresses it from
  its start. A later version may split large entries that are read in pieces
  into blocks that decompress on their own.

`tb_bench package` times reading 1,000 entries of 4 KiB, plain and
obfuscated.

**Obfuscation is not security.** The key ships inside the game, and anyone who
runs the game under a debugger can recover it. Obfuscation deters casual
extraction, and no more.

## The key

A game's packages are obfuscated with one 256-bit key. The key is compiled into
the game, hidden, by a CMake function that needs nothing but CMake:

```cmake
# A key made once and kept in a file, which can stay out of version control:
tb_add_package_key(game FUNCTION game_package_key
    KEY_FILE "${CMAKE_SOURCE_DIR}/secrets/package.key")

# Or a key made from a password, given in a file:
tb_add_package_key(game FUNCTION game_package_key
    PASSWORD_FILE "${CMAKE_SOURCE_DIR}/secrets/package.password")
```

Either form adds to the target a C function, declared in `<FUNCTION>.h`, that
writes the key into 32 bytes when it is called:

```c
#include "game_package_key.h"

uint8_t key[32];
game_package_key(key);
```

The tools that build the packages are given the same key file or password, so
the game and its packages agree.

- **A key file** holds the key as 64 hexadecimal digits. If the file does not
  exist, a key is made and written there, and every later configure reuses it.
  Anyone with the file can read the packages: keep it where it lasts, such as
  a secret store for release builds, and commit it only if the project means
  to share it.
  - **Linux and macOS:** a new key comes from `/dev/urandom`.
  - **Windows:** CMake has no such source. The key is the SHA-256 of CMake's own
    random string, whose seed holds 32 bits from the system, and the time to
    the microsecond. That is ample for obfuscation, which a debugger defeats
    regardless. A project that wants more can make its key file elsewhere.
- **A password file's** first line is the password. The key is the SHA-256 of a
  fixed label and the password, so the password itself is never compiled in.

### How the key is hidden

The key is never stored whole in the executable, nor as text:

- **Three parts.** It is split into three parts whose XOR is the key. Each part
  is 32 bytes in its own array, in a shuffled order, and the function puts them
  together when it runs.
- **No folding.** The parts are read through a volatile pointer, so no compiler
  folds them back into the key.
- **Reproducible.** The parts and their order are derived from the key, so the
  same key makes the same source and builds stay reproducible.

This defeats `strings` and any search of the executable for the key's bytes.
The tests check that: they build a program with a key from a file and one from
a password, and require that both come back whole, and that no 8 bytes of
either appear anywhere in the program. They also check that keys are made,
kept, and refused when malformed.

The generated source holds the key, hidden, and lives in the build directory.
Do not commit it.
