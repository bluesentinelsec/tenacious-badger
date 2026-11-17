# UTF-8 paths

`badger/path.h` handles spelling; it does not access a filesystem. Choose
`TB_PATH_WINDOWS` or `TB_PATH_POSIX` explicitly. Both are tested on every host.
All inputs are bounded UTF-8 views without embedded NUL. Parts returned by
`tb_path_parse` borrow the original input; builders return NUL-terminated text.

Builders have bounded and `_new` forms. A NULL destination with capacity zero
measures the bytes required, including NUL. Insufficient capacity returns
`TB_PATH_NO_SPACE` without changing the destination. Other errors set the
required size to zero. Inputs may overlap the destination. Owned strings use
the supplied allocator (NULL selects the heap) and are freed with `tb_str_free`
using that allocator. Temporary storage uses the engine heap. Allocation
exhaustion is fatal; malformed input and size overflow are returned as errors.

Normalization uses forward slashes, removes repeated separators and ordinary
`.` components, collapses `..` against preceding names, retains leading parents
on relative paths, and clamps them at roots. Empty input becomes `.`. Trailing
separators disappear except on roots. POSIX preserves exactly two leading
slashes; three or more become one. A backslash is an ordinary POSIX character.

Windows drive-relative `C:a` remains relative; `/a` is rooted on the current
drive but is not fully absolute. UNC requires both server and share. Extended
`//?/C:/` and `//?/UNC/server/share/` prefixes retain literal dot components;
the `UNC` spelling is uppercase. Other extended namespaces and `//./` device
paths are rejected. `tb_path_to_native` converts Windows separators back to
backslashes. Native filesystem validation, reserved names, and long-path
prefixing belong to the filesystem adapter.

Joins replace the first path when the second has a root (including `C:a`).
Relative-path construction requires matching fully absolute roots and ordinary
namespaces; comparisons are bytewise, including drive-letter case. Dotfiles
such as `.bashrc` have no extension; `archive.tar.gz` has `.gz`, and `name.` has
`.`. Extension replacement takes the leading dot or an empty view to remove it.
A trailing separator has no filename and cannot receive an extension.

`tb_path_rooted_join` requires an absolute ordinary root and rejects a rooted
child or any prefix of a child that climbs above that root. For example,
`a/../../b` fails even though another later component might reenter the tree.
This is **lexical containment**, suitable for virtual archive names. It is not
an OS security boundary: symlinks, reparse points, alternate streams, filesystem
aliases, and subsequent percent decoding can change what a name resolves to.
Percent-encoded text is literal and must not be decoded after containment.
Filesystem containment requires handle-relative lookup and a separate policy.

All operations are thread-safe when callers synchronize shared buffers and
allocators. No function changes the process working directory.

Windows namespace behavior is described in Microsoft's
[Naming Files, Paths, and Namespaces](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file).
