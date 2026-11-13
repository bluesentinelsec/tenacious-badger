"""Call the installed shared library from Python without headers or a C wrapper."""
import argparse
import ctypes
from pathlib import Path
import platform


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path, required=True)
    arguments = parser.parse_args()
    relative = {'Windows': 'bin/badger.dll', 'Darwin': 'lib/libbadger.dylib',
                'Linux': 'lib/libbadger.so'}[platform.system()]
    path = (arguments.prefix / relative).resolve(strict=True)
    library = ctypes.CDLL(str(path))
    library.tb_sum_i32.argtypes = [ctypes.c_int32, ctypes.c_int32, ctypes.POINTER(ctypes.c_int32)]
    library.tb_sum_i32.restype = ctypes.c_bool
    for left, right, expected in [(2, 3, 5), (-2, -3, -5), (2147483647, 0, 2147483647),
                                  (-2147483648, 2147483647, -1)]:
        output = ctypes.c_int32(0)
        if not library.tb_sum_i32(left, right, ctypes.byref(output)) or output.value != expected:
            raise RuntimeError('External C ABI sum invocation failed')
    for left, right in [(2147483647, 1), (-2147483648, -1)]:
        output = ctypes.c_int32(123)
        if library.tb_sum_i32(left, right, ctypes.byref(output)) or output.value != 123:
            raise RuntimeError('External C ABI overflow contract failed')
    library.tb_assert_library_enabled.argtypes = []
    library.tb_assert_library_enabled.restype = ctypes.c_bool
    if library.tb_assert_library_enabled() not in (True, False):
        raise RuntimeError('External assertion-setting query failed')
    library.tb_platform_ticks.argtypes = []
    library.tb_platform_ticks.restype = ctypes.c_uint64
    if platform.system() == 'Windows':
        first = library.tb_platform_ticks()
        if library.tb_platform_ticks() < first:
            raise RuntimeError('External platform time invocation failed')
    print(f'Python ctypes successfully invoked the installed C ABI: {path.name}')


if __name__ == '__main__':
    main()
