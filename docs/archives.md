# Archives

Archives hold many files in one, for exports, packages, and the exchange of
content with other tools. Each format is a codec in the `codec` module, read
and written through [streams](streams.md).

## TAR

`badger/tar.h` reads and writes TAR archives in order, never seeking, so an
archive can come through a decompression stream or go to one. A `.tar.gz` is a
TAR reader over `tb_inflate_open_stream` in gzip format, or a TAR writer into
`tb_deflate_open_stream` (see [codecs](codecs.md)).

| Format | Read | Written |
| --- | --- | --- |
| POSIX ustar, with names split between the prefix and name fields | Yes | Yes |
| pax extended headers: paths and link targets of any length, sizes of 8 GiB and more, times out of the octal range | Yes | Yes, when the ustar fields cannot hold a value |
| pax global headers | Checked, and their records ignored | No |
| GNU long names and link targets (`././@LongLink`) and base-256 numbers | Yes | No |
| The older format without a magic | Yes, by its checksum | No |
| GNU sparse files and multi-volume archives | `TB_TAR_UNSUPPORTED` | No |

### Reading

A reader (`tb_tar_reader_create`) takes the entries in order.
`tb_tar_reader_next` describes the next one. It gives the name, the type, the
link target, the size, the permissions, and the time. Its data is read with
`tb_tar_reader_read`, or through a stream from `tb_tar_reader_open_data`. Data
that is not read is skipped by the next call. After the last entry the reader
returns `TB_TAR_END`, and it returns that again on every later call; so does
any failure.

Names come without a trailing slash, even for a directory, and the type says
what the entry is. Types other than files, directories, and links, such as
devices and FIFOs, are `TB_TAR_OTHER`, with the header's type flag kept in
`flag`.

### Untrusted archives

Archives come from downloads and mods, so the reader treats them as hostile:

- **Every header is checked.** The checksum must match (unsigned, or the
  signed sum some old writers made). Numeric fields must be octal or GNU
  base-256, and a damaged one is `TB_TAR_MALFORMED`.
- **Extended headers have limits.** By default an extended header holds at
  most 1 MiB, and a name or link target at most 64 KiB. Anything longer is
  `TB_TAR_TOO_LARGE`; `tb_tar_options` sets both limits.
- **A source that ends early is `TB_TAR_TRUNCATED`.** That includes a source
  that ends inside a header, inside data, or at an entry's edge without the
  zero blocks that end an archive. A source cut short at an edge looks exactly
  like an archive whose writer left out the end blocks. Only
  `TB_TAR_ALLOW_MISSING_END` takes such an archive as complete.
- **Names are reported as they are.** A name or link target may climb out of
  its directory (`../`), be absolute (`/etc`, `C:/`), use backslashes, or not
  be UTF-8, because ustar names no encoding. The reader never changes or
  follows them. Code that makes files of them joins each to its destination
  with `tb_path_rooted_join` (see [paths](paths.md)), which refuses every one
  of those.
- **Memory is bounded.** The reader holds one header block, the current name
  and link target, and one extended header at a time. Data passes straight to
  the caller.

`tb_tar_reader_offset` tells where in the archive a failure happened, for a
message.

### Writing

A writer (`tb_tar_writer_create`) writes each entry as a header and then
exactly its data. `tb_tar_writer_finish` ends the archive with two zero blocks
and flushes the sink.

- **Names that fit go in the ustar fields.** A name splits between the prefix
  and name fields when it must. Longer names, link targets over 100 bytes,
  sizes of 8 GiB and more, and times before 1970 or after 2242 go in a pax
  header, which GNU tar, bsdtar, and Python's tarfile all read.
- **What an entry may be is checked first.** A name must be a relative path of
  UTF-8, without empty, `.`, or `..` components. A link needs a target, and
  only a file has data. An entry that breaks a rule is `TB_TAR_INVALID`, and
  nothing is written.
- **Nothing of the host is written.** Each entry gets the permissions and time
  it is given, and owner and group 0 with no names. A mode of 0 means the usual:
  0644 for a file, 0755 for a directory, and 0777 for a symbolic link. The same
  entries make the same bytes on every machine, and executable bits survive
  into Linux exports.
- **A sink that refuses** makes the writer return `TB_TAR_OUTPUT_FAILED` from
  then on. An entry begun when the sink refuses still owes its data, so the
  caller's calls stay the same.

Beginning an entry before the last one's data is complete, writing more data
than an entry's size, and finishing twice are contract violations.

Extracting an archive to a directory is not part of the reader. That belongs
to the [virtual file system](https://github.com/bluesentinelsec/tenacious-badger/issues/152),
which mounts archives and joins every name under its root.

### Tests

The tests read archives that GNU tar, bsdtar, and Python's tarfile wrote, in
ustar, pax, and GNU format. They are kept in `tests/data/tar`, and
`tools/make_tar_test_data.py` remade them; it needs both tar programs. The tests
also cover:

- every limit of the ustar fields;
- sizes on each side of 8 GiB;
- reading one byte at a time, and through gzip;
- every truncation, damaged headers, and malformed extended headers;
- hostile names;
- random damage.

`tb_bench tar` times writing and reading an archive of 1,000 files of 4 KiB.

## ZIP

`badger/zip.h` reads ZIP archives from any stream that reports its size and
reads at an offset, such as a file or memory, and writes them to any stream.

### Reading

`tb_zip_open` reads and checks the whole central directory once. After that
the archive does not change:

- `tb_zip_count` and `tb_zip_entry_at` list the entries;
- `tb_zip_find` finds one by name, through a hash table;
- `tb_zip_open_entry` opens one as a stream.

An entry's stream inflates a deflated entry. It ends at the entry's stated
size and checks the CRC-32 when it gets there. Entries are read at their
offsets, so several can be open at once, from as many threads as the source
allows (files and memory allow any number). This is what a virtual file
system needs to mount an archive.

An entry reports:

- its name, with no trailing slash;
- its type: a file, a directory, or a symbolic link (by its Unix mode, with the
  target as its data);
- its compression method, and whether it is encrypted;
- its sizes and CRC-32;
- its Unix permissions, when the archive has them;
- its time.

The time comes from the extended timestamp, in UTC, when there is one.
Otherwise it is the DOS time read as UTC, because DOS times name no zone.

| Feature | Read | Written |
| --- | --- | --- |
| Stored and deflated entries | Yes | Yes |
| Data descriptors | Yes | Yes, after every entry with data |
| ZIP64: sizes and offsets past 4 GiB, more than 65,535 entries | Yes | Yes, when needed |
| UTF-8 names (flag 11) and Info-ZIP Unicode paths | Yes | Yes, always flagged |
| Names in code page 437, without the flag | Converted to UTF-8 | No |
| Extended timestamps and Unix modes | Yes | Yes |
| Archive and entry comments, unknown extra fields | Skipped | No |
| Encryption, other methods | `TB_ZIP_UNSUPPORTED` when opened | No |
| Archives over several volumes | `TB_ZIP_UNSUPPORTED` | No |

### Untrusted archives

Before any entry is read, `tb_zip_open` checks the whole central directory:

- **The end record** must be the last record, followed only by its own
  comment, which may be up to 65,535 bytes. Data before the archive or after
  the record is `TB_ZIP_MALFORMED`. So is a self-extracting archive: the
  reader does not search for an archive inside another file.
- **The central directory** must lie inside the archive and hold exactly the
  entries it claims, each with a well-formed record and extra fields.
- **No two entries may overlap**, with each other or with the central
  directory. An archive that reuses the same compressed bytes for many entries
  to multiply their size is refused.
- **Sizes must be possible.** A stored entry's two sizes must agree. A
  deflated entry may not claim more than DEFLATE's 1032:1 expansion.
- **Entries are limited** to 1,048,576 by default (`tb_zip_options`).
  Duplicate names are `TB_ZIP_DUPLICATE_NAME` unless
  `TB_ZIP_ALLOW_DUPLICATE_NAMES` is set, in which case `tb_zip_find` finds the
  last.

Opening an entry checks its local header:

- its signature;
- its method;
- its name, byte for byte against the central record;
- its sizes and CRC-32, when it has no data descriptor;
- that its data fits before the next entry.

The stream refuses data that inflates to more or less than the stated size, or
to a different CRC-32, as `TB_STREAM_MALFORMED`. Memory never depends on what
an entry claims.

Names are reported as stored, so a name may climb out of its directory or be
absolute. As with TAR, join each one to its destination with
`tb_path_rooted_join`.

### Writing

A writer (`tb_zip_writer_create`) writes each entry as a local header, its
data, and a data descriptor with the CRC-32 and sizes. It writes the central
directory when it finishes, so it never seeks. The caller gives each file's
size up front, and the writer requires exactly that much data, as the TAR
writer does.

- **Methods:** a file is stored, or deflated at levels 1 to 9. Directories and
  empty files are stored, with no descriptor.
- **Names** are relative paths of UTF-8 under the same rules as TAR, and a
  name may not be written twice.
- **ZIP64 fields** are written for a file of 3.75 GiB or more, and for any size,
  offset, or count that its 16- or 32-bit field cannot hold.
- **Permissions and time:** Unix permissions are kept. A mode of 0 means 0644
  for a file and 0755 for a directory. Each entry gets a DOS time (1980 to
  2107, clamped), plus an extended timestamp whenever the time fits 32 bits.
- **Nothing of the host is written.** The same entries make the same bytes.

### Tests

The tests read archives written by Python's zipfile, bsdtar, and .NET. They
are kept in `tests/data/zip`, and `tools/make_zip_test_data.py` remade them.
Python's include variants with:

- ZIP64 fields;
- a 65,535-byte comment;
- an unknown extra field;
- a code page 437 name;
- no entries at all.

Python, Info-ZIP's unzip, bsdtar, and .NET read the writer's archives. That was
checked at development time.

The tests also cover:

- more than 65,535 entries;
- an entry of 5 GiB, from a source that makes up its zeros;
- every truncation, and inconsistent and damaged directories and data;
- hostile names;
- random damage.

`tb_bench zip` times writing and reading 1,000 files of 4 KiB.
