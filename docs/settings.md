# Settings

A setting is a named, typed value that a player or a developer chooses: the
window mode, the master volume, the mouse sensitivity. `settings.h` holds them
in a registry that any subsystem or game extends, and `engine_settings.h`
defines the engine's own, in the families players know from the options menus
of AAA games.

Players change settings at almost any time: they pause, open the options
menu, change something, and carry on. So every setting can be read and
changed from any thread at any moment, and nothing waits for a restart.

## Where a value comes from

Each setting holds a value for each of five layers. Its value is that of the
highest layer that has one:

| Layer | Set by | Saved |
| --- | --- | --- |
| Runtime, highest | the game while it runs: the options menu, a console | yes |
| Command line | the command line the game started with, `--set name=value` | no |
| Player file | the player's settings file | yes |
| Default file | the game's own defaults, over the engine's | no |
| Built-in, lowest | the setting's definition | no |

So the game's defaults override the engine's, the player's choices override
the game's, a command line overrides both for one session, and a change made
while the game runs overrides everything.

The layers stay apart. Saving writes the player's choices, from the runtime
and player file layers, and never a value the command line gave for one
session: a developer who starts the game with `--set display.vsync=off` does
not find vsync off in the player's file afterwards.

## Using a setting

A subsystem reads its settings where it uses them, and applies changes at a
moment that suits it, such as the start of a frame. Each setting has a
version that grows whenever its value changes, and the registry has one that
grows whenever any value does:

```c
uint64_t applied = 0;

void audio_frame(const tb_engine_settings *engine)
{
    if (tb_setting_version(engine->audio.master_volume) != applied)
    {
        applied = tb_setting_version(engine->audio.master_volume);
        mixer_set_master_volume(tb_setting_number(engine->audio.master_volume));
    }
}
```

Nothing calls a subsystem back in the middle of its work, so a change from the
options menu, on the game thread, is safe for the audio thread to pick up on
its own time.

The options menu changes a setting with `tb_setting_set_boolean()` and the
others of its kind, which set the runtime layer and refuse a value that does
not fit, then calls `tb_settings_save_file()` when the player confirms. A menu
that shows a display change for a few seconds before keeping it saves only
once the player keeps it, so a mode the display cannot show is never saved.

## Adding settings

A game adds its own settings beside the engine's, to the engine's families or
to families of its own:

```c
tb_settings_add_family(settings, &(tb_settings_family){
    .name = "squad", .title = "Squad", .for_players = true});
tb_setting *const difficulty = tb_settings_add(settings, &(tb_setting_definition){
    .name = "gameplay.difficulty",
    .description = "How hard the enemies fight.",
    .type = TB_SETTING_CHOICE,
    .choice = {.names = (const char *const[]){"easy", "normal", "hard"}, .count = 3,
               .default_value = 1}});
```

A name is lowercase words of letters, digits and underscores joined by dots;
its first word is its family. A family is for players, shown in options menus
and saved in the player's file, or it is not: engine tuning and developer
switches.

| Type | Holds | Written |
| --- | --- | --- |
| Boolean | on or off | `true`, `false` (also `on`/`off`, `yes`/`no`, `1`/`0`) |
| Integer | a whole number within a range | `144` |
| Number | a finite number within a range | `0.75` |
| Choice | one of a list of named choices | `borderless` |
| Text | UTF-8 text up to a limit | `"fr-FR"` |

Settings may be added at any time. A value that a file gave for a name before
its setting was added applies when the setting is added, so a module that
starts late still gets the player's choice.

## Files

A settings file holds one setting a line, as `name = value`. Lines that start
with `#` are comments; blank lines are ignored.

```
# Display
display.window_mode = borderless
display.frame_limit = 144

# Language
language.text = "fr-FR"
```

Text is quoted, with `\"`, `\\`, `\n`, `\r` and `\t` as escapes, or bare to the
end of the line; any value may be quoted. A file may begin with a UTF-8 byte
order mark and end its lines with CR LF, as Notepad writes them.

Problems are reported with their line and skipped, and the rest of the file
loads: a player who mistypes one value loses that value, not their settings. A
line that names a setting this version of the game does not have is reported
and kept, and saving writes it back, so a file shared by two versions of a
game loses nothing.

Saving replaces the file in one step: a crash while saving leaves the old
file or the new one, never part of either.

The player's file is `settings.cfg` in a directory named after the game, in
the user's configuration directory:

| Platform | Path |
| --- | --- |
| Windows | `%APPDATA%\Burrow\settings.cfg` |
| Linux and Steam Deck | `$XDG_CONFIG_HOME/Burrow/settings.cfg`, or `~/.config/Burrow/settings.cfg` |
| macOS | `~/Library/Preferences/Burrow/settings.cfg` |

## Restoring defaults

`tb_setting_restore_default()`, `tb_settings_restore_family()` and
`tb_settings_restore_all()` remove the player's choices, as an options menu's
"restore defaults" does; the default file and the built-in values give the
settings their values again.

When the settings leave a game unusable, a display mode the monitor cannot
show for instance, the player starts it with `--reset-settings`:
`tb_settings_set_aside_file()` renames the player's file to
`settings.cfg.bak`, and the game starts from the defaults. The old file is
there to look at, or to restore by renaming it back.

## The engine's settings

`tb_engine_settings_add()` adds these families and settings, in this order,
which is the order of an options menu. A developer build starts in a window
and quits on Escape; a release build starts in a borderless window that fills
the display, and asks before it quits.

Every setting is declared now, so that a game's options and files are complete
from the start; a subsystem that does not exist yet honors none of its
settings. The **Honored by** column says which subsystem applies each one.

### Gameplay

| Setting | Type | Default | Honored by |
| --- | --- | --- | --- |
| `gameplay.pause_when_unfocused` | boolean | `true` | [the game loop](game-loop.md) |

Games add their own, such as difficulty.

### Display

| Setting | Type | Default | Honored by |
| --- | --- | --- | --- |
| `display.window_mode` | `windowed`, `borderless`, `fullscreen` | developer `windowed`, release `borderless` | [the window](windows.md) |
| `display.monitor` | integer, 0 to 63 | `0`, the primary display | [the window](windows.md) |
| `display.window_width` | integer, 0 to 16384 | `0`, to fit the display | [the window](windows.md) |
| `display.window_height` | integer, 0 to 16384 | `0`, to fit the display | [the window](windows.md) |
| `display.fullscreen_width` | integer, 0 to 16384 | `0`, the display's own | [the window](windows.md) |
| `display.fullscreen_height` | integer, 0 to 16384 | `0`, the display's own | [the window](windows.md) |
| `display.aspect_ratio` | `auto`, `16_9`, `16_10`, `21_9`, `32_9`, `4_3` | `auto`, the display's own | the renderer and the options menu, to come |
| `display.refresh_rate` | integer, 0 to 1000 | `0`, the display's own | [the window](windows.md) |
| `display.vsync` | `off`, `on`, `adaptive` | `on` | the renderer, to come |
| `display.frame_limit` | integer, 0 to 1000 | `0`, no limit | [the game loop](game-loop.md) |
| `display.background_frame_limit` | integer, 0 to 1000 | `30` | [the game loop](game-loop.md) |
| `display.brightness` | number, 0.5 to 2 | `1.0` | the renderer, to come |
| `display.hdr` | boolean | `false` | the renderer, to come |

Exclusive fullscreen exists only where the platform has it; elsewhere the
window falls back to borderless and says so.

### Graphics

| Setting | Type | Default | Honored by |
| --- | --- | --- | --- |
| `graphics.renderer` | `auto`, `d3d12`, `d3d11`, `vulkan`, `opengl`, `metal`, `software`, `null` | `auto` | [the RHI](rhi.md#choosing-a-backend), through `tb_engine_settings_device_options()` |
| `graphics.render_scale` | number, 0.25 to 2 | `1.0` | the renderer, to come |
| `graphics.dynamic_resolution` | integer, 0 to 1000 frames a second | `0`, a fixed resolution | the renderer, to come |
| `graphics.upscaler` | `native`, `dlss`, `fsr`, `xess`, `metalfx` | `native` | the renderer, to come |
| `graphics.upscaler_mode` | `native`, `quality`, `balanced`, `performance`, `ultra_performance`, `custom` | `quality` | the renderer, to come |
| `graphics.sharpening` | number, 0 to 1 | `0.5` | the renderer, to come |
| `graphics.frame_generation` | boolean | `false` | the renderer, to come |
| `graphics.low_latency` | `off`, `on`, `boost` | `off` | the renderer, to come |
| `graphics.lighting` | `raster`, `hybrid`, `path_tracing` | `raster` | the renderer, to come |
| `graphics.anti_aliasing` | `off`, `fxaa`, `taa` | `taa` | the renderer, to come |
| `graphics.texture_quality` | `low`, `medium`, `high`, `ultra` | `high` | the renderer, to come |
| `graphics.geometry_quality` | `low`, `medium`, `high`, `ultra` | `high` | the renderer, to come |
| `graphics.shading_quality` | `low`, `medium`, `high`, `ultra` | `high` | the renderer, to come |
| `graphics.shadow_quality` | `low`, `medium`, `high`, `ultra` | `high` | the renderer, to come |
| `graphics.reflection_quality` | `low`, `medium`, `high`, `ultra` | `high` | the renderer, to come |
| `graphics.light_quality` | `low`, `medium`, `high`, `ultra` | `high` | the renderer, to come |
| `graphics.directional_occlusion_quality` | `low`, `medium`, `high`, `ultra` | `high` | the renderer, to come |
| `graphics.particle_quality` | `low`, `medium`, `high`, `ultra` | `high` | the renderer, to come |
| `graphics.decal_quality` | `low`, `medium`, `high`, `ultra` | `high` | the renderer, to come |
| `graphics.water_quality` | `low`, `medium`, `high`, `ultra` | `high` | the renderer, to come |
| `graphics.volumetric_quality` | `low`, `medium`, `high`, `ultra` | `high` | the renderer, to come |
| `graphics.anisotropic_filtering` | `off`, `2x`, `4x`, `8x`, `16x` | `8x` | the renderer's materials (`tb_engine_settings_anisotropy()`) |
| `graphics.field_of_view` | number, 60 to 120 degrees | `90.0` | the renderer, to come |
| `graphics.motion_blur` | `off`, `low`, `medium`, `high` | `off` | the renderer, to come |
| `graphics.depth_of_field` | boolean | `true` | the renderer, to come |
| `graphics.chromatic_aberration` | boolean | `true` | the renderer, to come |
| `graphics.film_grain` | number, 0 to 1 | `1.0`, as the game was made | the renderer, to come |

**Renderer.** `graphics.renderer` names the graphics API, its choices in the
order of `tb_rhi_backend`, and `tb_engine_settings_device_options()` turns it
into the backend of a device's options. `auto` takes the platform's best
that the machine has. Every choice is a value on every platform, because
settings files travel between machines; a choice this platform does not
build is reported by the RHI when the device is made, as "not implemented"
with the next step. `null` draws nothing, for servers and tests that name it
on the command line; an options menu leaves it out, as Unreal's leave out its
null RHI.

The upscaler's mode sets the render scale as the vendors' modes do (`native`
renders at the output's resolution, `ultra_performance` at a third of it),
and `custom` uses `graphics.render_scale`. `graphics.dynamic_resolution`
lowers the render resolution from there while frames are slower than it
asks. Sharpening is the upscaler's own where one runs, and the engine's
otherwise.

**Quality.** The eleven `graphics.*_quality` settings are quality groups, as
Unreal's scalability groups are, and the overall quality of an options menu
is what they are, not a setting of its own. `tb_engine_settings_set_quality()`
sets every group to one level, and anisotropic filtering from `2x` at low to
`16x` at ultra; `tb_engine_settings_quality()` gives that level back, or
custom once a player has changed a group or the filtering. A player's file
keeps each group as they chose it.

### Audio

| Setting | Type | Default | Honored by |
| --- | --- | --- | --- |
| `audio.master_volume` | number, 0 to 1 | `1.0` | the mixer, to come |
| `audio.music_volume` | number, 0 to 1 | `1.0` | the mixer, to come |
| `audio.effects_volume` | number, 0 to 1 | `1.0` | the mixer, to come |
| `audio.ambient_volume` | number, 0 to 1 | `1.0` | the mixer, to come |
| `audio.voice_volume` | number, 0 to 1 | `1.0` | the mixer, to come |
| `audio.interface_volume` | number, 0 to 1 | `1.0` | the mixer, to come |
| `audio.mute_when_unfocused` | boolean | `false` | the mixer, to come |
| `audio.output_device` | text | `""`, the system's | the audio device, to come |
| `audio.speaker_mode` | `automatic`, `stereo`, `headphones`, `surround_5_1`, `surround_7_1` | `automatic` | the mixer, to come |
| `audio.dynamic_range` | `wide`, `standard`, `night` | `standard` | the mixer, to come |

### Controls

| Setting | Type | Default | Honored by |
| --- | --- | --- | --- |
| `controls.mouse.sensitivity` | number, 0.01 to 10 | `1.0` | input, to come |
| `controls.mouse.invert_y` | boolean | `false` | input, to come |
| `controls.mouse.raw_input` | boolean | `true` | input, to come |
| `controls.mouse.smoothing` | boolean | `false` | input, to come |
| `controls.gamepad.horizontal_sensitivity` | number, 0.1 to 10 | `1.0` | input, to come |
| `controls.gamepad.vertical_sensitivity` | number, 0.1 to 10 | `1.0` | input, to come |
| `controls.gamepad.invert_y` | boolean | `false` | input, to come |
| `controls.gamepad.vibration` | number, 0 to 1 | `1.0` | input, to come |
| `controls.gamepad.left_dead_zone` | number, 0 to 0.9 | `0.1` | input, to come |
| `controls.gamepad.right_dead_zone` | number, 0 to 0.9 | `0.1` | input, to come |
| `controls.touch.sensitivity` | number, 0.1 to 10 | `1.0` | input, to come |

Key and button bindings are not settings: they belong to the input module,
which keeps them in a file of their own.

### Interface

| Setting | Type | Default | Honored by |
| --- | --- | --- | --- |
| `interface.scale` | number, 0.5 to 2 | `1.0` | the UI, to come |
| `interface.font` | text | `""`, the game's own | the UI, to come |
| `interface.hud_opacity` | number, 0 to 1 | `1.0` | the UI, to come |
| `interface.crosshair` | boolean | `true` | the UI, to come |
| `interface.performance_overlay` | `off`, `frame_rate`, `detailed` | `off` | the UI, to come |

`interface.scale` multiplies the scale the engine picks from the display's
size and density, so the interface stays readable on a small screen such as
the Steam Deck's, and a player can still make it larger or smaller.

### Accessibility

| Setting | Type | Default | Honored by |
| --- | --- | --- | --- |
| `accessibility.subtitles` | `off`, `dialogue`, `captions` | `dialogue` | the UI, to come |
| `accessibility.subtitle_size` | `small`, `medium`, `large`, `largest` | `medium` | the UI, to come |
| `accessibility.subtitle_background` | number, 0 to 1 | `0.5` | the UI, to come |
| `accessibility.text_size` | `standard`, `large`, `largest` | `standard` | the UI, to come |
| `accessibility.color_blind_mode` | `off`, `protanopia`, `deuteranopia`, `tritanopia` | `off` | the renderer, to come |
| `accessibility.color_blind_strength` | number, 0 to 1 | `1.0` | the renderer, to come |
| `accessibility.camera_shake` | number, 0 to 1 | `1.0` | the game, to come |
| `accessibility.reduce_flashing` | boolean | `false` | the renderer, to come |

### Language

| Setting | Type | Default | Honored by |
| --- | --- | --- | --- |
| `language.text` | text, a language tag | `""`, the system's language | localization, to come |
| `language.voice` | text, a language tag | `""`, the text's language | localization, to come |

### Engine

Not shown to players, and not saved in their file: what a game ships with in
its default file.

| Setting | Type | Default | Honored by |
| --- | --- | --- | --- |
| `engine.tick_rate` | integer, 1 to 1000 | `60` | [the game loop](game-loop.md) |
| `engine.max_frame_time` | number, 0.01 to 1 seconds | `0.25` | [the game loop](game-loop.md) |

### Developer

Not shown to players, and not saved in their file.

| Setting | Type | Default | Honored by |
| --- | --- | --- | --- |
| `developer.quit_without_asking` | boolean | developer `true`, release `false` | [the game loop](game-loop.md) |
| `developer.frame_count` | integer, 0 and up | `0`, run until asked to quit | [the game loop](game-loop.md) |
| `developer.time_scale` | number, 0 to 16 | `1.0` | [the game loop](game-loop.md) |

## Burrow

Burrow loads its settings in layers at start: the engine's, then
`src/burrow_defaults.cfg`, Burrow's default file, which the build embeds in
the executable, then the player's file, then the command line.

| Command line | Does |
| --- | --- |
| `burrow settings` | Lists every setting, its value, and the layer it comes from |
| `burrow` | Plays |
| `--set NAME=VALUE` | Gives a setting a value for this session; may be repeated |
| `--renderer=NAME` | The same as `--set graphics.renderer=NAME`, given after every `--set` |
| `--reset-settings` | Sets the player's file aside and starts from the defaults |
| `--settings-file FILE` | Uses another settings file in place of the player's |
| `--mode developer` or `--mode release` | Takes a developer or a release build's defaults; each build defaults to its own |

## Threading

Every function but `tb_settings_create()` and `tb_settings_destroy()` is
thread-safe. One mutex in the registry guards the values, so readers on any
thread always see a whole value, never part of one being written; a setting's
name, type and range never change after it is added.
