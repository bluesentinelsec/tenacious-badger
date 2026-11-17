"""Exercise a real external utility through filesystem effects and diagnostics."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

program = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='tb-foundation-') as temporary:
    root = Path(temporary)
    (root / 'données-漢字-🦡').mkdir()
    source = 'données-漢字-🦡/entrée.bin'
    destination = 'données-漢字-🦡/sortie.bin'
    request = root / 'request.txt'

    def invoke(payload=None, *limits, success=True, diagnostic=b'copied'):
        if payload is None:
            payload = f'{source}\n{destination}\n'.encode('utf-8')
        request.write_bytes(payload)
        result = subprocess.run([program, 'request.txt', *map(str, limits)], cwd=root,
                                capture_output=True, timeout=30)
        assert (result.returncode == 0) == success, result
        assert diagnostic in result.stderr, result
        return result

    for size in (0, 1, 4095, 4096, 4097, 65536, 1024 * 1024 + 3):
        data = bytes((index * 67 + 19) % 256 for index in range(size))
        (root / source).write_bytes(data)
        (root / destination).write_bytes(b'old output')
        invoke()
        assert (root / destination).read_bytes() == data
        assert not list((root / destination).parent.glob('tb-*'))
    # Both newline conventions and requests without a final newline are valid.
    invoke(f'{source}\r\n{destination}'.encode('utf-8'))
    # The source remains available if it is also the replacement destination.
    before = (root / source).read_bytes()
    invoke(f'{source}\n{source}\n'.encode('utf-8'))
    assert (root / source).read_bytes() == before
    sentinel = b'previous destination must survive failure'
    (root / destination).write_bytes(sentinel)
    invalid = [b'\xff\nx\n', b'x\0y\nz\n', b'\noutput\n', b'input\n\n',
               b'one-line', b'a\nb\nc\n', b'\xed\xa0\x80\nx\n', b'x' * 65537]
    for data in invalid:
        invoke(data, success=False, diagnostic=b'invalid or unreadable UTF-8 request')
        assert (root / destination).read_bytes() == sentinel
    invoke(f'missing.bin\n{destination}\n'.encode(), success=False, diagnostic=b'read failed')
    invoke(None, 10, success=False, diagnostic=b'read failed')
    # A deliberately tiny reservation cannot hold the file; the fallible arena
    # adapter reports failure and leaves the old destination intact.
    invoke(None, 2 * 1024 * 1024, 4096, success=False, diagnostic=b'read failed')
    invoke(None, 2 * 1024 * 1024, (1 << 64) - 1, success=False, diagnostic=b'reservation failed')
    invoke(None, '18446744073709551616', success=False, diagnostic=b'usage:')
    assert (root / destination).read_bytes() == sentinel
    invoke(f'{source}\nmissing-parent/output.bin\n'.encode(), success=False,
           diagnostic=b'replacement failed')
    assert not (root / 'missing-parent').exists()
    assert not list((root / destination).parent.glob('tb-*'))
    if os.name != 'nt':
        # A symlink followed by .. must keep native filesystem semantics.
        (root / 'real/child').mkdir(parents=True)
        (root / 'alias').symlink_to('real/child', target_is_directory=True)
        (root / 'real/input').write_bytes(b'native parent resolution')
        invoke(b'alias/../input\nalias/../output\n')
        assert (root / 'real/output').read_bytes() == b'native parent resolution'
        assert not (root / 'output').exists()
print('Installed foundation utility: UTF-8, paths, I/O, arenas, logging, and replacement verified.')
