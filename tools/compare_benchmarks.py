"""Compare the benchmarks of two builds of the engine.

    python tools/compare_benchmarks.py --before <tb_bench> --after <tb_bench>
        [--rounds N] [--repeats N] [--margin RATIO] [--summary FILE] [module ...]
    python tools/compare_benchmarks.py --results <before.tsv> <after.tsv> [--margin RATIO]
    python tools/compare_benchmarks.py --self-test

The two programs are run alternately, round after round, on this machine, and
the fastest time of each case is kept. That is the only comparison of two
builds that means anything: numbers from another machine, or from this one an
hour ago, say little about each other (see docs/benchmarks.md).

A case is reported when it is slower or faster than before by more than the
margin, when it is new, and when it is gone. `--summary` appends the same as
Markdown to a file, which is how CI shows it on the page of a run.

The exit status is 0 whatever is found: on a shared machine one case in a
hundred is slower by chance, and a build that failed on it would teach people
to ignore the failure. `--fail-above RATIO` makes it 1 when a case is slower
by more than that ratio, for a machine that is quiet enough to mean it.
"""
import subprocess
import sys
from pathlib import Path

DEFAULT_MARGIN = 1.25
DEFAULT_ROUNDS = 3
DEFAULT_REPEATS = 3


def parse_results(text):
    """The cases of one run of tb_bench: (module, case, variant) -> nanoseconds."""
    results = {}
    for line in text.splitlines():
        fields = line.split('\t')
        if len(fields) != 4:
            continue
        try:
            results[tuple(fields[:3])] = float(fields[3])
        except ValueError:
            continue
    return results


def keep_fastest(into, results):
    for case, nanoseconds in results.items():
        if case not in into or nanoseconds < into[case]:
            into[case] = nanoseconds


def compare(before, after, margin):
    """The cases that differ, as (kind, case, before, after, ratio), the worst first."""
    rows = []
    for case in sorted(set(before) | set(after)):
        old = before.get(case)
        new = after.get(case)
        if old is None:
            rows.append(('new', case, None, new, None))
        elif new is None:
            rows.append(('gone', case, old, None, None))
        elif old > 0 and new / old > margin:
            rows.append(('slower', case, old, new, new / old))
        elif new > 0 and old / new > margin:
            rows.append(('faster', case, old, new, new / old))
    order = {'slower': 0, 'gone': 1, 'faster': 2, 'new': 3}
    rows.sort(key=lambda row: (order[row[0]], -(row[4] or 0) if row[0] == 'slower' else (row[4] or 0),
                               row[1]))
    return rows


def number(value):
    return '' if value is None else f'{value:.2f}'


def describe(rows, compared, margin):
    """The comparison as lines of text and as lines of Markdown."""
    counts = {kind: sum(1 for row in rows if row[0] == kind)
              for kind in ('slower', 'faster', 'new', 'gone')}
    headline = (f'{compared} cases compared: {counts["slower"]} slower and {counts["faster"]} '
                f'faster by more than {(margin - 1) * 100:.0f}%, {counts["new"]} new, '
                f'{counts["gone"]} gone.')
    text = [headline]
    markdown = ['### Benchmarks against the base', '', headline, '']
    if rows:
        markdown += ['| | Module | Case | Who | Before, ns | After, ns | After / before |',
                     '| --- | --- | --- | --- | ---: | ---: | ---: |']
    for kind, (module, case, variant), old, new, ratio in rows:
        change = '' if ratio is None else f'{ratio:.2f}'
        text.append(f'{kind:7} {module}: {case} [{variant}]  {number(old)} -> {number(new)}  '
                    f'{change}'.rstrip())
        markdown.append(f'| {kind} | {module} | {case} | {variant} | {number(old)} | '
                        f'{number(new)} | {change} |')
    return text, markdown


def run(program, repeats, modules):
    done = subprocess.run([program, '--repeats', str(repeats), *modules], capture_output=True,
                          text=True, errors='replace')
    if done.returncode != 0:
        sys.exit(f'{program} ended with status {done.returncode}:\n{done.stdout}{done.stderr}')
    return parse_results(done.stdout)


def measure(before_program, after_program, rounds, repeats, modules):
    before = {}
    after = {}
    for _ in range(rounds):
        keep_fastest(before, run(before_program, repeats, modules))
        keep_fastest(after, run(after_program, repeats, modules))
    return before, after


def self_test():
    first = 'table\tfind a key\tbadger\t5.50\nfmt\ta log line\tbadger\t100.00\n'
    second = ('table\tfind a key\tbadger\t5.00\nfmt\ta log line\tbadger\t130.00\n'
              'tb_bench: 2 cases, checksum 1f\nfmt\tnot a number\tbadger\tfast\n')
    fastest = {}
    keep_fastest(fastest, parse_results(first))
    keep_fastest(fastest, parse_results(second))
    table = ('table', 'find a key', 'badger')
    line = ('fmt', 'a log line', 'badger')
    old = {table: 10.0, line: 100.0, ('ring', 'push', 'badger'): 3.0, ('bit', 'set', 'badger'): 2.0,
           ('hash', 'bytes', 'badger'): 8.0}
    new = {table: 14.0, line: 60.0, ('ring', 'push', 'badger'): 3.6, ('atom', 'new', 'badger'): 9.0,
           ('hash', 'bytes', 'badger'): 30.0}
    rows = compare(old, new, 1.25)
    answers = [
        (parse_results(second), {table: 5.0, line: 130.0}),
        (fastest, {table: 5.0, line: 100.0}),
        ([(row[0], row[1][0]) for row in rows],
         [('slower', 'hash'), ('slower', 'table'), ('gone', 'bit'), ('faster', 'fmt'),
          ('new', 'atom')]),
        (compare({table: 10.0}, {table: 12.5}, 1.25), []),
        (compare({table: 10.0}, {table: 8.0}, 1.25), []),
        (compare({table: 0.0}, {table: 0.0}, 1.25), []),
        (describe(rows, 5, 1.25)[0][0],
         '5 cases compared: 2 slower and 1 faster by more than 25%, 1 new, 1 gone.'),
        (describe(rows, 5, 1.25)[1][-1], '| new | atom | new | badger |  | 9.00 |  |'),
    ]
    wrong = [(actual, wanted) for actual, wanted in answers if actual != wanted]
    for actual, wanted in wrong:
        print(f'self-test: got {actual!r}, expected {wanted!r}')
    return wrong


def take(arguments, name, default, convert):
    """Removes `name VALUE` from the arguments and returns the value."""
    if name not in arguments:
        return default
    at = arguments.index(name)
    value = convert(arguments[at + 1])
    del arguments[at:at + 2]
    return value


def main():
    arguments = sys.argv[1:]
    if arguments == ['--self-test']:
        if self_test():
            sys.exit(1)
        print('The benchmark comparison gives the known answers.')
        return
    margin = take(arguments, '--margin', DEFAULT_MARGIN, float)
    fail_above = take(arguments, '--fail-above', None, float)
    summary = take(arguments, '--summary', None, Path)
    rounds = take(arguments, '--rounds', DEFAULT_ROUNDS, int)
    repeats = take(arguments, '--repeats', DEFAULT_REPEATS, int)
    before_program = take(arguments, '--before', None, str)
    after_program = take(arguments, '--after', None, str)
    if before_program and after_program:
        before, after = measure(before_program, after_program, rounds, repeats, arguments)
    elif len(arguments) == 3 and arguments[0] == '--results':
        before = parse_results(Path(arguments[1]).read_text(encoding='utf-8'))
        after = parse_results(Path(arguments[2]).read_text(encoding='utf-8'))
    else:
        sys.exit(__doc__)
    rows = compare(before, after, margin)
    text, markdown = describe(rows, len(set(before) & set(after)), margin)
    print('\n'.join(text))
    if summary is not None:
        with open(summary, 'a', encoding='utf-8') as file:
            file.write('\n'.join(markdown) + '\n')
    if fail_above is not None and any(row[0] == 'slower' and row[4] > fail_above for row in rows):
        sys.exit(f'A case is slower than before by more than {(fail_above - 1) * 100:.0f}%.')


main()
