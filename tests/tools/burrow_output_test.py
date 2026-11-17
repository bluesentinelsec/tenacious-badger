"""Give burrow a standard output that refuses writes and check that it fails.

    python burrow_output_test.py <burrow executable>

A program that prints its result and exits with success when nothing could be
printed has lied to whatever ran it.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='tb-burrow-output-') as temporary:
    destination = Path(temporary) / 'read-only.txt'
    destination.write_bytes(b'unchanged')
    with destination.open('rb') as refused:
        result = subprocess.run([sys.argv[1]], stdout=refused, stderr=subprocess.PIPE, timeout=30)
    assert result.returncode != 0, 'burrow reported success though it could print nothing'
    assert destination.read_bytes() == b'unchanged'
    accepted = subprocess.run([sys.argv[1]], capture_output=True, timeout=30)
    assert accepted.returncode == 0, accepted.stderr
    assert b'2 + 3 = 5' in accepted.stdout, accepted.stdout
