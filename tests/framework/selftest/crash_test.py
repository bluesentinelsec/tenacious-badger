"""A case identity must accompany a native crash without changing its status."""
import re
import signal
import subprocess
import sys

probe = sys.argv[1]
sanitized = len(sys.argv) > 2

def run(*arguments):
    return subprocess.run([probe, *arguments], capture_output=True, timeout=20)

for case, codes in (
        ('aborts', {-int(signal.SIGABRT), 3}),
        ('invalid_access', {-int(signal.SIGSEGV), -int(getattr(signal, 'SIGBUS', signal.SIGSEGV)),
                            0xC0000005, -1073741819}),
        ('arithmetic_signal', {-int(signal.SIGFPE), 0xC0000094, -1073741676})):
    result = run('crash_fixture.a_passes', f'crash_fixture.{case}')
    assert result.returncode != 0, result
    assert re.search(rb'test case: crash_fixture\.' + case.encode() +
                     rb' \([^\n]*crash_tests\.c:[0-9]+\)', result.stderr), result
    if not sanitized:
        assert result.returncode in codes, result
        assert b'test case: crash_fixture.a_passes' not in result.stderr, result
    elif case == 'invalid_access':
        assert b'AddressSanitizer' in result.stderr, result

result = run('crash_fixture.a_passes')
assert result.returncode == 0, result
if not sanitized:
    assert b'test case:' not in result.stderr, result
    # --break retains native fault status and does not install the reporter.
    result = run('--break', 'crash_fixture.invalid_access')
    assert result.returncode in {0xC0000005, -1073741819, -int(signal.SIGSEGV),
                                 -int(getattr(signal, 'SIGBUS', signal.SIGSEGV))}, result
    assert b'test case:' not in result.stderr, result
    # An embedder's handler remains in control, and a completed run leaves none
    # of our reporter installed (nor a dangling pointer to the previous case).
    result = run('--existing-handler', 'crash_fixture.aborts')
    assert result.returncode == 77, result
    assert b'test case:' not in result.stderr, result
    result = run('--after-run', 'crash_fixture.a_passes')
    assert result.returncode in {-int(signal.SIGABRT), 3}, result
    assert b'test case:' not in result.stderr, result
print('Crash identity, native status, and handler ownership verified.')
