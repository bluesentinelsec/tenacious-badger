# Native files

`badger/file.h` defines synchronous binary access, metadata, directory iteration,
temporary objects, and per-user locations. Windows uses native wide-character
APIs; Linux and macOS use POSIX file descriptors. All three run the same tests.
The standard-C default reports `platform.files`, the called function, and
`not implemented`, then terminates. No supported platform has a stub.

File and directory tokens start zeroed, have one owner, and must be closed.
Copying a token does not duplicate ownership. Individual handles are externally
synchronized; distinct handles may be used concurrently. Native buffers and
iterator state return `TB_FS_NO_MEMORY` on allocation/resource exhaustion. These
small native allocations sit beneath the core allocator and are not counted by
its debugging wrapper; standalone sanitizer tests cover their lifetimes.

Open modes are explicit. Create requires write access, exclusive requires
create, and truncate requires write. POSIX files use mode 0600 and directories
0700, subject to the process umask. Windows shares read, write, and delete so an
open reader can retain an old file across replacement. No implicit locks are
acquired. Read and write can complete short; always use the transferred count.
EOF is a successful read of zero bytes. Positions are 64-bit, nonnegative, and
at most INT64_MAX. Seeking beyond EOF does not allocate or extend a file.
Close consumes a handle even on error and must not be retried.

Paths are bounded UTF-8 without embedded NUL. POSIX byte names that are not
Unicode are rejected. Enumeration reports an invalid name and then advances,
so a caller can continue. A too-small output buffer retains the current entry;
measurement also retains it. No enumeration ordering or snapshot is promised.
Queries inspect the final symlink/reparse point itself; file opens follow links.
Removal never recurses. These operations are not a filesystem sandbox.

On Windows, ordinary paths are resolved once with `GetFullPathNameW`, then use
an extended-length prefix so paths beyond MAX_PATH work without changing the
machine registry. Existing extended drive and UNC prefixes are accepted. Device
names (`NUL`, `CON`, `COM1`, etc.), alternate streams, wildcard characters, and
ambiguous trailing dots/spaces are rejected. Drive-relative paths follow native
Windows resolution. The process working directory must remain stable during
relative operations; prefer absolute names across threads.

Errors map to a small engine enum. `NOT_FOUND` means absent; an access failure
must not be treated as absence. Native systems sometimes classify malformed
parent paths differently (`NOT_DIRECTORY` versus `NOT_FOUND`). Other native
failures map to `IO`. File flush requests the platform's storage guarantee;
rename within a filesystem provides replacement visibility, not a guarantee of
survival after power loss. Cross-filesystem moves fail instead of copying.

Known locations return bases without creating them. Windows uses Known Folders
(roaming application data for data/configuration, local application data for
cache). Linux honors absolute XDG base-directory variables and otherwise uses
`~/.local/share`, `~/.config`, and `~/.cache`. macOS uses the user's
`Library/Application Support`, `Library/Preferences`, and `Library/Caches`.
Append an application identifier before storing files. Environment-dependent
queries require a stable process environment. Executable-directory queries use
the native module path, `/proc/self/exe`, or `_NSGetExecutablePath`.

Temporary files and directories are exclusively created under a supplied
parent, or the system temporary directory. Their names are collision-resistant,
not secret. The caller closes file handles and explicitly removes objects.
Insufficient output capacity creates nothing and leaves the output untouched.

Native contracts: Microsoft's [CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
and [long-path rules](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation),
and the Linux [fsync documentation](https://man7.org/linux/man-pages/man2/fsync.2.html).
