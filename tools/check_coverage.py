"""Gate GCC source-line coverage, merging separately linked test executables."""
import gzip
import json
from pathlib import Path
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
groups = {
    'arena': ['src/core/arena.c'],
    'memory_debug': ['src/core/memory_debug.c'],
    'utf8': ['src/core/utf8.c'],
    'path': ['src/core/path.c'],
    'files': ['src/core/fs.c'],
    'virtual_memory': ['src/platform/posix/virtual_memory.c',
                       'src/platform/linux/virtual_memory.c'],
    'native_files': ['src/platform/posix/fs.c', 'src/platform/shared/fs.c',
                     'src/platform/linux/fs_location.c'],
}
# These executables deliberately exercise different layers of the same source.
targets = ['badger.dir', 'tb_internal_tests.dir', 'tb_platform_tests.dir']
lines = {source: {} for sources in groups.values() for source in sources}
with tempfile.TemporaryDirectory(prefix='line-coverage-', dir=build) as scratch:
    for notes in build.rglob('*.gcno'):
        if not any(target in notes.parts for target in targets):
            continue
        if not any(notes.name == Path(source).name + '.gcno' for source in lines):
            continue
        subprocess.run(['gcov', '--json-format', str(notes)], cwd=scratch,
                       check=True, stdout=subprocess.DEVNULL)
        report = Path(scratch) / (notes.name.removesuffix('.gcno') + '.gcov.json.gz')
        with gzip.open(report, 'rt') as stream:
            data = json.load(stream)
        for entry in data['files']:
            source = next((name for name in lines
                           if entry['file'].replace('\\', '/').endswith(name)), None)
            if source is None:
                continue
            for line in entry['lines']:
                number = line['line_number']
                lines[source][number] = lines[source].get(number, False) or line['count'] > 0

failed = False
for group, sources in groups.items():
    missing = [source for source in sources if not lines[source]]
    if missing:
        print(f'{group}: no coverage data for {missing}')
        failed = True
        continue
    total = sum(len(lines[source]) for source in sources)
    covered = sum(sum(lines[source].values()) for source in sources)
    print(f'{group}: {covered}/{total} source lines ({covered / total:.1%})')
    if covered / total < 0.8:
        failed = True
        for source in sources:
            print(f'  {source}: uncovered lines ' + ', '.join(
                str(number) for number, hit in sorted(lines[source].items()) if not hit))
if failed:
    sys.exit('Foundation source coverage must be at least 80% per module.')
