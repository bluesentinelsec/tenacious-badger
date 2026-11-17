"""Run the Clang static analyzer over the engine's sources.

    python tools/analyze_with_clang.py <compile_commands.json>
    python tools/analyze_with_clang.py --self-test

Clang's analyzer is a separate action of the compiler, which produces no
object file, so it cannot be a flag of the ordinary build as MSVC's and GCC's
are. This takes each compilation of a source under src/ from the compilation
database that CMake wrote, and runs it again as an analysis with the same
definitions and include paths. Any finding fails the run.

The `tb_analyze` target runs it; see cmake/StaticAnalysis.cmake.
"""
import concurrent.futures
import json
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

# Arguments of a compilation that say where its results go, each with the
# number of arguments that follow it and belong to it.
OUTPUT_ARGUMENTS = {'-o': 1, '-MF': 1, '-MT': 1, '-MQ': 1, '-MD': 0, '-MMD': 0, '-c': 0}


def analysis_arguments(compilation, output):
    """The arguments that analyze what `compilation` compiles."""
    arguments = []
    skip = 0
    for argument in compilation:
        if skip > 0:
            skip -= 1
        elif argument in OUTPUT_ARGUMENTS:
            skip = OUTPUT_ARGUMENTS[argument]
        else:
            arguments.append(argument)
    return arguments[:1] + ['--analyze', '--analyzer-output', 'text', '-o', output] + arguments[1:]


def is_engine_source(path, root):
    """Whether a source is the engine's own: under src/ of the repository."""
    try:
        relative = Path(path).resolve().relative_to(root)
    except ValueError:
        return False
    return relative.parts[:1] == ('src',) and relative.suffix == '.c'


def compilations(database, root):
    """The compilations of the engine's sources, one for each distinct command."""
    seen = set()
    for entry in json.loads(Path(database).read_text(encoding='utf-8')):
        arguments = entry.get('arguments') or shlex.split(entry['command'])
        key = (entry['file'], tuple(arguments))
        if is_engine_source(entry['file'], root) and key not in seen:
            seen.add(key)
            yield entry['directory'], entry['file'], arguments


def analyze(directory, source, arguments, scratch):
    output = os.path.join(scratch, f'{abs(hash((source, tuple(arguments))))}.plist')
    done = subprocess.run(analysis_arguments(arguments, output), cwd=directory,
                          capture_output=True, text=True, errors='replace')
    return source, done.returncode, done.stderr.strip()


def self_test():
    compilation = ['cc', '-DTB_X=1', '-Iinclude', '-O0', '-o', 'x.o', '-MD', '-MT', 'x.o', '-MF',
                   'x.d', '-c', 'src/core/x.c']
    expected = ['cc', '--analyze', '--analyzer-output', 'text', '-o', 'out', '-DTB_X=1',
                '-Iinclude', '-O0', 'src/core/x.c']
    root = Path(__file__).resolve().parent.parent
    answers = [
        (analysis_arguments(compilation, 'out'), expected),
        (is_engine_source(root / 'src' / 'core' / 'x.c', root), True),
        (is_engine_source(root / 'tests' / 'core' / 'x.c', root), False),
        (is_engine_source(root / 'src' / 'core' / 'x.h', root), False),
        (is_engine_source(root.parent / 'elsewhere' / 'src' / 'x.c', root), False),
    ]
    wrong = [(actual, wanted) for actual, wanted in answers if actual != wanted]
    for actual, wanted in wrong:
        print(f'self-test: got {actual!r}, expected {wanted!r}')
    return wrong


def main():
    if sys.argv[1:] == ['--self-test']:
        if self_test():
            sys.exit(1)
        print('The analyzer driver gives the known answers.')
        return
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = Path(__file__).resolve().parent.parent
    work = list(compilations(sys.argv[1], root))
    if not work:
        sys.exit(f'{sys.argv[1]} names no source under src/: was the build configured with '
                 'TB_ENABLE_STATIC_ANALYSIS and Clang?')
    findings = 0
    with tempfile.TemporaryDirectory(prefix='tb-analyze-') as scratch:
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
            jobs = [pool.submit(analyze, *entry, scratch) for entry in work]
            for job in jobs:
                source, status, report = job.result()
                if status != 0 or report:
                    findings += 1
                    print(f'{source}:\n{report or "the analyzer failed with status " + str(status)}\n')
    if findings:
        sys.exit(f'The Clang static analyzer reported on {findings} of {len(work)} compilations.')
    print(f'The Clang static analyzer found nothing in {len(work)} compilations.')


main()
