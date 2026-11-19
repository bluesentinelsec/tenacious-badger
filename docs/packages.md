# Packages

A package is the file a game's cooked assets ship in: compressed, and
optionally obfuscated, so they are not read by casual extraction tools. The
format and its reader and writer are planned in
[issue #153](https://github.com/bluesentinelsec/tenacious-badger/issues/153).
This page starts with the key.

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
