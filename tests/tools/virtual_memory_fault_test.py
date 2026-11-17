"""A successful setup followed by the exact native memory fault is required."""
import subprocess
import sys

for case in ('reserved_page', 'decommitted_page'):
    completed = subprocess.run([sys.argv[1], f'platform_memory_fault.{case}'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               timeout=20)
    # SIGSEGV/SIGBUS on POSIX; STATUS_ACCESS_VIOLATION on Windows.
    assert completed.returncode in (-11, -7, 0xC0000005, -1073741819), completed
    assert b'touching inaccessible page' in completed.stderr, completed
print('Reserved and decommitted pages fault after successful setup.')
