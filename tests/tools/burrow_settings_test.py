"""Check Burrow's settings from its command line: the layers, --set,
--renderer, --reset-settings, --mode, and problems in a settings file.

    python burrow_settings_test.py <burrow executable>

Every run names its own settings file, in a temporary directory, so the test
never reads or moves the settings of the user who runs it.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

BURROW = sys.argv[1]


def run(*arguments):
    result = subprocess.run([BURROW, *arguments], capture_output=True, timeout=60)
    return result.returncode, result.stdout.decode('utf-8'), result.stderr.decode('utf-8')


def expect(condition, message, output):
    if not condition:
        raise AssertionError(f'{message}\n--- output:\n{output}')


with tempfile.TemporaryDirectory(prefix='tb-burrow-settings-') as temporary:
    player = Path(temporary) / 'settings.cfg'
    common = ['--settings-file', str(player)]

    # The layers: the engine's built-in values, Burrow's defaults, then the
    # command line; a developer build starts in a window.
    status, out, err = run(*common, '--mode', 'developer', '--set', 'display.frame_limit=144',
                           'settings')
    expect(status == 0, f'status {status}', err)
    expect('display.window_mode = windowed  # built-in' in out, 'developer window mode', out)
    expect('developer.quit_without_asking = true  # built-in' in out, 'developer escape', out)
    expect('engine.tick_rate = 60  # default file' in out, "Burrow's defaults", out)
    expect('display.frame_limit = 144  # command line' in out, '--set', out)
    expect(str(player) in out, "the player's file is named", out)

    # A release build starts borderless and asks before quitting.
    status, out, err = run(*common, '--mode', 'release', 'settings')
    expect(status == 0, f'status {status}', err)
    expect('display.window_mode = borderless  # built-in' in out, 'release window mode', out)
    expect('developer.quit_without_asking = false  # built-in' in out, 'release escape', out)

    # The player's file loads, and its problems are reported, not fatal.
    player.write_text('audio.master_volume = 0.5\nnot a setting line\n', encoding='utf-8')
    status, out, err = run(*common, 'settings')
    expect(status == 0, f'status {status}', err)
    expect('audio.master_volume = 0.5  # player file' in out, "the player's file", out)
    expect("line 2: expected 'name = value'" in err, 'the problem is reported', err)

    # The command line overrides the player's file.
    status, out, err = run(*common, '--set', 'audio.master_volume=0.25', 'settings')
    expect('audio.master_volume = 0.25  # command line' in out, 'the command line wins', out)

    # Wrong --set values are usage errors.
    for value, message in [('nope.x=1', 'no setting has the name'),
                           ('display.frame_limit=fast', 'does not fit'),
                           ('display.frame_limit', 'expected NAME=VALUE')]:
        status, out, err = run(*common, '--set', value, 'settings')
        expect(status == 2, f'--set {value}: status {status}', err)
        expect(message in err, f'--set {value}: message', err)

    # --renderer is --set graphics.renderer, after the --set values; a renderer
    # the platform does not build is still a value, which the RHI reports.
    status, out, err = run(*common, '--renderer=metal', 'settings')
    expect(status == 0, f'--renderer: status {status}', err)
    expect('graphics.renderer = metal  # command line' in out, '--renderer', out)
    status, out, err = run(*common, '--set', 'graphics.renderer=null', '--renderer', 'd3d12',
                           'settings')
    expect('graphics.renderer = d3d12  # command line' in out, '--renderer wins', out)
    status, out, err = run(*common, 'settings')
    expect('graphics.renderer = auto  # built-in' in out, 'auto by default', out)
    status, out, err = run(*common, '--renderer=directx', 'settings')
    expect(status == 2, f'--renderer=directx: status {status}', err)
    expect('--renderer=directx: not a renderer; the renderers are auto, d3d12, d3d11, vulkan, '
           'opengl, metal, software, null' in err, '--renderer=directx: message', err)

    # Resetting sets the file aside and starts from the defaults.
    status, out, err = run(*common, '--reset-settings', 'settings')
    expect(status == 0, f'status {status}', err)
    expect('audio.master_volume = 1.0  # built-in' in out, 'the defaults after a reset', out)
    expect(not player.exists(), 'the file is set aside', err)
    backup = Path(str(player) + '.bak')
    expect(backup.read_text(encoding='utf-8').startswith('audio.master_volume'), 'the backup',
           err)

    # Checking that the game starts loads the same settings.
    status, out, err = run(*common, '--set', 'display.vsync=off', 'check')
    expect(status == 0, f'status {status}', err)
    expect('2 + 3 = 5' in out, 'the game starts', out)

    # The game runs without a window for as many frames as asked.
    status, out, err = run(*common, '--headless', '--set', 'developer.frame_count=30',
                           '--set', 'display.frame_limit=0')
    expect(status == 0, f'headless: status {status}', err)

    # In a window where there is a display, and with an error saying how to
    # run without one where there is not.
    status, out, err = run(*common, '--set', 'developer.frame_count=10',
                           '--set', 'display.window_mode=windowed', '--renderer=null')
    expect(status == 0 or 'no window' in err, f'windowed: status {status}', err)
