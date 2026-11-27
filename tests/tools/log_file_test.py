"""Run a program with --log-file and check that its output went to the file.

    python log_file_test.py <program> <expected text> [arguments...]

A game run from a launcher, or under Proton, has no standard output or error
that anything reads; --log-file is how its log is read there. The program
runs with its arguments after `--log-file <file>`, must succeed, must write
nothing to its own standard streams, and must write the expected text to the
file. A file in a directory that does not exist must be refused, and said so.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

program, expected = sys.argv[1], sys.argv[2].encode()
arguments = sys.argv[3:]
with tempfile.TemporaryDirectory(prefix='tb-log-file-') as temporary:
    log = Path(temporary) / 'output.log'
    result = subprocess.run([program, '--log-file', str(log), *arguments], capture_output=True,
                            timeout=120)
    written = log.read_bytes() if log.exists() else b''
    assert result.returncode == 0, (result, written)
    assert expected in written, written
    assert result.stdout == b'' and result.stderr == b'', result

    missing = Path(temporary) / 'missing' / 'output.log'
    result = subprocess.run([program, '--log-file', str(missing), *arguments],
                            capture_output=True, timeout=120)
    assert result.returncode != 0, result
    assert b'--log-file' in result.stderr, result
print('The output went to the log file, and a file that cannot be made was refused.')
