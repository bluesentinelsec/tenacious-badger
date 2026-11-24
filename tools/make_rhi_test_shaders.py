"""Compile the shaders that tests/rhi/rhi_d3d12_test.c draws with to DXIL,
with the Windows SDK's dxc.

    python tools/make_rhi_test_shaders.py [--dxc PATH]

Each tests/data/rhi/NAME.hlsl has entry points MainVS and MainPS, or MainCS;
each entry point becomes NAME.vs.dxil, NAME.ps.dxil or NAME.cs.dxil beside
it, for Shader Model 6.6. The SDK's dxc signs what it writes with dxil.dll,
which D3D12 requires. This is run at development time on Windows; the DXIL is
committed, so the tests need no compiler. When the engine compiles shaders
itself, the tests will compile these at run time instead.
"""
import argparse
import glob
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / 'tests' / 'data' / 'rhi'
ENTRIES = {'MainVS': ('vs', 'vs_6_6'), 'MainPS': ('ps', 'ps_6_6'), 'MainCS': ('cs', 'cs_6_6')}


def find_dxc():
    """The newest dxc of the Windows SDK."""
    pattern = r'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\dxc.exe'
    found = sorted(glob.glob(pattern))
    return found[-1] if found else None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--dxc', default=find_dxc())
    arguments = parser.parse_args()
    if arguments.dxc is None:
        sys.exit('no dxc: install the Windows SDK, or name one with --dxc')
    for source in sorted(DATA.glob('*.hlsl')):
        text = source.read_text(encoding='utf-8')
        for entry, (suffix, profile) in ENTRIES.items():
            if not re.search(r'\b' + entry + r'\s*\(', text):
                continue
            output = source.with_suffix('.' + suffix + '.dxil')
            subprocess.run([arguments.dxc, '-nologo', '-T', profile, '-E', entry, '-HV', '2021',
                            '-Qstrip_debug', '-Qstrip_reflect', '-Fo', str(output), str(source)],
                           check=True)
            print(output.relative_to(ROOT))


if __name__ == '__main__':
    main()
