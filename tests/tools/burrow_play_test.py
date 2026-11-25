"""Run Burrow's game for a few frames, the escape hatch tests and CI use.

    python burrow_play_test.py <burrow executable>

Without a window the game loop runs the player and stops after the frames
asked for. In a window, the null renderer takes every call the game makes to
draw the room and its HUD, on every platform, though it shows nothing. Where
there is no display, as on a headless CI runner, the windowed run is skipped
with exit status 77, which CTest reports as skipped. Every run names its own
settings file, in a temporary directory, so the test never reads or moves the
settings of the user who runs it.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

BURROW = sys.argv[1]
SKIPPED = 77


def run(*arguments):
    result = subprocess.run([BURROW, *arguments], capture_output=True, timeout=120)
    return result.returncode, result.stdout.decode('utf-8'), result.stderr.decode('utf-8')


def expect(condition, message, output):
    if not condition:
        raise AssertionError(f'{message}\n--- output:\n{output}')


with tempfile.TemporaryDirectory(prefix='tb-burrow-play-') as temporary:
    common = ['--settings-file', str(Path(temporary) / 'settings.cfg'), '--set',
              'gameplay.pause_when_unfocused=false']

    # --frames is the escape hatch: the same as developer.frame_count.
    status, out, err = run(*common, '--frames=3', 'settings')
    expect('developer.frame_count = 3  # command line' in out, '--frames sets the count', out)
    status, out, err = run(*common, '--frames=0')
    expect(status == 2, f'--frames=0: status {status}', err)

    # Without a window, the loop runs as many frames as asked, and stops.
    status, out, err = run(*common, '--headless', '--frames=30', '--set',
                           'display.frame_limit=0')
    expect(status == 0, f'headless: status {status}', err)

    # In a window, the room and the HUD go through the renderer: Play, the
    # main menu's first button, is pressed at the first frame.
    status, out, err = run(*common, '--frames=5', '--renderer=null', '--navigate=accept', '--set',
                           'display.window_mode=windowed', '--set', 'display.window_width=640',
                           '--set', 'display.window_height=360')
    if status != 0 and 'no window' in err:
        print(f'skipped: no display: {err.strip()}')
        sys.exit(SKIPPED)
    expect(status == 0, f'windowed: status {status}', err)
    expect('burrow: drawing with null' in out, 'the renderer is made', out)

    # Every way the HUD draws: the frame rate in detail and without a
    # crosshair, without the frame rate, and hidden altogether.
    for hud in (['interface.performance_overlay=detailed', 'interface.crosshair=false'],
                ['interface.performance_overlay=off'], ['interface.hud_opacity=0']):
        settings = [argument for value in hud for argument in ('--set', value)]
        status, out, err = run(*common, '--frames=3', '--renderer=null', '--navigate=accept',
                               '--set',
                               'display.window_mode=windowed', *settings)
        expect(status == 0, f'{hud}: status {status}', err)
