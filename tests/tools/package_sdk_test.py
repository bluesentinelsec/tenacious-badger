"""The SDK verifier must reuse its running interpreter, regardless of PATH."""
import os
from pathlib import Path
import platform
import sys
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import package_sdk


def main():
    with tempfile.TemporaryDirectory(prefix='tb-python-') as temporary:
        root = Path(temporary)
        prefix = root / 'sdk'
        headers = prefix / 'include/badger'
        headers.mkdir(parents=True)
        for name in ['api.h', 'assert.h', 'core.h', 'memory.h', 'platform.h',
                     'thread.h', 'thread_storage.h', 'version.h']:
            (headers / name).touch()
        config = prefix / 'lib/cmake/TenaciousBadger'
        config.mkdir(parents=True)
        (config / 'TenaciousBadgerConfig.cmake').touch()
        library = 'badger_static.lib' if platform.system() == 'Windows' else 'libbadger_static.a'
        (prefix / 'lib' / library).touch()
        (prefix / 'NOTICE').write_text('bluesentinelsec')
        smoke = root / 'tests/ffi'
        smoke.mkdir(parents=True)
        marker = root / 'interpreter.txt'
        (smoke / 'ctypes_smoke.py').write_text(
            'import sys\nfrom pathlib import Path\n'
            f'Path({str(marker)!r}).write_text(sys.executable)\n')
        # Launch a real child process with no interpreter discoverable on PATH.
        # Native signing, font licenses, and SDK contents are covered by the
        # packaging CI jobs.
        with patch.object(package_sdk, 'ROOT', root), \
                patch.object(package_sdk, 'verify_windows_signatures'),                 patch.object(package_sdk, 'verify_font_licenses'), \
                patch.dict(os.environ, {'PATH': str(root / 'empty-path')}):
            package_sdk.verify(prefix, {'publisher': 'bluesentinelsec'})
        assert Path(marker.read_text()).samefile(sys.executable)


if __name__ == '__main__':
    main()
