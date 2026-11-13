Development release of Tenacious Badger, a dependency-free C17 engine
foundation and its Burrow showcase executable. This project is under construction:
the demo prints `2 + 3 = 5` and exits; there is no renderer or playable game yet.

Windows builds now automatically create/reuse a local signing identity and sign
project-owned executables and DLLs after linking. An existing certificate can be
selected by thumbprint, and signing can be disabled explicitly. CI verifies
signatures, rejects tampering, and checks both controls.

The engine now provides developer assertions in `badger/assert.h`: `TB_ASSERT`
and `TB_ASSERT_MSG` for invariants that are checked in Debug and compiled out of
Release, `TB_STATIC_ASSERT` for build-time checks, and a replaceable failure
handler.

Memory allocation goes through `badger/memory.h`. `tb_malloc`, `tb_calloc`,
`tb_realloc`, and `tb_aligned_alloc` never return NULL: running out of memory
ends the process through a replaceable handler. The `tb_try_*` forms return NULL
for callers that can recover. An application can supply its own allocator with
`tb_memory_set_functions`.

Included foundations:

- `badger` dynamic library and `badger_static` static library with the same
  documented, overflow-checked C17 module API.
- Separate `burrow` consumer and an in-house test harness.
- Native Windows performance-counter adapter and explicit Linux/macOS fatal stubs.
- Debug/Release CI, formatting, and standalone sanitizer tests on all three OSes.
- Native desktop exports with icons, metadata, relative library paths, and
  extraction-and-launch checks in CI.

Downloads:

- **Windows x86_64:** extract the ZIP and run `burrow.exe` in a terminal.
  Windows 10 or newer; the standard C runtime is statically linked. The executable
  is Authenticode-signed with a self-created local code-signing certificate.
  The public certificate and signing metadata are included; no public trust is implied.
- **macOS Apple silicon:** extract the ZIP and run
  `Burrow.app/Contents/MacOS/burrow` in Terminal to see the output. Requires macOS
  13 or newer. The app is ad-hoc signed, without Developer ID signing or Apple
  notarization; macOS may require explicit approval for a downloaded app.
- **Linux x86_64:** extract the tarball and run `./burrow`. Built on Ubuntu 26.04;
  the archive's `BUILD-INFO.json` records the libc baseline. Desktop entry,
  AppStream metadata, and icons are included for optional desktop integration.

Six SDK downloads provide Debug and Release libraries for all three desktop
platforms, including dynamic/static binaries, public headers, CMake package
metadata, and available debug symbols. Each configuration passes Python ctypes
invocation, external static linking, and FetchContent consumption checks. See
[consumption instructions](https://github.com/bluesentinelsec/tenacious-badger/blob/v0.1.0/docs/consuming.md).

Archives include Apache-2.0 licensing, personal attribution to `bluesentinelsec`,
launch instructions, and build metadata. Verify downloads using `SHA256SUMS`.
Browser, Android, and iOS exports are planned for later milestones.
