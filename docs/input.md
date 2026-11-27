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

## Rebinding

A player may bind any mapping to another key, for that player alone, as
Unreal's player-mappable keys do: the definitions stay the game's, and each
player's **rebinds** are differences from them. A Controls menu lists a
context's mappings with `tb_input_context_mapping_count()` and
`tb_input_context_mapping_at()`, which give each mapping's action and its
default key, and shows each as the player has it bound,
`tb_input_binding_key()`.

```c
tb_input_rebind(player, gameplay, mapping, TB_INPUT_KEY_F);    /* jump on F, for this player */
tb_input_rebind(player, gameplay, mapping, TB_INPUT_KEY_NONE); /* unbound */
tb_input_reset_binding(player, gameplay, mapping);             /* its default again */
tb_input_reset_bindings(player);                               /* "reset to defaults" */
```

A rebound mapping keeps its modifiers and triggers, and sees its new key
from the next tick. An **unbound** mapping sees no key and consumes none, so
a context below sees the key again. Rebinding to the default forgets the
rebind; `tb_input_is_rebound()` and `tb_input_rebind_count()` say what
differs. The player's contexts with the mapping start over, as a changed
context does, and a new key held as it is bound waits to be released, so
the press that chose it does not act.

**Conflicts.** A rebind is never refused. `tb_input_find_conflicts()` lists
the mappings of a context whose keys, as the player has them, would collide
with a key: the same key, or a key that shares a value with it (a stick and
its X, a stick's X and its directions as buttons, the wheel and its
buttons). A menu asks before it rebinds, and warns, swaps or unbinds the
other, as the game prefers. Keys collide only within a context: two
contexts on the stack at once see the same key by their priorities, which
is what contexts are for. To compare with another context, ask of that
context, with `TB_INPUT_NO_MAPPING` for none left out.

**Classes.** Keys are of six classes (`tb_input_key_class_of()`): the
keyboard, the mouse's buttons, the mouse's axes (motion, the wheel and the
wheel's buttons), a gamepad's buttons, a gamepad's axes (sticks, triggers,
and the buttons made from them), and touch. A listener accepts classes, and
a prompt looks for a binding of some: `tb_input_find_binding()` gives the
first key, as the player has it bound, of a context's mappings of an action
whose class is asked for, so that one action shows Space to a keyboard
player and A to a gamepad player.

### Listening for a key

`tb_input_listen()` waits for the next key a player presses or moves, for
a menu's "press a key to bind", and `tb_input_listen_result()` says what
came: still waiting, a key captured, with the device and the player that
gave it, or canceled.

```c
tb_input_listen(input, &(tb_input_listen_desc){.player = player,
                                               .classes = TB_INPUT_CLASS_GAMEPAD});
/* each tick: */
tb_input_capture capture = {0};
if (tb_input_listen_result(input, &capture) == TB_INPUT_LISTEN_CAPTURED)
{
    tb_input_rebind(player, gameplay, mapping, capture.key);
}
```

| Field | Default | Does |
| --- | --- | --- |
| `player` | every player | whose devices count |
| `classes` | all | which classes of keys are taken |
| `type` | `bool` | what the key is for: a bool takes buttons, and a stick or a trigger as a button (`gamepad_left_stick_up`, `gamepad_right_trigger_button`) and the wheel's buttons, never the mouse's motion; an axis1d takes buttons and the axis moved (`gamepad_left_stick_x`, `mouse_x`, `mouse_wheel`); an axis2d or axis3d takes buttons and the 2D key of the axis moved (`gamepad_left_stick`, `mouse_xy`, a touch's position) |
| `threshold` | 0.5 | how far a stick or a trigger moves from rest to be taken as an axis |
| `distance` | 32 | how far the mouse moves, in logical units, to be taken |
| `cancel_keys` | Escape and `gamepad_start` | keys that cancel rather than being taken, whatever the classes; a list of none, with a count of 0, for no cancel |

Listening looks at each tick's keys, as the rest of the input does, so it is
the same at any frame rate. **The press that started it is not taken**: a
key down or an axis away from rest when listening began counts only once it
is released, or back below half the threshold. Cancel keys are looked at
first; of several keys in one tick, the first in the order of the keys is
taken. While listening, and in the tick that ends it, the players listened
to **have no actions**, so that Escape cancels the listener without closing
the menu; after, a key still held is ignored by their contexts until it is
released, so that the key captured does not act too. Listening ends at the
first capture or cancel; `tb_input_stop_listening()` ends it, and forgets
what came. A replayed tick's keys have no device.

`tb_input_player_last_device()` is the device that last gave a player a key,
a press or an axis moved, which is what a game shows prompts for: the
keyboard's and the mouse's labels, or those of the family of the pad it
numbered so ([gamepad families](gamepad.md#families)).

## Prompts

`tb_input_key_prompt()` gives how a key is shown, for a family of gamepads:
a **label**, English, as printed on the key or the pad, and a **glyph**, the
identifier of its picture.

```c
tb_gamepad_info info = {0};
tb_gamepad_get_info(gamepads, slot, &info);
tb_input_window_prompt prompt = {0};
tb_input_key_prompt_for_window(windows,
    tb_input_find_binding(player, gameplay, jump, TB_INPUT_CLASS_GAMEPAD), info.family, &prompt);
/* prompt.label "Cross", prompt.glyph "playstation/cross" on a DualSense */
```

For an on-screen binding prompt, call `tb_input_key_prompt_for_window()` with
the window system that receives the player's input. Its caller-owned label
follows the current keyboard layout and platform names such as Left Option
on macOS. Read it again when displaying the prompt after a layout change.
The glyph and the serialized binding still identify the physical key.
Without a native label the prompt uses the portable name.

A gamepad's key is shown by **the label its family prints at the key's
place**. South is A on an Xbox pad, Cross on a PlayStation pad, and B on a
Nintendo pad, whose A is east: the engine reads pads by place
([the layout](gamepad.md#nintendos-pads-by-place)), and the prompt names the
label the player sees there. The keyboard's keys, the mouse's and touches
are the same for every family.

**Glyphs** are `set/element`, lowercase ASCII, stable: the names of an icon
atlas's entries or of a folder of pictures, and keys of a game's own strings
for a localized label, falling back to the English one. A set is a family's
(`xbox`, `playstation`, `nintendo`, `steam_deck`, `generic`), or `keyboard`,
`mouse` or `touch`. As Steam Input's action origins name each family's
button by its own label (`XBoxOne_A`, `PS5_X`, `Switch_A`, `SteamDeck_L4`),
an element names a labeled button by its label, so that one glyph is one
picture; and as Unreal's CommonUI keeps a table of pictures for each
controller type, the sets are tables of the same elements. What has no
label (sticks, their directions, the d-pad) has the same element in every
set, and `generic` names every key by its place, for neutral pictures; its
labels are Xbox's, as most PC pads print.

| Key | `xbox` | `playstation` | `nintendo` | `steam_deck` | `generic` |
| --- | --- | --- | --- | --- | --- |
| `gamepad_face_south` | A, `a` | Cross, `cross` | B, `b` | A, `a` | A, `south` |
| `gamepad_face_east` | B, `b` | Circle, `circle` | A, `a` | B, `b` | B, `east` |
| `gamepad_face_west` | X, `x` | Square, `square` | Y, `y` | X, `x` | X, `west` |
| `gamepad_face_north` | Y, `y` | Triangle, `triangle` | X, `x` | Y, `y` | Y, `north` |
| `gamepad_left_shoulder`, `_right_` | LB, RB: `lb`, `rb` | L1, R1 | L, R | L1, R1 | LB, RB: `left_shoulder`, `right_shoulder` |
| `gamepad_left_trigger`, `_right_`, and their `_button`s | LT, RT | L2, R2 | ZL, ZR | L2, R2 | LT, RT: `left_trigger`, `right_trigger` |
| `gamepad_left_stick_click`, `_right_` | LS, RS | L3, R3 | LS, RS | L3, R3 | LS, RS: `left_stick_click`, `right_stick_click` |
| `gamepad_start` | Menu, `menu` | Options, `options` | +, `plus` | Menu, `menu` | Start, `start` |
| `gamepad_back` | View, `view` | Create, `create` | -, `minus` | View, `view` | Back, `back` |
| `gamepad_guide` | Xbox, `guide` | PS, `ps` | Home, `home` | Steam, `steam` | Guide, `guide` |
| `gamepad_left_paddle_1`, `_2` | P3, P4 | Left Back, `left_back`; `left_paddle_2` | GL, `gl`; `left_paddle_2` | L4, L5 | `left_paddle_1`, `_2` |
| `gamepad_right_paddle_1`, `_2` | P1, P2 | Right Back, `right_back`; `right_paddle_2` | GR, `gr`; `right_paddle_2` | R4, R5 | `right_paddle_1`, `_2` |
| sticks, their X and Y, their directions, the d-pad | Left Stick, `left_stick`; Left Stick X, `left_stick_x`; Left Stick Up, `left_stick_up`; D-pad Up, `dpad_up`; and so on, in every set | | | | |

Where an element is not given, it is the label in lowercase: `l1`, `zl`,
`l4`, `p3`. A trigger and its button share a glyph. A paddle a family has no
button for is named by its place.

| Keys | Labels | Glyphs |
| --- | --- | --- |
| The keyboard | `A` to `Z`, `0` to `9`, `F1` to `F12`, `Escape`, `Enter`, `Space`, `Tab`, `Backspace`, `` ` ``, `Left Arrow`, `Right Arrow`, `Up Arrow`, `Down Arrow`, `Left Shift`, `Right Shift`, `Left Ctrl`, `Right Ctrl`, `Left Alt`, `Right Alt`, `Home`, `End`, `Page Up`, `Page Down`, `Insert`, `Delete` | `keyboard/` and the key's name: `keyboard/a`, `keyboard/left_shift` |
| The mouse | `Left Mouse Button`, `Right Mouse Button`, `Middle Mouse Button`, `Mouse Button 4`, `Mouse Button 5`, `Mouse Wheel Up`, `Mouse Wheel Down`, `Mouse Wheel`, `Mouse X`, `Mouse Y`, `Mouse` | `mouse/left`, `right`, `middle`, `thumb_1`, `thumb_2`, `wheel_up`, `wheel_down`, `wheel`, `x`, `y`, `xy` |
| Touches | `Touch 1` to `Touch 10`, `Touch 1 Position` to `Touch 10 Position` | `touch/1` to `touch/10`, `touch/1_position` to `touch/10_position` |

The table lists the portable labels. Window-aware prompts can display A for
the physical `keyboard/q` binding on a French layout.

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
unchanged. The bindings a player changes are not written here, but in a
[bindings file](#bindings-files) of their own.

### Bindings files

A player's rebinds are saved apart from the definitions, as differences
from them, so that a game's new defaults reach players who changed nothing,
and a player's changes outlive the game's: `tb_input_save_bindings()` writes
a player's rebinds, and `tb_input_load_bindings()`, or
`tb_input_load_bindings_file()` through a VFS, gives a player those of a
file, in place of every rebind the player had.

```json
{
  "format": "badger-input-bindings",
  "version": 1,
  "bindings": [
    {
      "context": "gameplay",
      "action": "jump",
      "default": "space",
      "key": "f"
    },
    {
      "context": "gameplay",
      "action": "fire",
      "default": "mouse_left",
      "slot": 1,
      "key": null
    }
  ]
}
```

A file is an object of `format`, always `"badger-input-bindings"`;
`version`, 1, and a newer one is refused; and `bindings`, optional. A
**binding** names a mapping by its `context`, its `action` and its
`default` key, as the definitions have them, rather than by its place, so
that a game may add mappings without moving a player's rebinds; `slot`,
from 0 and left out for 0, tells apart mappings of one action from one key
in one context. Its `key` is the key the player bound, or `null` for a
mapping unbound.

**Loading** is all or nothing, with errors as the definitions' are: a file
that is not JSON is malformed; one with a key the format does not have, a
value of the wrong type or a key name unknown is invalid; and the player's
rebinds are as they were. A binding of a mapping the input no longer has, a
context or an action gone or renamed, a default changed, is **passed over**,
so that a game's new defaults never refuse a player's old file; saving
again drops it. **Saving** writes the rebinds alone, in the order of the
contexts and their mappings, so the same rebinds write the same bytes, and
a file loaded and saved again is unchanged.

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
- **Rebinds by player, by name.** Unreal's user settings save a profile's
  key mappings by a mapping's name and slot; here a rebind is a player's,
  saved by its context, its action and its default key, which the
  definitions already give, with a slot only when one action has two
  mappings from one key.
- **Callbacks after evaluation.** Unreal's delegates run as each action is
  processed; here they run once every action of the player has been
  evaluated, so a callback sees the whole tick.

## Threading

An input is **stage-owned**: it, its players and its recordings are used on
one thread, the one that ticks it, usually the game loop's. Callbacks run on
that thread, inside `tb_input_tick()`. The functions on keys alone
(`tb_input_key_name()`, `tb_input_find_key()`, `tb_input_key_type()`,
`tb_input_key_class_of()`, `tb_input_key_prompt()`,
`tb_input_key_from_window()`, `tb_input_key_from_mouse_button()`,
`tb_input_key_from_gamepad_button()`, `tb_input_key_from_gamepad_axis()`) are
thread-safe.

## Still to come

- **Touch**, through `tb_input_feed_key()`.
- **Integration**: the game loop gathers and ticks input, the GUI's
  navigation reads `ui_*` actions, and Burrow's controls become actions.
- A file format for recordings.
