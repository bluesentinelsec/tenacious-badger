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

## Windows self-signing

Windows builds enable `TB_WINDOWS_SELF_SIGN=ON` by default. During CMake configure,
`scripts/windows/Sign-Binary.ps1` finds or programmatically creates an RSA-3072,
SHA-256 code-signing certificate for `CN=bluesentinelsec` in `Cert:\CurrentUser\My`.
Its generated private key is non-exportable. There are no prompts, paid services,
network calls, administrator requirements, or changes to certificate trust stores.
The identity is reused for that Windows user across builds and checkouts. Different
users/machines create their own identities; ephemeral CI runners create their own
per-job identities.

CMake invokes the script after linking each project-owned EXE, DLL, or module
library. This includes the game, engine, tests, sanitizer binaries, and the two
external consumer test programs. Static archives, import libraries, headers, PDBs,
and toolchain-owned binaries are not signed. Signing failures stop the build.

The script writes public `self-signing.cer`, `signing.json`, and a thumbprint file
under `build/<configuration>/signing` (or the chosen binary directory). Game and
SDK archives include the public certificate and signing metadata. Packaging checks
signatures again after staging and extraction, accepting an untrusted chain only
when the embedded signer matches the explicitly supplied certificate. Unsigned,
modified, and unexpected-signer files fail verification. CI also tests certificate
reuse, explicit certificate selection, and signing opt-out.

To use an existing legitimate code-signing certificate, first make it and its
private key available in the current user's Personal certificate store. Then
configure with its thumbprint; its subject does not need to match our generated
certificate's subject:

```powershell
cmake -S . -B build/vs -G "Visual Studio 18 2026" -A x64 `
  -DTB_WINDOWS_SIGNING_THUMBPRINT=YOUR_CERTIFICATE_THUMBPRINT
cmake --build build/vs --config Release
```

To return to automatic local identity selection, clear that cache setting with
`-DTB_WINDOWS_SIGNING_THUMBPRINT=`. To opt out completely, configure with
`-DTB_WINDOWS_SELF_SIGN=OFF`. Opt-out builds can run normally; Windows release
packaging requires signed binaries and the public signing metadata.

For a single file, the same script can create/reuse the default identity and sign
it directly:

```powershell
powershell -NoProfile -File scripts/windows/Sign-Binary.ps1 `
  -FilePath build/vs/bin/burrow.exe
```

Self-signing is our ad-hoc distribution policy; it does not establish external
publisher trust. Certificate/private-key generation is local to the builder, and
only the public certificate ships. The certificate is valid for three years; an
expired selected identity fails with instructions to select a replacement. No
Microsoft Store packaging or submission is involved.
