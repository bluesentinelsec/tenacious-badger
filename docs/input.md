# Input

`input.h` turns what players press, move and touch into what they mean to
do, after Unreal's Enhanced Input. A game reads **actions** ("jump", "move",
"look", "select"), never devices. **Mapping contexts** ("gameplay", "menu",
"vehicle") say which **keys** drive which actions, and each player has a
stack of them, so that the same key means different things as the player
moves from a level to a menu. The code is in `src/input/`.

```c
tb_input *input = tb_input_create(NULL);
tb_input_file_error error = {0};
if (tb_input_load_file(input, vfs, "Input/burrow.json", &error) != TB_INPUT_FILE_OK)
{
    tb_fmt_print_error("%s\n", error.message); /* Input/burrow.json:12:16: $.contexts[0]... */
}
tb_input_player *player = tb_input_player_at(input, 0);
tb_input_add_context(player, tb_input_find_context(input, "gameplay"), 0);
const tb_input_action jump = tb_input_find_action(input, "jump");
const tb_input_action move = tb_input_find_action(input, "move");

/* As the window's events arrive: */
tb_input_feed_window_event(input, &event);
/* Once a frame, after polling the gamepads (gamepad.h): */
tb_input_feed_gamepad(input, TB_INPUT_DEVICE_GAMEPAD(slot), &state);
/* Once a tick, before the simulation: */
tb_input_tick(input, time->tick_seconds);
if (tb_input_action_triggered(player, jump)) { ... }
const tb_input_value direction = tb_input_action_value(player, move); /* x right, y forward */
```

The module is not yet driven by the [game loop](game-loop.md), the
[GUI](gui.md) or Burrow: a game feeds it and ticks it itself. Gamepads
come from the [gamepad interface](gamepad.md), through
`tb_input_feed_gamepad()`. Touch screens come in a later change, through
`tb_input_feed_key()`; their keys are named now, so that definitions written
today hold them.
The sandbox's `input` scene ([sandbox](sandbox.md)) shows every action of a
file live.

## Actions

An action has a name, unique in its input, of lowercase letters, digits and
`_`, and a value of one of four types:

| Type | Value | For |
| --- | --- | --- |
| `bool` | 1 or 0, in `x` | jump, fire, select |
| `axis1d` | `x` | throttle, zoom |
| `axis2d` | `x`, `y` | move, look |
| `axis3d` | `x`, `y`, `z` | flying |

`tb_input_create_action()` makes one; actions last as long as their input.
When several keys drive an action at once, their values combine in each axis
by its **accumulation**: `highest`, the value farthest from zero, Unreal's
default, so that W and the up arrow together are 1; or `cumulative`, the sum,
so that W and S together cancel.

An action may have modifiers and triggers of its own, applied to the
combined value after every mapping's.

## Keys

A key is anything a player presses, moves or touches, named by its place
rather than its label, as Unreal's `EKeys`. Files name keys as
`tb_input_key_name()` does.

| Keys | Names | Value |
| --- | --- | --- |
| Every key of `window.h` | `a` to `z`, `0` to `9`, `f1` to `f12`, `escape`, `enter`, `space`, `tab`, `backspace`, `grave`, `left`, `right`, `up`, `down`, `left_shift`, `left_control`, `left_alt` and their `right_` keys, `home`, `end`, `page_up`, `page_down`, `insert`, `delete` | a button |
| Mouse buttons | `mouse_left`, `mouse_right`, `mouse_middle`, `mouse_thumb_1`, `mouse_thumb_2` | a button |
| The wheel | `mouse_wheel_up`, `mouse_wheel_down` | a button, down for the ticks the wheel turned, by its notches |
| | `mouse_wheel` | an axis1d of notches, away from the player positive |
| The mouse's motion | `mouse_x`, `mouse_y`, and both as `mouse_xy` | how far it moved in a tick; up positive |
| Gamepad buttons | `gamepad_face_south`, `_east`, `_west`, `_north`, `gamepad_left_shoulder`, `gamepad_right_shoulder`, `gamepad_left_stick_click`, `gamepad_right_stick_click`, `gamepad_dpad_up`, `_down`, `_left`, `_right`, `gamepad_start`, `gamepad_back`, `gamepad_guide`, and the back paddles `gamepad_left_paddle_1`, `_2`, `gamepad_right_paddle_1`, `_2` (the Steam Deck's L4, L5, R4, R5) | a button |
| Gamepad triggers | `gamepad_left_trigger`, `gamepad_right_trigger` | an axis1d from 0 to 1 |
| | `gamepad_left_trigger_button`, `gamepad_right_trigger_button` | a button, made from the trigger's value |
| Gamepad sticks | `gamepad_left_stick_x`, `_y`, and both as `gamepad_left_stick`; the same for `right` | from -1 to 1, right and up positive |
| | `gamepad_left_stick_up`, `_down`, `_left`, `_right`; the same for `right` | a button, made from the stick's value |
| Touches | `touch_1` to `touch_10` | a button while a finger is down |
| | `touch_1_position` to `touch_10_position` | an axis2d in logical units |

The gamepad keys are one standard layout, by place: the bottom face button is
`gamepad_face_south` whether it says A or Cross.

**Sticks and triggers as buttons.** As a stick's X or Y or a trigger is fed,
the input makes the buttons of its directions, as Unreal's
`Gamepad_LeftStick_Up` and `Gamepad_LeftTrigger` are made beside
`Gamepad_LeftY` and `Gamepad_LeftTriggerAxis`, so that a context maps "jump"
to a trigger squeezed, or a menu's "up" to the stick. A direction is down
from 0.5 and up again below 0.4, a trigger's button from 0.2 and below 0.1:
the defaults of [the gamepad interface](gamepad.md#sticks-and-triggers-as-buttons),
whose `tb_gamepad_analog_button()` makes them, with the reasons for each.
Between the two thresholds a button keeps its state, so a value hovering at
the threshold does not press it again and again. They are made from the
values as fed, before any dead zone, and a recording, which holds the axes,
makes them again when replayed.

**The guide button is the system's.** `gamepad_guide` is named, so that a
device of a game's own may give it, but no gamepad of the engine does: the
Xbox, PS and Steam buttons, and the share button, belong to the operating
system and the launcher (Game Bar, the Steam overlay, screenshots, the
system's gestures), and the [gamepad interface](gamepad.md#the-systems-buttons)
leaves them to it.

A **button**'s value is 1 while it is down. The **mouse's motion** and the
**wheel** add up between ticks and start from zero after each. Any other
**axis** keeps the value fed last. A **2D key** is its X and Y keys: feeding
one sets the others.

## Contexts and players

A mapping context holds **mappings**, each from one key to one action, with
modifiers and triggers of its own. Any number of keys may map to one action,
and one key to any number of actions. `tb_input_context_map()` adds one.

An input has up to eight **players**, `tb_input_player_at()`, for local
multiplayer. Each player has its own stack of contexts, its own keys, and its
own state of every action. **Devices** are assigned to players
(`tb_input_assign_device()`): the keyboard and the mouse to player 0 at
first. A device of no player is ignored.

`tb_input_add_context()` puts a context on a player's stack with a
**priority**. Higher priorities see a key first; of equal priorities, the
context added last does. A mapping **consumes** its key, so that contexts
below do not see it, unless it says `pass_through`, as Unreal's actions do
by default. Consumption is by mapping, not by whether the action fired: a
menu that maps Escape hides Escape from gameplay while the menu is on the
stack. In one context, every mapping of a key sees it.

A key held down when a context is added is ignored by that context's
mappings until it is released, as Unreal ignores keys held when its mappings
are rebuilt: the press that opened a menu does not choose in it.
`tb_input_remove_context()` takes a context away; its actions that were under
way are completed or canceled at the next tick.

A context may also set **rules of an action**: modifiers and triggers it adds
to each of its own mappings of that action, after the mapping's own
(`tb_input_context_set_action_rules()`). A menu context makes "select" a tap
while gameplay makes it a hold. A context changed while a player has it
starts over for that player: its rules forget what they held.

## Modifiers

A modifier changes a value on its way from a key to an action. A mapping's
modifiers apply in order, then its context's for the action, then, on the
combined value, the action's own.

| Modifier | Fields | Does |
| --- | --- | --- |
| `dead_zone` | `kind` (`radial`, `axial`, `unscaled_radial`), `lower` (0 for 0.2), `upper` (0 for 1) | values shorter than `lower` are zero; `radial` scales the length from `lower` to `upper` onto 0 to 1, keeping the direction; `axial` does each axis alone; `unscaled_radial` keeps a value outside as it is, no longer than `upper` |
| `negate` | `axes` (`x`, `y`, `z`; none for all) | changes their sign |
| `swizzle` | `order`: `yxz`, `zyx`, `xzy`, `yzx`, `zxy` | puts the axes in that order: `yxz` turns a key's X into the action's Y, so that W is forward |
| `scalar` | `scale`, one number or three | multiplies each axis |
| `response_curve` | `exponent`, one number or three (0 is 1) | raises each axis to its power, keeping its sign |
| `smooth` | `samples`, 1 to 8 (0 for 4) | the mean of the last ticks' values |
| `settings` | `source`: `mouse`, `gamepad`, `touch`, `left_stick`, `right_stick` | the player's [settings](#settings) |

WASD into a 2D vector:

```json
{"action": "move", "key": "w", "modifiers": [{"type": "swizzle", "order": "yxz"}]},
{"action": "move", "key": "s", "modifiers": [{"type": "swizzle", "order": "yxz"}, {"type": "negate"}]},
{"action": "move", "key": "a", "modifiers": [{"type": "negate"}]},
{"action": "move", "key": "d"}
```

## Triggers

A trigger decides when an action fires. Each tick it comes to a state: none,
ongoing (under way, not yet fired), or triggered. A value is **actuated**
when its length is at least the trigger's `threshold`, 0.5 when it gives
none, as Unreal's.

| Trigger | Fields | Triggered |
| --- | --- | --- |
| `down` | `threshold` | every tick the value is actuated |
| `pressed` | `threshold` | once, on the tick it becomes actuated |
| `released` | `threshold` | on the tick it stops being actuated; ongoing while it is |
| `hold` | `threshold`, `seconds` (0 for 1), `one_shot` | every tick once actuated for `seconds`, or once a hold with `one_shot`; ongoing before |
| `hold_and_release` | `threshold`, `seconds` (0 for 1) | on release after `seconds` or more; ongoing while held |
| `tap` | `threshold`, `seconds` (0 for 0.2) | on release within `seconds`; ongoing while held, until held longer |
| `pulse` | `threshold`, `seconds` (0 for 1), `on_start`, `limit` (0 for none) | every `seconds` of a hold, on its first tick too with `on_start`, at most `limit` times a hold; ongoing between |
| `chorded` | `action` | while that other action is triggered |
| `chord_blocker` | `action` | never: it blocks while that other action is triggered |

Times are the sums of the ticks' lengths, compared inclusively: a `hold` of
0.5 seconds fires on the tick its time reaches 0.5, and a `tap` of 0.5
seconds released at 0.5 is a tap.

The triggers of one mapping come to one state by Unreal's rules. Most are
**explicit**: the mapping fires when any of them does. `chorded` is
**implicit**: the mapping fires only when all of them do too. `chord_blocker`
is a **blocker**: while it fires, the mapping does not. A mapping with no
explicit trigger fires while its value is not zero, so a mapping without
triggers is triggered while its key is down, and a chord alone needs its own
key down too. An action takes the highest state of its mappings. When it has
triggers of its own, they are applied to the combined value, and the action
fires only when both its mappings' triggers and its own do.

An action with a chord is evaluated after the actions without, so that it
sees the state of its chord's action in the same tick, whatever order they
were made in. When the chord's action has a chord of its own, it sees that
action's state of this tick if that action was made first, else of the last.

## States and events

Each tick an action moves from its last state to its new one, and the move
is its **events**, as Unreal's:

| From | To none | To ongoing | To triggered |
| --- | --- | --- | --- |
| none | | started | started, triggered |
| ongoing | canceled | ongoing | triggered |
| triggered | completed | ongoing | triggered |

An action's **value** is the value its mappings and modifiers made, while it
is ongoing or triggered, and zero while it is in state none. A bool action's
value is 1 or 0.

A game **polls** an action after a tick: `tb_input_action_value()`,
`tb_input_action_triggered()`, `tb_input_action_events()`,
`tb_input_action_state()`, and `tb_input_get_action_status()`, which adds how
long the action has been under way (`elapsed_seconds`), how long triggered
(`triggered_seconds`), and when it last triggered (`last_triggered_at`, in
`tb_input_seconds()`).

Or it **binds callbacks** to an action's events (`tb_input_bind()`). They are
called at the end of `tb_input_tick()`, after every action of the player has
been evaluated: in the order bound, once for each event in the order started,
ongoing, triggered, completed, canceled. A callback may unbind, which stops
at once, bind, which starts at the next tick, and add and remove contexts.

## Ticks

Input is gathered as it arrives and evaluated once a tick, with the tick's
length, so that it is the same at any frame rate and follows the game loop's
fixed ticks.

- **Gathering.** `tb_input_feed_window_event()` takes the window's keys,
  mouse buttons, motion and wheel. A key's repeats are ignored; text and IME
  composition stay with the window. Relative mouse mode gives the mouse's own
  motion; otherwise the cursor's motion is taken from its positions, in
  logical units, from the first position after it enters the window. When the
  window loses the focus, every key and button held is released.
  `tb_input_feed_gamepad()` takes a gamepad's state, as the
  [gamepad interface](gamepad.md) read it, for the player its device is
  assigned to: each button, stick axis and trigger that changed. A pad that
  is gone releases everything. Which pad is which device is the game's
  choice; `TB_INPUT_DEVICE_GAMEPAD(slot)` numbers them by slot, and
  `tb_input_assign_device()` gives each to a player. `tb_input_feed_key()`
  takes a key's value from any other device, and is how touch screens will
  come in.
- **Evaluation.** `tb_input_tick()` evaluates every action of every player,
  then calls the callbacks. A button pressed and released between two ticks
  is down for the next tick and up for the one after, so a tap is never lost,
  and a `pressed` trigger fires once: started and triggered, then completed.
  Several presses between two ticks are one.

## Settings

The `settings` modifier applies the player's own settings, so that a game
need not:

| Source | Applies |
| --- | --- |
| `mouse` | `controls.mouse.sensitivity` on X and Y, Y negated with `controls.mouse.invert_y`, and the mean of two ticks with `controls.mouse.smoothing` |
| `gamepad` | `controls.gamepad.horizontal_sensitivity` on X and `vertical_sensitivity` on Y, Y negated with `controls.gamepad.invert_y` |
| `touch` | `controls.touch.sensitivity` on X and Y |
| `left_stick`, `right_stick` | a radial dead zone of `controls.gamepad.left_dead_zone` or `right_dead_zone` |

Dead zones are applied here, and only here: the gamepad interface and
`tb_input_feed_gamepad()` give the pad's values as it reported them, scaled
to the standard ranges, so that a stick's dead zone is applied once, by the
mapping's `left_stick` or `right_stick` modifier. The buttons made from the
sticks and triggers have thresholds of their own, well past any dead zone a
player is likely to choose. `controls.gamepad.vibration` is not an input
setting: a game gives it to the gamepad system
(`tb_gamepad_system_set_vibration()`), which scales rumble by it.

`tb_input_follow_settings()` reads them from the engine's
[settings](settings.md) at the start of every tick, so a change in an options
menu applies at the next one. `tb_input_set_settings()` sets them directly
instead, for a tool or a test.

## Recording and replaying

`tb_input_record()` records every tick into a `tb_input_recording`: the keys
gathered for it, by player, its length, and the settings it used.
`tb_input_replay_tick()` gathers a recorded tick's keys, with its settings,
and ticks for its length. An input with the same actions and contexts, in the
same state as the recorded one when the recording began, gives the same
values, states, events and callbacks, tick for tick: a replay or a regression
test of gameplay. A recording lives in memory; writing one to a file is to
come.

## Files

Actions and contexts are made in code, or loaded from JSON files that an
editor can write, like the GUI's [layout files](gui.md#layout-files).
`tb_input_load()` makes a file's text, and `tb_input_load_file()` a file read
through a [VFS](vfs.md); `tb_input_save()` writes every action and context of
an input.

```json
{
  "format": "badger-input",
  "version": 1,
  "actions": [
    {"name": "move", "type": "axis2d", "accumulation": "cumulative"},
    {"name": "look", "type": "axis2d", "modifiers": [{"type": "settings", "source": "mouse"}]},
    {"name": "jump", "type": "bool"},
    {"name": "select", "type": "bool"}
  ],
  "contexts": [
    {
      "name": "gameplay",
      "mappings": [
        {"action": "move", "key": "w", "modifiers": [{"type": "swizzle", "order": "yxz"}]},
        {"action": "look", "key": "mouse_xy"},
        {"action": "jump", "key": "space", "triggers": [{"type": "pressed"}]}
      ]
    },
    {
      "name": "menu",
      "actions": [{"action": "select", "triggers": [{"type": "tap", "seconds": 0.25}]}],
      "mappings": [{"action": "select", "key": "enter"}]
    }
  ]
}
```

A file is an object of `format`, always `"badger-input"`; `version`, 1, and a
newer one is refused; and `actions` and `contexts`, each optional. An
**action** is its `name`, its `type`, and optionally its `accumulation`,
`modifiers` and `triggers`. A **context** is its `name`, its `mappings`, and
optionally `actions`, its rules of actions, each an `action` with
`modifiers` and `triggers`. A **mapping** is its `action` and its `key`, and
optionally its `modifiers`, its `triggers`, and `pass_through`. Modifiers
and triggers are objects of their `type` and the fields of the tables above.
A mapping or a chord may name an action of the file or one the input had
before, so one file may hold the actions and others the contexts; a chord
may name an action later in its file.

**Loading is all or nothing.** The text is strict JSON, read by the engine's
[reader](serialization.md#json). A file that is not JSON is
`TB_INPUT_FILE_MALFORMED`; one that is JSON but not definitions is
`TB_INPUT_FILE_INVALID`: another format or a newer version, a key the format
does not have, a name taken or unknown, a value of the wrong type, a number
out of range, a rule the checks of the API refuse. Whatever the file made is
undone. The error, `tb_input_file_error`, says where and why, as the GUI's
do: the JSON path of the value at fault (`$.contexts[0].mappings[2].key`),
its line and column, a reason, and a message of all of them for a log.

**Saving** writes the same bytes for the same definitions: indented two
spaces, actions and contexts in the order made, members in the order of the
format, numbers in the fewest digits that read back the same, and fields
left out where they hold their default. A file loaded and saved again is
unchanged. The bindings a player changes are not written here: rebinds will
be saved apart, as differences from these defaults.

## Where Unreal's model was adapted

- **Assets become handles.** Unreal's input actions and mapping contexts are
  assets; here they are definitions an input owns, named by handles, and
  last as long as it does. Contexts may gain mappings at any time.
- **Consumption is by mapping.** Unreal's `bConsumeInput` is a flag of the
  action; here each mapping consumes its key unless it passes it through,
  which is the same by default and finer when needed.
- **Rules of actions in contexts.** Unreal puts triggers and modifiers on
  actions and mappings; a context's rules of an action are added here so that
  one action can behave differently in two contexts without copying its
  mappings.
- **Ticks, not frames.** Unreal evaluates input once a frame with the frame's
  time. Here it is once a tick with the tick's length, so that it follows the
  fixed tick and can be replayed.
- **Callbacks after evaluation.** Unreal's delegates run as each action is
  processed; here they run once every action of the player has been
  evaluated, so a callback sees the whole tick.

## Threading

An input is **stage-owned**: it, its players and its recordings are used on
one thread, the one that ticks it, usually the game loop's. Callbacks run on
that thread, inside `tb_input_tick()`. The functions on keys alone
(`tb_input_key_name()`, `tb_input_find_key()`, `tb_input_key_type()`,
`tb_input_key_from_window()`, `tb_input_key_from_mouse_button()`,
`tb_input_key_from_gamepad_button()`, `tb_input_key_from_gamepad_axis()`) are
thread-safe.

## Still to come

- **Touch**, through `tb_input_feed_key()`.
- **Rebinding and prompts**: listening for a key to bind, conflicts, rebinds
  saved apart from the defaults, and keys' names for button prompts.
- **Integration**: the game loop gathers and ticks input, the GUI's
  navigation reads `ui_*` actions, and Burrow's controls become actions.
- A file format for recordings.
