"""Hold every source of the engine to a share of its lines being run by the tests.

    python tools/check_coverage.py <build directory> [--gcov "<command>"]
    python tools/check_coverage.py --self-test

The build directory is one that was compiled with `--coverage` by GCC or
Clang and whose tests have been run. Every source under src/ that was compiled
there is measured, a file at a time; none is listed here by hand, so a new
source is held to the bar from the change that adds it.

A source is compiled into more than one program: the engine library, and the
test programs that are built from the engine's sources. A line counts as run
when any of them ran it.

`--gcov` names the program that reads the compiler's notes: `gcov` for GCC,
which is the default, and `llvm-cov gcov` for Clang.

Lines that no test can reach
----------------------------

Some lines handle a failure of the operating system that a test cannot
provoke. They are left out of the count between two comments:

    /* coverage-off: pthread_create fails only when the system is out of threads */
    ...
    /* coverage-on */

The reason is required, and what is left out is printed, so that it is seen in
review and in every run.
"""
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

MINIMUM = 0.80

# A line of a .gcov report: "    count:  line:source". The count is "-" for a
# line with no code, "#####" or "=====" for code that never ran, and otherwise
# a number, which GCC marks with "*" when only part of the line ran.
REPORT_LINE = re.compile(r'^\s*(-|#####|=====|\d+\*?):\s*(\d+):')
SOURCE_LINE = re.compile(r'^\s*-:\s*0:Source:(.*)$')
OFF = re.compile(r'coverage-off\b(?::\s*(\S.*?))?\s*(\*/)?\s*$')
ON = re.compile(r'coverage-on\b')


def parse_report(text):
    """The source that a .gcov report is about, and whether each of its lines of code ran."""
    source = None
    lines = {}
    for row in text.splitlines():
        named = SOURCE_LINE.match(row)
        if named:
            source = named.group(1)
            continue
        counted = REPORT_LINE.match(row)
        if counted and counted.group(1) != '-' and int(counted.group(2)) > 0:
            number = int(counted.group(2))
            ran = counted.group(1)[0].isdigit() and int(counted.group(1).rstrip('*')) > 0
            lines[number] = lines.get(number, False) or ran
    return source, lines


def excluded_lines(text):
    """The lines between coverage-off and coverage-on, and what is wrong with the comments."""
    excluded = set()
    faults = []
    start = None
    for number, row in enumerate(text.splitlines(), start=1):
        off = OFF.search(row)
        if off:
            if start is not None:
                faults.append(f'line {number}: coverage-off inside coverage-off from line {start}')
            if not off.group(1):
                faults.append(f'line {number}: coverage-off gives no reason')
            start = number
        elif ON.search(row):
            if start is None:
                faults.append(f'line {number}: coverage-on without coverage-off')
            else:
                excluded.update(range(start, number + 1))
            start = None
    if start is not None:
        faults.append(f'line {start}: coverage-off is never turned on again')
    return excluded, faults


def measure(lines, excluded):
    """Lines of code that count, how many of them ran, and which did not."""
    counted = {number: ran for number, ran in lines.items() if number not in excluded}
    missed = sorted(number for number, ran in counted.items() if not ran)
    return len(counted), len(counted) - len(missed), missed


def ranges(numbers):
    """Line numbers as text, with runs written as first-last."""
    parts = []
    for number in numbers:
        if parts and parts[-1][1] == number - 1:
            parts[-1][1] = number
        else:
            parts.append([number, number])
    return ', '.join(str(a) if a == b else f'{a}-{b}' for a, b in parts)


def read_notes(gcov, notes, scratch):
    """Runs gcov on one notes file and returns the reports it wrote."""
    directory = tempfile.mkdtemp(dir=scratch)
    subprocess.run(gcov + ['-o', str(notes.parent), str(notes)], cwd=directory, check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return [path.read_text(encoding='utf-8', errors='replace')
            for path in Path(directory).glob('*.gcov')]


def collect(build, root, gcov):
    """For every source under src/ that the build compiled: whether each line of code ran."""
    sources = {}
    notes = sorted(build.rglob('*.gcno'))
    with tempfile.TemporaryDirectory(prefix='line-coverage-', dir=build) as scratch:
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
            for reports in pool.map(lambda path: read_notes(gcov, path, scratch), notes):
                for text in reports:
                    source, lines = parse_report(text)
                    if source is None:
                        continue
                    path = Path(source)
                    if not path.is_absolute():
                        path = build / path
                    try:
                        name = path.resolve().relative_to(root).as_posix()
                    except ValueError:
                        continue
                    if not name.startswith('src/') or not name.endswith('.c'):
                        continue
                    merged = sources.setdefault(name, {})
                    for number, ran in lines.items():
                        merged[number] = merged.get(number, False) or ran
    return sources, len(notes)


def check(build, root, gcov):
    sources, notes_count = collect(build, root, gcov)
    if not sources:
        sys.exit(f'{build} has coverage notes for no source under src/ ({notes_count} notes '
                 'files): was it built with --coverage, and were the tests run?')
    failed = []
    excluded_total = 0
    print(f'{"lines":>7} {"run":>7} {"share":>7} {"left out":>9}  source')
    for name in sorted(sources):
        excluded, faults = excluded_lines((root / name).read_text(encoding='utf-8'))
        total, ran, missed = measure(sources[name], excluded)
        left_out = len(excluded & set(sources[name]))
        excluded_total += left_out
        share = ran / total if total else 1.0
        print(f'{total:7} {ran:7} {share:7.1%} {left_out:9}  {name}')
        for fault in faults:
            failed.append(f'{name}: {fault}')
        if share < MINIMUM:
            failed.append(f'{name}: {share:.1%} of its lines are run, and lines '
                          f'{ranges(missed)} are not')
    not_built = sorted(path.relative_to(root).as_posix() for path in (root / 'src').rglob('*.c')
                       if path.relative_to(root).as_posix() not in sources)
    print(f'\n{len(sources)} sources measured; {excluded_total} lines of code left out by '
          'coverage-off comments.')
    if not_built:
        print('Not compiled in this build, and so not measured here:')
        for name in not_built:
            print(f'  {name}')
    if failed:
        print()
        for fault in failed:
            print(fault)
        sys.exit(f'Every source of the engine must have at least {MINIMUM:.0%} of its lines '
                 'run by the tests.')


def self_test():
    report = '''        -:    0:Source:/work/src/core/x.c
        -:    0:Graph:x.gcno
        -:    1:#include "x.h"
        3:    2:int f(int a)
        3:    3:{
       1*:    4:    if (a) { return 1; }
    #####:    5:    return 2;
    =====:    6:    abort();
        -:    7:}
'''
    marked = '''int f(void)
{
    /* coverage-off: the system call fails only without memory */
    if (g() != 0)
    {
        return 1;
    }
    /* coverage-on */
    return 0;
}
'''
    answers = [
        (parse_report(report), ('/work/src/core/x.c', {2: True, 3: True, 4: True, 5: False,
                                                        6: False})),
        (parse_report('        -:    0:Source:a.c\n        2:    1:x\n    #####:    1:x\n'),
         ('a.c', {1: True})),
        (excluded_lines(marked), (set(range(3, 9)), [])),
        (excluded_lines('/* coverage-off */\nx\n/* coverage-on */\n')[1],
         ['line 1: coverage-off gives no reason']),
        (excluded_lines('/* coverage-off: because */\nx\n')[1],
         ['line 1: coverage-off is never turned on again']),
        (excluded_lines('x\n/* coverage-on */\n')[1],
         ['line 2: coverage-on without coverage-off']),
        (excluded_lines('// coverage-off: a\n// coverage-off: b\n// coverage-on\n')[1],
         ['line 2: coverage-off inside coverage-off from line 1']),
        (measure({1: True, 2: False, 3: False, 4: True}, {3}), (3, 2, [2])),
        (measure({}, set()), (0, 0, [])),
        (ranges([1, 2, 3, 7, 9, 10]), '1-3, 7, 9-10'),
    ]
    wrong = [(actual, wanted) for actual, wanted in answers if actual != wanted]
    for actual, wanted in wrong:
        print(f'self-test: got {actual!r}, expected {wanted!r}')
    return wrong


def main():
    arguments = sys.argv[1:]
    if arguments == ['--self-test']:
        if self_test():
            sys.exit(1)
        print('The coverage check gives the known answers.')
        return
    gcov = ['gcov']
    if '--gcov' in arguments:
        at = arguments.index('--gcov')
        gcov = shlex.split(arguments[at + 1])
        del arguments[at:at + 2]
    if len(arguments) != 1:
        sys.exit(__doc__)
    root = Path(__file__).resolve().parent.parent
    check(Path(arguments[0]).resolve(), root, gcov)


main()
