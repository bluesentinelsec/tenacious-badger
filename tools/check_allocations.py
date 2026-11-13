"""Fail when C code allocates memory without going through badger/memory.h.

Every allocation in the project goes through the engine's allocation functions
(tb_malloc, tb_calloc, tb_realloc, tb_free and their relatives), so that
allocations are counted, exhaustion is handled in one place, and the allocator
can be replaced. This check reads every C source and header listed in
cmake/FormatSources.cmake and reports any use of an allocation function from
the C library or the operating system.

A file that legitimately needs one is authorized by name in AUTHORIZED below,
with the reason. There is no way to suppress the check from inside a source
file.

  python tools/check_allocations.py              Check the project.
  python tools/check_allocations.py --self-test  Check this script.
"""
import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
SOURCE_LIST = ROOT / 'cmake/FormatSources.cmake'

# Allocation functions that project code must not use, and what to use instead.
C_LIBRARY = 'tb_malloc, tb_calloc, tb_realloc, or tb_free'
DUPLICATING = 'a copy made into memory from tb_malloc'
STACK = 'a fixed-size array, or memory from tb_malloc'
ALIGNED = 'tb_aligned_alloc and tb_aligned_free'
PLATFORM = 'the engine allocation functions, or a platform memory interface'
FORBIDDEN = {
    **dict.fromkeys(['malloc', 'calloc', 'realloc', 'free', 'reallocarray', '_recalloc',
                     '_expand'], C_LIBRARY),
    **dict.fromkeys(['strdup', '_strdup', 'strndup', 'wcsdup', '_wcsdup', '_mbsdup', 'asprintf',
                     'vasprintf'], DUPLICATING),
    **dict.fromkeys(['alloca', '_alloca', '_malloca', '_freea'], STACK),
    **dict.fromkeys(['aligned_alloc', 'posix_memalign', 'memalign', 'valloc', 'pvalloc',
                     '_aligned_malloc', '_aligned_realloc', '_aligned_recalloc',
                     '_aligned_free'], ALIGNED),
    **dict.fromkeys(['HeapAlloc', 'HeapReAlloc', 'HeapFree', 'HeapCreate', 'VirtualAlloc',
                     'VirtualFree', 'mmap', 'munmap'], PLATFORM),
}

# Files that may use the named functions, and why. Keep this list short: each
# entry is a place where allocations escape the engine's accounting.
AUTHORIZED = {
    'src/core/memory.c': (
        {'malloc', 'calloc', 'realloc', 'free'},
        'implements the engine allocation functions on top of the C library'),
    'tests/framework/src/support.c': (
        {'calloc', 'free'},
        'the test framework is independent of the engine it tests'),
}

IDENTIFIER = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')


def blank_comments_and_literals(text):
    """Replace comments and string/character literals with spaces.

    Newlines are kept, so line numbers in the result match the source.
    """
    result = []
    index = 0
    length = len(text)
    while index < length:
        character = text[index]
        following = text[index + 1] if index + 1 < length else ''
        if character == '/' and following == '/':
            end = text.find('\n', index)
            end = length if end == -1 else end
            # A line comment continues after a backslash-newline.
            while end < length and text[end - 1] == '\\':
                next_end = text.find('\n', end + 1)
                end = length if next_end == -1 else next_end
            result.append(re.sub(r'[^\n]', ' ', text[index:end]))
            index = end
        elif character == '/' and following == '*':
            end = text.find('*/', index + 2)
            end = length if end == -1 else end + 2
            result.append(re.sub(r'[^\n]', ' ', text[index:end]))
            index = end
        elif character in '"\'':
            end = index + 1
            while end < length and text[end] != character and text[end] != '\n':
                end += 2 if text[end] == '\\' else 1
            end = min(end + 1, length)
            result.append(re.sub(r'[^\n]', ' ', text[index:end]))
            index = end
        else:
            result.append(character)
            index += 1
    return ''.join(result)


def find_violations(path_name, text):
    """Return (line, function, replacement) for each unauthorized use in one file."""
    allowed = AUTHORIZED.get(path_name, (set(), ''))[0]
    violations = []
    for number, line in enumerate(blank_comments_and_literals(text).split('\n'), start=1):
        for match in IDENTIFIER.finditer(line):
            name = match.group(0)
            if name in FORBIDDEN and name not in allowed:
                violations.append((number, name, FORBIDDEN[name]))
    return violations


def listed_sources():
    """The C sources and headers of the project, from the explicit source list."""
    listing = SOURCE_LIST.read_text(encoding='utf-8')
    sources = re.findall(r'^\s+([\w./-]+\.(?:c|h|h\.in))\)?\s*$', listing, flags=re.MULTILINE)
    if not sources:
        sys.exit(f'{SOURCE_LIST}: no sources found; has its format changed?')
    return sources


def check_project():
    sources = listed_sources()
    problems = 0
    for path_name in AUTHORIZED:
        if path_name not in sources:
            print(f'{Path(__file__).name}: error: authorized file {path_name} is not in '
                  f'{SOURCE_LIST.name}; remove its entry')
            problems += 1
    for path_name in sources:
        text = (ROOT / path_name).read_text(encoding='utf-8')
        for number, name, replacement in find_violations(path_name, text):
            print(f'{ROOT / path_name}:{number}: error: {name} is not allowed here; '
                  f'use {replacement} (see docs/memory.md)')
            problems += 1
    if problems:
        print(f'{problems} unauthorized allocation function use(s) found.')
        return 1
    print(f'Checked {len(sources)} files: every allocation goes through badger/memory.h.')
    return 0


def self_test():
    """Check the scanner against small sources with known answers."""
    cases = [
        ('a plain call', 'void f(void) { char *p = malloc(4); free(p); }', ['malloc', 'free']),
        ('a function used as a value', 'allocator a = { calloc, realloc };',
         ['calloc', 'realloc']),
        ('spacing before the parenthesis', 'p = strdup (text);\nq = _aligned_malloc\n(1, 2);',
         ['strdup', '_aligned_malloc']),
        ('the line of each use', '\n\nint x;\nvoid *p = alloca(8);', ['alloca']),
        ('engine functions', 'p = tb_malloc(4); tb_free(p); q = tb_try_calloc(1, 1);', []),
        ('longer identifiers', 'my_malloc(1); malloc_count++; free_list = 0; refree();', []),
        ('line comments', 'int x; // malloc(1) would be wrong here\n', []),
        ('block comments', '/* free(p);\n   calloc(1, 1); */ int y;', []),
        ('string literals', 'puts("call malloc() and free()");', []),
        ('escaped quotes', 'puts("a \\" malloc(1) \\" b"); char c = \'\\\'\';', []),
        ('a comment marker inside a string', 'puts("// not a comment"); p = malloc(1);',
         ['malloc']),
        ('a quote inside a comment', "// don't\nvoid *p = realloc(q, 2);", ['realloc']),
        ('a continued line comment', '// note \\\nmalloc(1)\nint z;', []),
        ('member access is still a use', 'table.free(p); table->malloc(1);', ['free', 'malloc']),
        ('operating system heaps', 'HeapAlloc(h, 0, 8); VirtualFree(p, 0, 0); mmap(0);',
         ['HeapAlloc', 'VirtualFree', 'mmap']),
        ('an include of the allocator header', '#include <malloc.h>', ['malloc']),
    ]
    failures = 0
    for description, source, expected in cases:
        found = [name for _, name, _ in find_violations('example.c', source)]
        if found != expected:
            print(f'self-test failed: {description}: expected {expected}, found {found}')
            failures += 1
    line = find_violations('example.c', '\n\nint x;\nvoid *p = alloca(8);')[0][0]
    if line != 4:
        print(f'self-test failed: expected a use on line 4, found line {line}')
        failures += 1
    authorized = find_violations('src/core/memory.c', 'p = malloc(1); q = strdup(s); free(p);')
    if [name for _, name, _ in authorized] != ['strdup']:
        print(f'self-test failed: authorization must cover only the named functions: {authorized}')
        failures += 1
    if failures:
        return 1
    print(f'Self-test passed: {len(cases) + 2} checks.')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--self-test', action='store_true', help='check this script and exit')
    arguments = parser.parse_args()
    return self_test() if arguments.self_test else check_project()


if __name__ == '__main__':
    sys.exit(main())
