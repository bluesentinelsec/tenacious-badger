"""Make each standard stream refuse writes and check the engine notices."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='tb-stream-failure-') as temporary:
    destination = Path(temporary) / 'read-only.txt'
    destination.write_bytes(b'unchanged')
    for stream in ['stdout', 'stderr']:
        with destination.open('rb') as refused:
            options = {'stdout': subprocess.PIPE, 'stderr': subprocess.PIPE}
            options[stream] = refused
            result = subprocess.run(
                [sys.argv[1], f'fmt_failure_probe.{stream}_refuses_output'],
                capture_output=False, timeout=30, **options)
        report = result.stderr if stream == 'stdout' else result.stdout
        assert result.returncode == 0, (stream, result.returncode, report)
        assert f'{stream}-failure-detected'.encode() in report, report
        assert destination.read_bytes() == b'unchanged'
