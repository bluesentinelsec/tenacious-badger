"""Make the TAR archives that tests/codec/tar_test.c reads, with three
independent writers: GNU tar, bsdtar (libarchive), and Python's tarfile.

    python tools/make_tar_test_data.py --gnu-tar PATH --bsdtar PATH

Each writer writes the same entries (listed in ENTRIES and in the test) in
each format it has: ustar, pax, and GNU. An entry a format cannot hold, such
as a name longer than 255 bytes in ustar, is left out of that archive, and so
is an entry a writer cannot write on Windows (see write_bsdtar and
write_gnu_tar). The archives go to tests/data/tar, with one of them
also compressed by gzip.

This is run at development time, on a machine with both tar programs; the
archives are committed, so the tests need neither.
"""
import argparse
import gzip
import io
import os
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

MTIME = 1700000000

LONG_NAME = '/'.join(['d' * 99] * 3) + '/long.txt'           # 308 bytes
NAME_100 = 'n' * 96 + '.txt'                                 # exactly 100 bytes
PREFIX_NAME = 'p' * 120 + '/needs-the-prefix-field.txt'      # 147 bytes; splits

# (name, kind, mode, contents). A directory's name ends in '/'.
ENTRIES = [
    ('hello.txt', 'file', 0o644, b'Hello, tar!\n'),
    ('empty.txt', 'file', 0o644, b''),
    ('dir/', 'directory', 0o755, None),
    ('dir/data.bin', 'file', 0o644, bytes((index * 7 + 3) & 0xFF for index in range(1000))),
    ('exact.bin', 'file', 0o600, bytes(index & 0xFF for index in range(512))),
    (NAME_100, 'file', 0o644, b'one hundred\n'),
    (PREFIX_NAME, 'file', 0o644, b'prefix\n'),
    (LONG_NAME, 'file', 0o644, b'long\n'),
    ('ünïcödé.txt', 'file', 0o644, 'ünïcödé\n'.encode('utf-8')),
    ('run.sh', 'file', 0o755, b'#!/bin/sh\necho run\n'),
    ('link', 'symlink', 0o777, 'hello.txt'),
    ('hard', 'hardlink', 0o644, 'hello.txt'),
]


def fits(name, form):
    """Whether ustar can hold a name: in the name field, or split with the prefix."""
    if form != 'ustar':
        return True
    data = name.rstrip('/').encode('utf-8') + (b'/' if name.endswith('/') else b'')
    if len(data) <= 100:
        return True
    for index, byte in enumerate(data):
        if byte == ord('/') and index <= 155 and len(data) - index - 1 <= 100:
            return True
    return False


def write_python(output, form):
    formats = {'ustar': tarfile.USTAR_FORMAT, 'pax': tarfile.PAX_FORMAT, 'gnu': tarfile.GNU_FORMAT}
    with tarfile.open(output, 'w', format=formats[form], encoding='utf-8') as archive:
        for name, kind, mode, contents in ENTRIES:
            if not fits(name, form):
                continue
            info = tarfile.TarInfo(name.rstrip('/'))
            info.mode = mode
            info.mtime = MTIME
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            if kind == 'directory':
                info.type = tarfile.DIRTYPE
                archive.addfile(info)
            elif kind == 'symlink':
                info.type = tarfile.SYMTYPE
                info.linkname = contents
                archive.addfile(info)
            elif kind == 'hardlink':
                info.type = tarfile.LNKTYPE
                info.linkname = contents
                archive.addfile(info)
            else:
                info.size = len(contents)
                archive.addfile(info, io.BytesIO(contents))


def stage(directory):
    """Write the files to a directory under short names; return their paths."""
    paths = {}
    for index, (name, kind, _, contents) in enumerate(ENTRIES):
        if kind == 'file':
            path = Path(directory) / f'f{index}'
            path.write_bytes(contents)
            paths[name] = path
    return paths


def mtree_escape(name):
    out = []
    for byte in name.encode('utf-8'):
        if byte < 0x21 or byte > 0x7E or byte in b'#\\=':
            out.append('\\%03o' % byte)
        else:
            out.append(chr(byte))
    return ''.join(out)


def write_bsdtar(bsdtar, output, form, staged):
    """bsdtar reads an mtree description, so names, modes, and links need no file system.
    mtree reads a name without a slash as within the last directory it named, so each such
    directory is closed again with '..' at once. bsdtar on Windows reads an mtree's names in
    the ANSI code page, which garbles UTF-8, so names that are not ASCII are left out."""
    lines = ['#mtree']
    for name, kind, mode, contents in ENTRIES:
        if not fits(name, form) or kind == 'hardlink' or not name.isascii():
            continue
        fields = [mtree_escape(name.rstrip('/')), f'mode={mode:o}', f'time={MTIME}.0', 'uid=0',
                  'gid=0']
        if kind == 'directory':
            fields.append('type=dir')
        elif kind == 'symlink':
            fields += ['type=link', 'link=' + mtree_escape(contents)]
        else:
            fields += ['type=file', 'contents=' + str(staged[name]).replace('\\', '/')]
        lines.append(' '.join(fields))
        if kind == 'directory':
            lines.append('..')
    spec = Path(output).with_suffix('.mtree')
    spec.write_text('\n'.join(lines) + '\n', encoding='ascii')
    names = {'ustar': 'ustar', 'pax': 'pax', 'gnu': 'gnutar'}
    subprocess.run([bsdtar, '-c', '-f', str(output), f'--format={names[form]}', '--uname', '',
                    '--gname', '', f'@{spec}'], check=True)
    spec.unlink()


def write_gnu_tar(gnu_tar, output, form, staged, directory):
    """GNU tar reads the file system, so short files are renamed with --transform. It takes one
    --mode for a whole archive, so each entry is written alone, one block to a record, and the
    archives are joined without their end blocks. Appending with -r would not do: it takes the
    format from the archive, which is plain ustar until a pax header is needed. It writes no
    links here: links on this file system are not reliable."""
    names = {'ustar': 'ustar', 'pax': 'posix', 'gnu': 'gnu'}
    arguments = [gnu_tar, '-c', '-f', '-', '--blocking-factor=1', f'--format={names[form]}',
                 '--owner=0', '--group=0', '--numeric-owner', f'--mtime=@{MTIME}',
                 '--no-recursion']
    if form == 'pax':
        # Without this, GNU tar records the time of access and change too.
        arguments.append('--pax-option=delete=atime,delete=ctime')
    end = bytes(1024)
    joined = b''
    for index, (name, kind, mode, _) in enumerate(ENTRIES):
        if not fits(name, form) or kind in ('symlink', 'hardlink'):
            continue
        if kind == 'directory':
            member = f'v{index}'
            (Path(directory) / member).mkdir()
        else:
            member = staged[name].name
        assert ',' not in name
        command = arguments + [f'--mode={mode:o}',
                               f'--transform=s,^{member}$,{name.rstrip("/")},', member]
        archive = subprocess.run(command, check=True, cwd=directory, capture_output=True).stdout
        assert archive.endswith(end), name
        joined += archive[:-len(end)]
    Path(output).write_bytes(joined + end)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--gnu-tar', required=True)
    parser.add_argument('--bsdtar', required=True)
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = root / 'tests' / 'data' / 'tar'
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as directory:
        staged = stage(directory)
        for form in ('ustar', 'pax', 'gnu'):
            write_python(output / f'python-{form}.tar', form)
            write_bsdtar(arguments.bsdtar, output / f'bsdtar-{form}.tar', form, staged)
            gnu_directory = Path(directory) / f'gnu-{form}'
            gnu_directory.mkdir()
            for path in staged.values():
                shutil.copy(path, gnu_directory / path.name)
            write_gnu_tar(arguments.gnu_tar, output / f'gnutar-{form}.tar', form,
                          {name: gnu_directory / path.name for name, path in staged.items()},
                          gnu_directory)
    data = (output / 'gnutar-pax.tar').read_bytes()
    with open(output / 'gnutar-pax.tar.gz', 'wb') as file:
        with gzip.GzipFile(filename='', mode='wb', fileobj=file, mtime=0) as compressed:
            compressed.write(data)
    for path in sorted(output.iterdir()):
        print(f'{path.name}: {path.stat().st_size} bytes')


if __name__ == '__main__':
    os.environ.setdefault('TZ', 'UTC')
    main()
