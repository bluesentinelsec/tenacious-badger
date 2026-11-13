# Contributing to Tenacious Badger

Read the binding vision and engineering standards in [AGENTS.md](AGENTS.md)
before changing code. Use C17, standard C, and native platform APIs. Do not add third-party
packages or download external dependencies.

Work on a feature branch and submit a focused pull request. Preserve existing
work and shared history. Prefer squash merges into `main`. Commit messages
should explain the problem and resulting behavior.

Keep reusable behavior in the `badger` library, and keep `burrow` thin. List
translation units explicitly in component CMake files. Every new platform
interface needs a Windows adapter and compiling Linux/macOS implementations or
fatal stubs. Document public APIs with Doxygen, including their threading,
ownership, valid-input, and failure contracts.

Before review:

- Run `make fmt` and `make check-format`.
- Run `make test`, `make release`, and `make sanitizer` where supported.
- Add focused regression tests for changed behavior, including edge cases.
- Fix warnings; ask the author before suppressing any warning.
- Update interface and platform documentation when contracts change.

See [README.md](README.md) for equivalent CMake and Windows commands. Change only
`VERSION` when updating the application version, and follow semantic versioning.
The project remains in initial development until the author approves 1.0.0.

Follow [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Report security issues privately
as described in [SECURITY.md](SECURITY.md). Contributions use the repository's
[license](LICENSE).
