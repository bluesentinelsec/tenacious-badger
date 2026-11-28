"""Exercise Burrow's cooked shaders or read back a textured sandbox frame on Metal."""
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def draw(executable, arguments, directory):
    process = subprocess.run([executable, *arguments], cwd=directory, capture_output=True,
                             text=True, timeout=90)
    output = process.stdout + process.stderr
    if process.returncode:
        if any(reason in output for reason in ('no window:', 'no windowing:',
                                               'Metal: no system default GPU')):
            print(output)
            sys.exit(77)
        raise RuntimeError(output)
    if 'could not be made' in output:
        raise RuntimeError(output)
    return output


with tempfile.TemporaryDirectory(prefix='badger-metal-') as temporary:
    directory = Path(temporary)
    options = ['--set', 'display.window_mode=windowed', '--set', 'display.window_width=640',
               '--set', 'display.window_height=360']
    if sys.argv[1] == 'burrow':
        for diagnostic in ('--recreate-renderer-on-pause', '--lose-device-on-pause'):
            output = draw(sys.argv[2], ['--renderer=metal', '--frames=6', '--navigate=accept,back,wait,accept',
                                        diagnostic,
                                        '--settings-file', str(directory / 'settings.cfg'), '--set',
                                        'shaders.runtime_compile=false', '--set',
                                        'gameplay.pause_when_unfocused=false', *options], directory)
            for line in ('drawing with metal on ', 'menu: main', 'menu: playing', 'menu: pause'):
                if line not in output:
                    raise AssertionError(output)
            hashes = re.search(r'paused frame ([0-9a-f]{16}) -> ([0-9a-f]{16})', output)
            if not hashes or hashes[1] != hashes[2] or output.count('menu: playing') != 2:
                raise AssertionError(output)
            if diagnostic == '--lose-device-on-pause' and 'recovered graphics device from CPU scene data' not in output:
                raise AssertionError(output)
    else:
        capture = directory / 'checker.bmp'
        output = draw(sys.argv[2], ['--renderer', 'metal', '--scene', 'textured',
                                    '--frames', '3', '--capture', str(capture), '--set',
                                    'shaders.runtime_compile=true', *options], directory)
        bmp = capture.read_bytes()
        offset, = struct.unpack_from('<I', bmp, 10)
        width, height = struct.unpack_from('<ii', bmp, 18)
        height = -height
        if width < 3 or height < 3 or len(bmp) != offset + width * height * 4:
            raise AssertionError('Incomplete BGRA capture')
        # The center of the rotating square contains alternating light gray
        # checks and colored checks. Sampling here excludes the HUD and clear.
        light = colored = count = 0
        for y in range(height // 3, 2 * height // 3, 4):
            for x in range(width // 3, 2 * width // 3, 4):
                start = offset + (y * width + x) * 4
                blue, green, red = bmp[start:start + 3]
                light += min(red, green, blue) > 170 and max(red, green, blue) - min(red, green, blue) < 8
                colored += max(red, blue) > green + 16
                count += 1
        if min(light, colored) < count // 10:
            raise AssertionError(f'Missing checkerboard in frame: {light} light, {colored} colored')
