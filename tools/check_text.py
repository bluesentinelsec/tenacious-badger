"""Check what every tracked text file must have in common, whatever it holds.

    python tools/check_text.py              check the tracked files
    python tools/check_text.py --self-test  check the checker

A file must not begin with a byte-order mark. UTF-8 has no byte order to mark,
compilers and interpreters differ in what they make of one, and a tool that
compares the first bytes of a file with what it expects is thrown by it.

Test data under tests/data is exempt: it is kept byte for byte, and a corpus
may hold a byte-order mark on purpose, to test what a parser makes of one.
"""
import subprocess
import sys
from pathlib import Path

BYTE_ORDER_MARKS = {
    b'\xef\xbb\xbf': 'UTF-8',
    b'\xff\xfe': 'UTF-16, little-endian',
    b'\xfe\xff': 'UTF-16, big-endian',
}

# Files that are not text, by their last suffix.
BINARY_SUFFIXES = {'.png', '.ico', '.icns', '.bmp', '.zip', '.gz', '.bin', '.ttf'}

# Directories whose files are data, kept byte for byte.
DATA_DIRECTORIES = ('tests/data/',)


def mark_of(data):
    """The name of the byte-order mark that the bytes begin with, or None."""
    for mark, name in BYTE_ORDER_MARKS.items():
        if data.startswith(mark):
            return name
    return None


def is_data(name):
    """Whether a tracked file, named relative to the root, is test data."""
    return name.startswith(DATA_DIRECTORIES)


def tracked_files(root):
    listing = subprocess.run(['git', 'ls-files', '-z'], cwd=root, capture_output=True, check=True)
    names = listing.stdout.decode('utf-8').split('\0')
    return [root / name for name in names if name and not is_data(name)]


def check(root):
    faults = []
    for path in tracked_files(root):
        if path.suffix.lower() in BINARY_SUFFIXES or not path.is_file():
            continue
        with open(path, 'rb') as file:
            mark = mark_of(file.read(4))
        if mark is not None:
            faults.append(f'{path.relative_to(root).as_posix()}: begins with a byte-order mark '
                          f'({mark}); remove it')
    return faults


def self_test():
    answers = [
        (b'', None),
        (b'/* plain */', None),
        (b'\xef\xbb\xbf/* marked */', 'UTF-8'),
        (b'\xef\xbb', None),
        (b'\xff\xfe/\x00', 'UTF-16, little-endian'),
        (b'\xfe\xff\x00/', 'UTF-16, big-endian'),
        (b'a\xef\xbb\xbf', None),
    ]
    wrong = [(data, expected, mark_of(data)) for data, expected in answers
             if mark_of(data) != expected]
    for data, expected, actual in wrong:
        print(f'self-test: {data!r} gave {actual!r}, expected {expected!r}')
    exemptions = [
        ('tests/data/jsontestsuite/test_parsing/y_.json', True),
        ('tests/data', False),
        ('tests/database.c', False),
        ('src/tests/data/x.c', False),
    ]
    for name, expected in exemptions:
        if is_data(name) != expected:
            print(f'self-test: {name} is data: {is_data(name)}, expected {expected}')
            wrong.append((name, expected, is_data(name)))
    return wrong


def main():
    if sys.argv[1:] == ['--self-test']:
        if self_test():
            sys.exit(1)
        print('The text check gives the known answers.')
        return
    root = Path(__file__).resolve().parent.parent
    faults = check(root)
    for fault in faults:
        print(fault)
    if faults:
        sys.exit(1)
    print('No tracked text file begins with a byte-order mark.')


main()
