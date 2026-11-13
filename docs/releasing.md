# Native desktop releases

`VERSION` is the only version definition. Work through a feature branch and pull
request. Before publishing, require CI and sanitizer checks to pass on the final
commit, then merge the pull request and tag that commit as `v` plus `VERSION`.
Push the annotated tag using the repository's SSH remote. The release workflow
checks the version, reruns all CI and sanitizers on the tag, verifies all nine
exports, and publishes them with release notes and `SHA256SUMS`.

CI produces Windows x86_64 ZIP, macOS Apple silicon ZIP containing `Burrow.app`,
and Linux x86_64 tar.gz exports. It also publishes six SDK archives: Debug and
Release for each desktop platform, with dynamic/static libraries, headers, and
relocatable CMake metadata. Debug SDKs include available symbols. See
[library consumption](consuming.md) for integration details. Each archive has native product metadata and
icons, its engine library, license/notice, launch instructions, and build metadata.
The Windows runtime is statically linked; macOS uses a bundled Frameworks
directory; Linux uses `$ORIGIN/lib`. Exports contain no third-party packages,
SDKs, tests, or sanitizer runtimes. macOS minimum deployment is 13; Linux records
the build's libc baseline. See [release notes](release-notes.md) for current scope
and signing limitations.

To generate and verify an export locally with Python 3.12+ already installed:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
python3 tools/package_desktop.py --build build/release --output dist
```

The script stages the native files, validates metadata, launches the demo, creates
an archive, extracts it to a separate directory, and repeats validation and launch
without development library-path overrides. Windows also checks PE imports for
unwanted runtime dependencies; macOS verifies the bundle's ad-hoc signature.
Linux checks bundled library discovery and desktop metadata. Python's standard
library and tools supplied by the native SDK/OS perform packaging; no packages
are installed. Icons are original geometric assets generated reproducibly by
`python3 tools/make_icons.py`, without image libraries.

Use `gh config set git_protocol ssh --host github.com` and an SSH origin for Git
operations. GitHub API operations (such as creating releases) use `gh`'s normal
account or workflow token; GitHub does not expose those APIs over SSH.

The SDK checks additionally install the libraries, call the public ABI through
Python ctypes, compile/link/run an external static consumer, and build/run a
local FetchContent consumer. Both configurations must pass before publication.
