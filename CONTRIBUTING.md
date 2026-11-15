# Contributing to Tenacious Badger

Read the binding vision and engineering standards in [AGENTS.md](AGENTS.md)
before changing code. Use C17, standard C, and native platform APIs. Do not add third-party
packages or download external dependencies.

Work on a feature branch and submit a focused pull request. Preserve existing
work and shared history. Prefer squash merges into `main`. Commit messages
should explain the problem and resulting behavior. Link each pull request to its
issue with a closing keyword (`Closes #12`) so the two stay in sync.

Keep reusable behavior in the `badger` library, and keep `burrow` thin. List
translation units explicitly in component CMake files; build inputs are never
found by globbing. Formatting and the source checks find C sources and headers
by themselves in `benchmarks`, `cmake`, `include`, `src`, and `tests`.
Every new platform interface needs a Windows adapter and compiling Linux/macOS
implementations or fatal stubs. Document public APIs with Doxygen, including
their threading, ownership, valid-input, and failure contracts.

Before review:

- Run `make fmt` and `make check-format`.
- Run `make test`, `make release`, and `make sanitizer` where supported.
- Add focused regression tests for changed behavior, including edge cases. Write
  them with the in-house framework described in [docs/testing.md](docs/testing.md).
- Use the foundation layer wherever it has an equivalent of a C library
  function: allocate through `badger/memory.h`, format and print through
  `badger/fmt.h`, handle C strings through `badger/str.h`, and assert with
  `TB_ASSERT`. [docs/development.md](docs/development.md#the-foundation-check)
  lists what replaces what. `python tools/check_foundation.py` must pass; it
  also runs with the tests and as a CI job.
- Fix warnings; ask the author before suppressing any warning.
- Update interface and platform documentation when contracts change.

See [README.md](README.md) for equivalent CMake and Windows commands. Change only
`VERSION` when updating the application version, and follow semantic versioning.
The project remains in initial development until the author approves 1.0.0.

Follow [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Report security issues privately
as described in [SECURITY.md](SECURITY.md). Contributions use the repository's
[license](LICENSE).
