"""Hold every source of the engine to a share of its lines being run by the tests.

    python tools/check_coverage.py <build directory> [--gcov "<command>"]
    python tools/check_coverage.py --cobertura <report.xml>
    python tools/check_coverage.py --self-test

Every source under src/ that was compiled is measured, a file at a time; none
is listed here by hand, so a new source is held to the bar from the change
that adds it.

A source is compiled into more than one program: the engine library, and the
test programs that are built from the engine's sources. A line counts as run
when any of them ran it.

With GCC or Clang, the build directory is one that was compiled with
`--coverage` and whose tests have been run. `--gcov` names the program that
reads the compiler's notes: `gcov` for GCC, which is the default, and
`llvm-cov gcov` for Clang.

MSVC has no such flag. On Windows the tests are run under
Microsoft.CodeCoverage.Console, which ships with Visual Studio and writes a
report in the Cobertura format; `--cobertura` reads it. MSVC gives the brace
that closes a function a line of code of its own, even in a function that
never returns, where no test can reach it. A line that holds nothing but a
closing brace is therefore not counted from such a report; GCC and Clang do
not count one either.

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
import xml.etree.ElementTree
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
                    name = engine_source(source, root, build) if source else None
                    if name is None:
                        continue
                    merged = sources.setdefault(name, {})
                    for number, ran in lines.items():
                        merged[number] = merged.get(number, False) or ran
    return sources


def engine_source(path, root, base):
    """The name of a source within the repository when it is one of the engine's, or None."""
    path = Path(path)
    if not path.is_absolute():
        path = base / path
    try:
        name = path.resolve().relative_to(root).as_posix()
    except ValueError:
        return None
    if name.startswith('src/') and name.endswith('.c'):
        return name
    return None


def closing_braces(text):
    """The lines of a source that hold nothing but a closing brace."""
    return {number for number, row in enumerate(text.splitlines(), start=1)
            if row.strip() == '}'}


def read_source(root, name):
    path = root / name
    if not path.is_file():
        return ''
    return path.read_text(encoding='utf-8')


def collect_cobertura(text, root, read=read_source):
    """The same from a Cobertura report: every line of every class, by its file."""
    sources = {}
    braces = {}
    for entry in xml.etree.ElementTree.fromstring(text).iter('class'):
        name = engine_source(entry.get('filename', ''), root, root)
        if name is None:
            continue
        if name not in braces:
            braces[name] = closing_braces(read(root, name))
        merged = sources.setdefault(name, {})
        for line in entry.iter('line'):
            number = int(line.get('number'))
            if number not in braces[name]:
                merged[number] = merged.get(number, False) or int(line.get('hits')) > 0
    return sources


def check(sources, root, where):
    if not sources:
        sys.exit(f'{where} has coverage of no source under src/: was the build instrumented, '
                 'and were the tests run?')
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
    root = Path(__file__).resolve().parent.parent
    inside = root / 'src' / 'core' / 'x.c'
    outside = root / 'tests' / 'core' / 'x_test.c'
    # The same file appears once for each program it was compiled into, and
    # its lines both under a method and under the class.
    cobertura = f'''<?xml version="1.0"?>
<coverage><packages>
  <package name="badger"><classes>
    <class filename="{inside}"><methods><method name="f"><lines>
      <line number="4" hits="0"/><line number="5" hits="0"/>
    </lines></method></methods><lines><line number="4" hits="0"/></lines></class>
    <class filename="{outside}"><lines><line number="1" hits="3"/></lines></class>
  </classes></package>
  <package name="tb_internal_tests"><classes>
    <class filename="{inside}"><lines>
      <line number="4" hits="2"/><line number="9" hits="1"/>
    </lines></class>
  </classes></package>
</packages></coverage>'''
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
        (collect_cobertura(cobertura, root, lambda root, name: ''),
         {'src/core/x.c': {4: True, 5: False, 9: True}}),
        (collect_cobertura(cobertura, root, lambda root, name: 'a\nb\nc\nd\n    }\n'),
         {'src/core/x.c': {4: True, 9: True}}),
        (closing_braces('void f(void)\n{\n    g();\n}\n  } \nx = {0};\n} else {\n'), {4, 5}),
        (engine_source(root / 'tests' / 'x.c', root, root), None),
        (engine_source('src/jobs/x.c', root, root), 'src/jobs/x.c'),
        (engine_source(root / 'src' / 'core' / 'x.h', root, root), None),
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
    root = Path(__file__).resolve().parent.parent
    if len(arguments) == 2 and arguments[0] == '--cobertura':
        text = Path(arguments[1]).read_text(encoding='utf-8-sig')
        check(collect_cobertura(text, root), root, arguments[1])
        return
    gcov = ['gcov']
    if '--gcov' in arguments:
        at = arguments.index('--gcov')
        gcov = shlex.split(arguments[at + 1])
        del arguments[at:at + 2]
    if len(arguments) != 1:
        sys.exit(__doc__)
    build = Path(arguments[0]).resolve()
    check(collect(build, root, gcov), root, build)


main()
