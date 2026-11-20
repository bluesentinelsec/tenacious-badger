# Virtual file system

`badger/vfs.h` gives the engine one namespace of game files. The game asks for
`textures/rock.tex`; the VFS finds it in whichever mount has it and returns a
[stream](streams.md). The mounts are directories, archive files, and archives
held in memory.

```c
tb_vfs *vfs = tb_vfs_create(NULL);
uint8_t key[32];
game_package_key(key); /* See packages.md. */
tb_vfs_mount_archive_memory(vfs, "", 0, embedded_core, embedded_core_size, key, 0);
tb_vfs_mount_archive_file(vfs, "", 0, "C:/Games/Burrow/data/base.pkg", key, 10);
tb_vfs_mount_directory(vfs, "", 0, "C:/Users/me/Burrow/mods/sharper-rocks", 20);

tb_stream rock = {0};
if (tb_vfs_open(vfs, "textures/rock.tex", 17, NULL, &rock) == TB_VFS_OK)
{
    /* ... read it ... */
    tb_stream_close(&rock);
}
tb_vfs_destroy(vfs);
```

## Mounts

| Mount | Function | What it reads |
| --- | --- | --- |
| A directory | `tb_vfs_mount_directory` | Files on disk, live: files changed or added after mounting are seen. For development and mods. |
| An archive file | `tb_vfs_mount_archive_file` | A [package](packages.md), a ZIP archive, or a TAR archive, recognized by its bytes. The file stays open and is read at offsets. |
| An archive in memory | `tb_vfs_mount_archive_memory` | The same three kinds, from bytes that stay valid while the VFS lives, such as an archive embedded in the executable. It needs no file system at all. |
| A source of one's own | `tb_vfs_mount` | Anything that implements `tb_vfs_source_functions`: open, query, list, and release. A new kind of mount is a new source, not a change to the VFS. |

An archive is indexed once, when it is mounted. Every name it has, and every
directory those names imply, goes into a sorted array that is searched in
O(log n). That holds even for archives that list no directories of their own.

A `.tar.gz` cannot be read at offsets, and is `TB_VFS_UNSUPPORTED`:
decompress it first. Links in ZIP and TAR archives, and devices and other
kinds of entry, are left out.

## Priorities and mount points

- **Where two mounts have one name,** the mount of higher priority wins. Of
  equal priority, the one mounted later wins. A mod mounted over the base game
  replaces its files, and a patch mounted after the base, at its priority,
  replaces them too.
- **A directory lists the names of every mount, each once,** as the winning
  mount tells it, in the order of their bytes.
- **A mount point** places a mount in the namespace. Mounted at `mods/one`, an
  archive's `readme.txt` is `mods/one/readme.txt`, and `mods` is a directory
  that lists `one`.

Opening tries the mounts in order of priority until one has the name, so its
cost is the number of mounts that do not, times a binary search or a query
of the disk. `tb_bench vfs` opens files across ten package mounts in memory.

## Names

Names are UTF-8, case-sensitive, with `/` between components. A name is
`TB_VFS_INVALID`, and never reaches a mount, if it:

- has an empty, `.`, or `..` component, which a slash at either end makes;
- contains `\` or `:`, which mean other things to Windows;
- contains NUL, or is not UTF-8.

So no name leaves its mount, and every name means the same on every system.
The empty name is the root, which only `tb_vfs_query` and `tb_vfs_list` take.

**Archives are checked when they are mounted.** An archive with any entry
whose name the VFS would refuse is `TB_VFS_UNSAFE_NAME`, and is not mounted. A
name that is both a file and a directory in one archive is `TB_VFS_MALFORMED`.

**Directories on disk** are reached through `tb_path_rooted_join`, and links
are not followed. A name is `TB_VFS_NOT_FOUND` if its file, or any directory on
the way to it, is a link or another kind that is not a file or a directory. A
link inside a mounted directory cannot lead out of it. Names on disk that the
VFS could not name are left out of listings.

A directory mount checks each component, then opens the file. Something that
replaces a component with a link between the two is not detected. A directory
mount is for content the user controls; ship content in packages.

Names are case-sensitive everywhere, as packages, ZIP, and TAR are, so content
made on Windows must name its files as it asks for them.

## Threading

Mounting and destroying are externally synchronized with every other call:
mount during setup, then read. After that, `tb_vfs_open`, `tb_vfs_query`, and
`tb_vfs_list` are thread-safe, and any number of threads may read at once.
Archive mounts read at offsets; directory mounts open each file afresh. A
source of one's own must be as thread-safe.

The VFS is read-only. A writable mount for saved games and configuration is a
follow-up.
