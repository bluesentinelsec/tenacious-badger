"""Check native stderr output/failure separately from a broken stdout stream."""
import subprocess
import sys
import tempfile
from pathlib import Path
probe = sys.argv[1]
result = subprocess.run([probe, 'log_probe.stderr_record'], capture_output=True, timeout=20)
assert result.returncode == 0, result
assert result.stderr.replace(b'\r\n', b'\n').count(b'[INFO] [probe] value 42\n') == 1, result
with tempfile.TemporaryDirectory() as root:
    path = Path(root) / 'read-only.txt'
    path.write_bytes(b'untouched')
    with path.open('rb') as bad:
        result = subprocess.run([probe, 'log_probe.stderr_failure'], stdout=subprocess.PIPE,
                                stderr=bad, timeout=20)
    assert result.returncode == 0, result
    assert path.read_bytes() == b'untouched'
    with path.open('rb') as bad:
        result = subprocess.run([probe, 'log_probe.stderr_record'], stdout=bad,
                                stderr=subprocess.PIPE, timeout=20)
    assert result.returncode == 0, result
    assert b'[INFO] [probe] value 42\n' in result.stderr.replace(b'\r\n', b'\n'), result
print('Stderr sink output and isolated failure reporting verified.')
