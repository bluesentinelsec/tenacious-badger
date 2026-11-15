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
    library.tb_memory_allocation_count.argtypes = []
    library.tb_memory_allocation_count.restype = ctypes.c_size_t
    library.tb_calloc.argtypes = [ctypes.c_size_t, ctypes.c_size_t]
    library.tb_calloc.restype = ctypes.c_void_p
    library.tb_free.argtypes = [ctypes.c_void_p]
    library.tb_free.restype = None
    blocks_before = library.tb_memory_allocation_count()
    block = library.tb_calloc(16, 4)
    if not block or ctypes.string_at(block, 64) != bytes(64):
        raise RuntimeError('External allocation did not return zero-filled memory')
    if library.tb_memory_allocation_count() != blocks_before + 1:
        raise RuntimeError('External allocation was not counted')
    library.tb_free(block)
    if library.tb_memory_allocation_count() != blocks_before:
        raise RuntimeError('External free was not counted')
    size_max = ctypes.c_size_t(-1).value
    for name, left, right, expected in [('tb_size_add', size_max - 1, 1, size_max),
                                        ('tb_size_add', size_max, 1, None),
                                        ('tb_size_multiply', size_max // 3, 3, size_max),
                                        ('tb_size_multiply', size_max // 2 + 1, 2, None)]:
        function = getattr(library, name)
        function.argtypes = [ctypes.c_size_t, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        function.restype = ctypes.c_bool
        output = ctypes.c_size_t(123)
        succeeded = function(left, right, ctypes.byref(output))
        if succeeded != (expected is not None) or output.value != (expected or 123):
            raise RuntimeError(f'External {name} overflow contract failed')
    library.tb_arena_create.argtypes = [ctypes.c_size_t]
    library.tb_arena_create.restype = ctypes.c_void_p
    library.tb_arena_calloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t,
                                        ctypes.c_size_t]
    library.tb_arena_calloc.restype = ctypes.c_void_p
    library.tb_arena_reset.argtypes = [ctypes.c_void_p]
    library.tb_arena_reset.restype = None
    library.tb_arena_destroy.argtypes = [ctypes.c_void_p]
    library.tb_arena_destroy.restype = None
    arena = library.tb_arena_create(4096)
    first = library.tb_arena_calloc(arena, 16, 4, 64)
    if not first or first % 64 != 0 or ctypes.string_at(first, 64) != bytes(64):
        raise RuntimeError('External arena allocation was not aligned and zero-filled')
    library.tb_arena_reset(arena)
    if library.tb_arena_calloc(arena, 16, 4, 64) != first:
        raise RuntimeError('External arena reset did not recycle its memory')
    library.tb_arena_destroy(arena)
    if library.tb_memory_allocation_count() != blocks_before:
        raise RuntimeError('External arena destruction did not release its memory')
    library.tb_platform_clock_nanoseconds.argtypes = []
    library.tb_platform_clock_nanoseconds.restype = ctypes.c_uint64
    first = library.tb_platform_clock_nanoseconds()
    if library.tb_platform_clock_nanoseconds() < first:
        raise RuntimeError('External platform clock invocation failed')
    library.tb_thread_current_id.argtypes = []
    library.tb_thread_current_id.restype = ctypes.c_uint64
    if library.tb_thread_current_id() == 0:
        raise RuntimeError('External thread identifier invocation failed')
    print(f'Python ctypes successfully invoked the installed C ABI: {path.name}')


if __name__ == '__main__':
    main()
