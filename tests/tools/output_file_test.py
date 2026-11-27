"""Check that tb_process_write_output_to() sends both standard streams to a file.

    python output_file_test.py <tb_probe executable>

A game run from a launcher, or under Proton, has no standard output or error
that anything reads, so its log is lost unless the streams go to a file.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

probe = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='tb-output-file-') as temporary:
    result = subprocess.run([probe, 'process_probe.both_streams_go_to_the_file'],
                            capture_output=True, cwd=temporary, timeout=20)
    assert result.returncode == 0, result
    written = (Path(temporary) / 'output.log').read_bytes().replace(b'\r\n', b'\n')
    expected = b'output 1\nerror 2\nnative error 3\noutput 4\n'
    assert expected in written, written
    for line in expected.splitlines():
        assert line not in result.stdout + result.stderr, result

with tempfile.TemporaryDirectory(prefix='tb-output-file-') as temporary:
    result = subprocess.run([probe, 'process_probe.a_file_that_cannot_be_made_moves_nothing'],
                            capture_output=True, cwd=temporary, timeout=20)
    assert result.returncode == 0, result
    assert b'still on standard output' in result.stdout, result
    assert b'still on standard error' in result.stderr, result
    assert not (Path(temporary) / 'missing').exists()
# A launcher can start a process with neither stream connected.
def close_output():
    os.close(1)
    os.close(2)


with tempfile.TemporaryDirectory(prefix='tb-detached-output-') as temporary:
    detached = ({'creationflags': subprocess.DETACHED_PROCESS} if os.name == 'nt'
                else {'preexec_fn': close_output})
    result = subprocess.run([probe, 'process_probe.both_streams_go_to_the_file'],
                            cwd=temporary, timeout=20, **detached)
    assert result.returncode == 0, result
    written = (Path(temporary) / 'output.log').read_bytes().replace(b'\r\n', b'\n')
    assert expected in written, written
print('Inherited and detached streams went to the log, and a refused file moved neither.')
