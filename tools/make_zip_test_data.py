"""Make the ZIP archives that tests/codec/zip_test.c reads, with three
independent writers: Python's zipfile, bsdtar (libarchive), and .NET's
System.IO.Compression through Windows PowerShell.

    python tools/make_zip_test_data.py --bsdtar PATH --powershell PATH

Each writer writes the entries of ENTRIES (listed in the test too) that it
can: .NET keeps no Unix permissions or links, and bsdtar on Windows garbles
names that are not ASCII (see make_tar_test_data.py). Python also writes the
archives that test one feature each:

    python-zip64.zip     every entry with ZIP64 extra fields
    python-comments.zip  an archive comment of 65,535 bytes, the most there is,
                         and entry comments
    python-legacy.zip    a name in code page 437, without the UTF-8 flag
    python-extra.zip     an extra field no reader knows
    python-empty.zip     no entries at all

This is run at development time on Windows; the archives are committed, so
the tests need none of the writers.
"""
import argparse
import os
import subprocess
import tempfile
import zipfile
from pathlib import Path

# 2023-11-14 22:13:20 UTC, an even number of seconds, as DOS times have.
MTIME = 1700000000
DOS_TIME = (2023, 11, 14, 22, 13, 20)

BIG_TEXT = b''.join(b'line %05d of a text that compresses well\n' % index for index in range(2500))

# (name, kind, mode, method, contents). A directory's name ends in '/'.
ENTRIES = [
    ('hello.txt', 'file', 0o644, zipfile.ZIP_DEFLATED, b'Hello, zip!\n'),
    ('empty.txt', 'file', 0o644, zipfile.ZIP_STORED, b''),
    ('dir/', 'directory', 0o755, zipfile.ZIP_STORED, None),
    ('dir/data.bin', 'file', 0o644, zipfile.ZIP_DEFLATED,
     bytes((index * 7 + 3) & 0xFF for index in range(1000))),
    ('stored.bin', 'file', 0o600, zipfile.ZIP_STORED, bytes(index & 0xFF for index in range(512))),
    ('big.txt', 'file', 0o644, zipfile.ZIP_DEFLATED, BIG_TEXT),
    ('ünïcödé.txt', 'file', 0o644, zipfile.ZIP_DEFLATED,
     'ünïcödé\n'.encode('utf-8')),
    ('run.sh', 'file', 0o755, zipfile.ZIP_DEFLATED, b'#!/bin/sh\necho run\n'),
    ('link', 'symlink', 0o777, zipfile.ZIP_STORED, b'hello.txt'),
]


def info_of(name, kind, mode, method):
    info = zipfile.ZipInfo(name, date_time=DOS_TIME)
    info.compress_type = method
    info.create_system = 3  # Unix, so the mode is kept.
    types = {'file': 0o100000, 'directory': 0o040000, 'symlink': 0o120000}
    info.external_attr = (types[kind] | mode) << 16
    if kind == 'directory':
        info.external_attr |= 0x10
    return info


def write_python(path, force_zip64=False, comment=b'', entry_comment=b'', extra=b''):
    with zipfile.ZipFile(path, 'w') as archive:
        archive.comment = comment
        for name, kind, mode, method, contents in ENTRIES:
            info = info_of(name, kind, mode, method)
            info.comment = entry_comment
            info.extra = extra
            if kind == 'directory':
                archive.writestr(info, b'')
                continue
            with archive.open(info, 'w', force_zip64=force_zip64) as entry:
                entry.write(contents)


def write_legacy(path):
    """A name in code page 437: zipfile writes a name that is ASCII without the
    UTF-8 flag, so an ASCII stand-in of the same length is written, then
    replaced in both headers."""
    stand_in = b'caf_.txt'
    legacy = 'café.txt'.encode('cp437')
    with zipfile.ZipFile(path, 'w') as archive:
        info = info_of(stand_in.decode('ascii'), 'file', 0o644, zipfile.ZIP_DEFLATED)
        archive.writestr(info, b'legacy\n')
    data = Path(path).read_bytes()
    assert data.count(stand_in) == 2
    Path(path).write_bytes(data.replace(stand_in, legacy))


def write_bsdtar(bsdtar, path, directory):
    """bsdtar reads an mtree description; see make_tar_test_data.py."""
    lines = ['#mtree']
    for index, (name, kind, mode, method, contents) in enumerate(ENTRIES):
        if not name.isascii():
            continue
        fields = [name.rstrip('/'), f'mode={mode:o}', f'time={MTIME}.0', 'uid=0', 'gid=0']
        if kind == 'directory':
            fields.append('type=dir')
        elif kind == 'symlink':
            fields += ['type=link', 'link=' + contents.decode('ascii')]
        else:
            source = Path(directory) / f'f{index}'
            source.write_bytes(contents)
            fields += ['type=file', 'contents=' + str(source).replace('\\', '/')]
        lines.append(' '.join(fields))
        if kind == 'directory':
            lines.append('..')
    spec = Path(directory) / 'entries.mtree'
    spec.write_text('\n'.join(lines) + '\n', encoding='ascii')
    subprocess.run([bsdtar, '-c', '-f', str(path), '--format=zip', '--uname', '', '--gname', '',
                    f'@{spec}'], check=True)


DOTNET_SCRIPT = r'''
param([string]$Output, [string]$Directory)
Add-Type -AssemblyName System.IO.Compression
$stream = [System.IO.File]::Open($Output, [System.IO.FileMode]::Create)
$archive = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create, $false, [System.Text.Encoding]::UTF8)
$time = New-Object DateTimeOffset(2023, 11, 14, 22, 13, 20, [TimeSpan]::Zero)
foreach ($line in [System.IO.File]::ReadAllLines((Join-Path $Directory 'entries.txt'), [System.Text.Encoding]::UTF8)) {
    $parts = $line.Split("`t")
    $level = if ($parts[1] -eq 'stored') { [System.IO.Compression.CompressionLevel]::NoCompression } else { [System.IO.Compression.CompressionLevel]::Optimal }
    $entry = $archive.CreateEntry($parts[0], $level)
    $entry.LastWriteTime = $time
    if ($parts.Length -gt 2) {
        $bytes = [System.IO.File]::ReadAllBytes((Join-Path $Directory $parts[2]))
        $writer = $entry.Open()
        $writer.Write($bytes, 0, $bytes.Length)
        $writer.Dispose()
    }
}
$archive.Dispose()
$stream.Dispose()
'''


def write_dotnet(powershell, path, directory):
    """.NET keeps no Unix permissions or links."""
    lines = []
    for index, (name, kind, mode, method, contents) in enumerate(ENTRIES):
        level = 'stored' if method == zipfile.ZIP_STORED else 'deflated'
        if kind == 'directory':
            lines.append(f'{name}\t{level}')
        elif kind == 'file':
            (Path(directory) / f'd{index}').write_bytes(contents)
            lines.append(f'{name}\t{level}\td{index}')
    (Path(directory) / 'entries.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    script = Path(directory) / 'write.ps1'
    script.write_text(DOTNET_SCRIPT, encoding='utf-8')
    subprocess.run([powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script),
                    '-Output', str(path), '-Directory', str(directory)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--bsdtar', required=True)
    parser.add_argument('--powershell', required=True)
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = root / 'tests' / 'data' / 'zip'
    output.mkdir(parents=True, exist_ok=True)
    write_python(output / 'python.zip')
    write_python(output / 'python-zip64.zip', force_zip64=True)
    write_python(output / 'python-comments.zip', comment=b'c' * 65535, entry_comment=b'an entry')
    write_python(output / 'python-extra.zip', extra=b'\xfe\xca\x04\x00abcd')
    write_legacy(output / 'python-legacy.zip')
    with zipfile.ZipFile(output / 'python-empty.zip', 'w'):
        pass
    with tempfile.TemporaryDirectory() as directory:
        write_bsdtar(arguments.bsdtar, output / 'bsdtar.zip', directory)
    with tempfile.TemporaryDirectory() as directory:
        write_dotnet(arguments.powershell, output / 'dotnet.zip', directory)
    for path in sorted(output.iterdir()):
        print(f'{path.name}: {path.stat().st_size} bytes')


if __name__ == '__main__':
    os.environ.setdefault('TZ', 'UTC')
    main()
