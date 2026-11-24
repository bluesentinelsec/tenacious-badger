#!/usr/bin/env python3
"""Write, or check, what DXC reports for the shader reflection corpus.

Each shader of tests/data/shader_reflection/*.hlsl begins with the line

    // dxc: -T ps_6_6 -E main

which names the arguments DXC compiles it with. This tool compiles each with
the Windows SDK's dxc, reads the disassembly (`-Fc`), and writes beside the
shader, as NAME.dxc, the lines the engine's compiler must report too:

    binding NAME TYPE BIND COUNT        from the resource bindings
    input SEMANTIC INDEX FORMAT COLUMNS [INTERPOLATION]
    output SEMANTIC INDEX FORMAT COLUMNS
    buffer NAME SIZE                    from the buffer definitions
    variable BUFFER.PATH OFFSET
    feature NAME                        what the shader requires, of those
                                        the engine names

sorted, so the order of a section does not matter. The test
shader_compile.reflection_matches_dxc (tests/shader/shader_compile_test.c)
renders the engine's reflection the same way and compares.

    python tools/shader_reflection_goldens.py           write the goldens
    python tools/shader_reflection_goldens.py --check   fail if any differs

dxc is the newest of the Windows SDK's, or the one TB_DXC names. Without one,
--check fails only with --require-dxc, so the check runs where DXC is.
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

CORPUS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests', 'data',
                      'shader_reflection')

# DXC's names of what a shader requires, for the features the engine names.
FEATURES = {
    'Double-precision floating point': 'float64',
    'Wave level operations': 'wave',
    '64-Bit integer': 'int64',
    'Raytracing tier 1.1 features': 'rayquery',
}


def find_dxc():
    named = os.environ.get('TB_DXC')
    if named:
        return named if os.path.isfile(named) else None
    pattern = r'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\dxc.exe'

    def version(path):
        parts = path.split(os.sep)[-3].split('.')
        return tuple(int(part) for part in parts if part.isdigit())

    found = sorted(glob.glob(pattern), key=version)
    return found[-1] if found else None


def table_rows(lines, start):
    """The rows of the table whose header is at `start`: sliced by the
    columns its dashes mark, until a line that is only ';'."""
    dashes = lines[start + 1]
    columns = [(match.start(), match.end()) for match in re.finditer(r'-+', dashes)]
    rows = []
    for line in lines[start + 2:]:
        if line.strip() in (';', '') or 'no parameters' in line:
            break
        rows.append([line[first:end].strip() if first < len(line) else '' for first, end in
                     [(column[0], columns[index + 1][0] if index + 1 < len(columns) else len(line))
                      for index, column in enumerate(columns)]])
    return rows


def find_tables(lines, title, header):
    """The indices of each table's header line that follows a `title` line."""
    found = []
    for index, line in enumerate(lines):
        if line.strip() == '; ' + title:
            for at in range(index + 1, min(index + 4, len(lines))):
                if lines[at].startswith('; ' + header):
                    found.append(at)
                    break
    return found


# The words DXC writes for a signature element's format.
FORMATS = {'float', 'int', 'uint', 'bool', 'fp16', 'i16', 'u16', 'min16f', 'min16i', 'min16u',
           'double', 'i64', 'u64', 'min2_8f', 'min10f', 'unknown'}


def signature_rows(lines, header):
    """The rows of a signature table, read field by field: a name longer
    than its column, as `SV_RenderTargetArrayIndex`, pushes the rest of its
    row right, and a long register runs into the system value after it
    (`oStencilRefSTENCILREF`), so its columns cannot be sliced."""
    rows = []
    for line in lines[header + 2:]:
        if line.strip() in (';', '') or 'no parameters' in line:
            break
        match = re.match(r';\s(\S+)\s+(\d+)(.*)$', line)
        if match:
            rows.append(match.groups())
    return rows


def signature(lines, title, kind, interpolation):
    tables = find_tables(lines, title, 'Name                 Index   Mask')
    result = []
    if not tables:
        return result
    # DXC lists an array's or a matrix's mode once, at its first index; the
    # rows after it share it.
    modes = {}
    for header in find_tables(lines, title, 'Name                 Index             InterpMode'):
        for name, index, rest in signature_rows(lines, header):
            modes.setdefault(name, []).append((int(index), rest.strip()))
    for name, index, rest in signature_rows(lines, tables[0]):
        # The mask is 4 components wide, after 3 characters of padding: `  z `,
        # or ` N/A` for a system value passed in no register.
        mask = rest[3:7]
        letters = [letter for letter in mask if letter in 'xyzw']
        fields = rest[7:].split()
        form = next((field for field in fields if field in FORMATS), '')
        register = re.match(r'\d*', fields[0]).group(0) if fields else ''
        # Where it is passed: `r1.z`, its register and first component, or
        # `-` for a system value in none (`special`, `oDepth`, `primID`).
        where = 'r%s.%s' % (register, letters[0]) if register and letters else '-'
        # DXC gives one in no register no components: its width is not compared.
        width = str(len(letters)) if where != '-' else '-'
        line = '%s %s %s %s %s %s' % (kind, name, index, form, width, where)
        if interpolation:
            starts = [mode for first, mode in sorted(modes.get(name, [])) if first <= int(index)]
            line += ' ' + (starts[-1] if starts else '')
        result.append(line.rstrip())
    return result


def bindings(lines):
    tables = find_tables(lines, 'Resource Bindings:', 'Name')
    result = []
    for header in tables:
        # Its values are wider than its dashes, and hold no spaces: split them.
        for line in lines[header + 2:]:
            fields = line.lstrip(';').split()
            # DXC writes an unbounded count against the binding: `t0,space1unbounded`.
            if len(fields) == 6 and fields[5].endswith('unbounded'):
                fields = fields[:5] + [fields[5][:-len('unbounded')], 'unbounded']
            if len(fields) < 7:
                break
            name, kind, _form, _dimension, _id, bind, count = fields[:7]
            result.append('binding %s %s %s %s' % (name, kind, bind, count))
    return result


def features(lines):
    result = []
    for index, line in enumerate(lines):
        if 'shader requires additional functionality' in line:
            for item in lines[index + 1:]:
                name = item.lstrip('; ').strip()
                if name == '':
                    break
                if name in FEATURES:
                    result.append('feature ' + FEATURES[name])
    return result


def reflect(dxc, shader):
    with open(shader, encoding='utf-8') as source:
        first = source.readline()
    arguments = re.match(r'//\s*dxc:\s*(.*)', first)
    if not arguments:
        raise SystemExit('%s: the first line must be "// dxc: ARGUMENTS"' % shader)
    with tempfile.TemporaryDirectory() as directory:
        listing = os.path.join(directory, 'listing.txt')
        command = [dxc] + arguments.group(1).split() + [shader, '-Fc', listing]
        subprocess.run(command, check=True, capture_output=True)
        with open(listing, encoding='utf-8', errors='replace') as text:
            lines = text.read().splitlines()
    stage = arguments.group(1).split('-T')[1].split()[0][:2]
    report = (bindings(lines) + signature(lines, 'Input signature:', 'input', stage == 'ps') +
              signature(lines, 'Output signature:', 'output', False) + nested_buffers(lines) +
              features(lines))
    return '\n'.join(sorted(report)) + '\n'


def nested_buffers(lines):
    """Each cbuffer's size and its variables' offsets, struct members by
    their path: DXC opens a struct with `struct X {` and closes it with
    `} name; ; Offset: N`, so members are named when their struct closes."""
    start = next((index for index, line in enumerate(lines) if line.strip() == '; Buffer Definitions:'),
                 None)
    if start is None:
        return []
    result = []
    buffer = None
    stack = []  # Each open struct: the lines of its members, as (path, offset).
    for line in lines[start + 1:]:
        text = line[1:].strip() if line.startswith(';') else line.strip()
        if text.startswith('Resource Bindings:'):
            break
        opened = re.match(r'cbuffer (\S+)$', text)
        if opened:
            buffer = opened.group(1)
            stack = []
            continue
        if buffer is None:
            continue
        if text.startswith('struct ') and not text.endswith(';'):
            stack.append([])
            continue
        closed = re.match(r'\}\s*([^;\s]+);+\s*; Offset:\s*(\d+)(?:\s+Size:\s*(\d+))?', text)
        if closed:
            members = stack.pop()
            name = re.sub(r'\[.*\]', '', closed.group(1))
            if not stack:
                result.append('buffer %s %s' % (buffer, closed.group(3)))
                result.extend('variable %s.%s %s' % (buffer, path, offset) for path, offset in members)
                buffer = None
                continue
            stack[-1].append((name, closed.group(2)))
            stack[-1].extend(('%s.%s' % (name, path), offset) for path, offset in members)
            continue
        variable = re.match(r'.*?([A-Za-z_$][A-Za-z0-9_]*)(\[\d+\])?;\s*; Offset:\s*(\d+)', text)
        if variable and stack:
            stack[-1].append((variable.group(1), variable.group(3)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--check', action='store_true', help='fail if a golden differs')
    parser.add_argument('--require-dxc', action='store_true', help='fail when there is no dxc')
    options = parser.parse_args()
    dxc = find_dxc()
    if dxc is None:
        print('shader_reflection_goldens: no dxc found; set TB_DXC to one')
        return 1 if options.require_dxc else 0
    differ = []
    for shader in sorted(glob.glob(os.path.join(CORPUS, '*.hlsl'))):
        golden = os.path.splitext(shader)[0] + '.dxc'
        report = reflect(dxc, shader)
        if options.check:
            current = open(golden, encoding='utf-8').read() if os.path.isfile(golden) else None
            if current != report:
                differ.append(os.path.basename(golden))
        else:
            with open(golden, 'w', encoding='utf-8', newline='\n') as out:
                out.write(report)
    if differ:
        print('these goldens differ from what %s reports: %s' % (dxc, ', '.join(differ)))
        print('run python tools/shader_reflection_goldens.py, and review the difference')
        return 1
    print('%s goldens of %s' % ('checked' if options.check else 'wrote', dxc))
    return 0


if __name__ == '__main__':
    sys.exit(main())
