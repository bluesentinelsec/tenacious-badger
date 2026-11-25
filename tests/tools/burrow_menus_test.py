"""Drive Burrow's menus by navigation alone, as a keyboard or a gamepad does.

    python burrow_menus_test.py <burrow executable>

Burrow runs in a window with the null renderer, which draws nothing, and is
given navigation actions one a frame with --navigate; it writes each screen
its menus show, which the test follows. The player's settings file is the
test's own, in a temporary directory. Where there is no display, as on a
headless CI runner, the test is skipped with exit status 77, which CTest
reports as skipped. The game does not pause when its window loses the
focus, which a window on a CI runner may never have, so that every run
follows its script.
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


def screens(output):
    prefix = 'burrow: menu: '
    return [line[len(prefix):] for line in output.splitlines() if line.startswith(prefix)]


with tempfile.TemporaryDirectory(prefix='tb-burrow-menus-') as temporary:
    player = Path(temporary) / 'settings.cfg'
    common = ['--settings-file', str(player), '--renderer=null', '--set',
              'display.window_mode=windowed', '--set', 'display.window_width=960', '--set',
              'display.window_height=540', '--set', 'developer.quit_without_asking=true',
              '--set', 'gameplay.pause_when_unfocused=false']

    # A word that is no action is refused before a window opens.
    status, out, err = run(*common, '--frames=3', '--navigate=down,jump')
    expect(status == 2, f'a wrong action: status {status}', err)
    expect("'jump' is not an action" in err, 'the wrong action is named', err)

    # Play, pause with Back, quit to the menu, and quit: Quit is the main
    # menu's last button, and Up goes around to it.
    status, out, err = run(*common, '--frames=60',
                           '--navigate=accept,back,down,down,accept,up,accept')
    if status != 0 and 'no window' in err:
        print(f'skipped: no display: {err.strip()}')
        sys.exit(SKIPPED)
    expect(status == 0, f'playing: status {status}', err)
    expect(screens(out) == ['main', 'playing', 'pause', 'main', 'quit'],
           'play, pause, quit to the menu, and quit', out)

    # The options from the pause menu go back to it.
    status, out, err = run(*common, '--frames=40', '--navigate=accept,back,down,accept,back')
    expect(status == 0, f'options from the pause menu: status {status}', err)
    expect(screens(out) == ['main', 'playing', 'pause', 'options', 'pause'],
           'the options go back to the pause menu', out)

    # The options: the Graphics tab by a shoulder, down into the field of
    # view, two steps right, and Back, which asks to keep the change: Yes,
    # the dialog's first button, applies it and saves the player's file.
    status, out, err = run(*common, '--frames=40',
                           '--navigate=down,accept,tab_next,down,right,right,back,accept')
    expect(status == 0, f'options: status {status}', err)
    expect(screens(out) == ['main', 'options', 'options applied', 'main'],
           'the change is applied on the way back', out)
    expect('graphics.field_of_view = 92' in player.read_text(), 'the change is saved',
           player.read_text())

    # No, the dialog's second button, lets go of the change.
    status, out, err = run(*common, '--frames=40',
                           '--navigate=down,accept,tab_next,down,right,back,right,accept')
    expect(status == 0, f'options let go: status {status}', err)
    expect(screens(out) == ['main', 'options', 'options reverted', 'main'],
           'the change is let go of on the way back', out)
    expect('graphics.field_of_view = 92' in player.read_text(), 'the file is as it was',
           player.read_text())

    # Back from the main menu asks to quit; Back again says no.
    status, out, err = run(*common, '--frames=20', '--navigate=back,back')
    expect(status == 0, f'not quitting: status {status}', err)
    expect(screens(out) == ['main'], 'Back from the main menu quits nothing by itself', out)
