/* Burrow's player: a first-person camera that flies through the room
 * (noclip), steered by the mouse and the keyboard.
 *
 * The view turns as the mouse moves, at once, so that looking never waits
 * for a tick; the player moves at the game loop's fixed ticks, and each frame
 * draws from between the last two, as the loop's `alpha` says. Everything
 * here is arithmetic on the player's state and the window's events: the game
 * applies what it asks of the window, such as capturing the mouse.
 *
 * Conventions are the engine's: right-handed, Y up, looking down -Z when the
 * yaw and pitch are 0. A positive yaw turns left; a positive pitch looks up.
 *
 * Threading: externally synchronized; the game calls everything on the
 * thread that runs its loop. */
#ifndef BURROW_PLAYER_H
#define BURROW_PLAYER_H

#include "badger/math.h"
#include "badger/window.h"

#include <stdbool.h>

/* The most the view looks up or down: 89 degrees, short of straight up,
 * where yaw would lose its meaning. */
#define BURROW_MOST_PITCH 1.5533430f

/* Meters a second the player flies, and how much faster with sprint held. */
#define BURROW_FLY_SPEED 6.0f
#define BURROW_SPRINT_FACTOR 2.5f

/* Radians the view turns for a unit of the mouse's motion at a
 * sensitivity of 1. */
#define BURROW_LOOK_RADIANS 0.0022f

/* What the player holds down to move. */
typedef enum burrow_control
{
    BURROW_CONTROL_FORWARD, /* W */
    BURROW_CONTROL_BACK,    /* S */
    BURROW_CONTROL_LEFT,    /* A */
    BURROW_CONTROL_RIGHT,   /* D */
    BURROW_CONTROL_UP,      /* Space */
    BURROW_CONTROL_DOWN,    /* Control or C */
    BURROW_CONTROL_SPRINT,  /* Shift */
    BURROW_CONTROL_COUNT
} burrow_control;

/* What the player asks of the mouse, which the game asks of the window. */
typedef enum burrow_mouse_request
{
    BURROW_MOUSE_KEEP,    /* Leave it as it is. */
    BURROW_MOUSE_CAPTURE, /* Hide it and hold it, for looking: a click. */
    BURROW_MOUSE_RELEASE  /* Give it back: Escape. */
} burrow_mouse_request;

/* How the mouse turns the view, from the `controls.mouse` settings. */
typedef struct burrow_look_options
{
    float sensitivity; /* 1 turns BURROW_LOOK_RADIANS a unit. */
    bool invert_y;     /* Moving the mouse forward looks down. */
} burrow_look_options;

/* The player. Start from burrow_player_spawn(). */
typedef struct burrow_player
{
    tb_vec3 position;          /* The eye, after the last tick. */
    tb_vec3 previous_position; /* The eye before the last tick. */
    float yaw;                 /* Radians about +Y, from -pi to pi. */
    float pitch;               /* Radians, within BURROW_MOST_PITCH of level. */
    bool held[BURROW_CONTROL_COUNT];
    bool captured; /* The mouse is captured, and turns the view. */
} burrow_player;

/* A player at `position`, looking level at `yaw`, holding nothing, the
 * mouse not yet captured. */
burrow_player burrow_player_spawn(tb_vec3 position, float yaw);

/* `pitch` held within BURROW_MOST_PITCH of level. */
float burrow_clamp_pitch(float pitch);

/* The view's rotation from looking down -Z: the yaw about +Y, then the pitch
 * about the turned +X. */
tb_quat burrow_view_orientation(float yaw, float pitch);

/* The view's axes for a yaw and a pitch, each of unit length: where it
 * looks, its right, and its up. Right stays level whatever the pitch. */
void burrow_view_basis(float yaw, float pitch, tb_vec3 *forward, tb_vec3 *right, tb_vec3 *up);

/* The vertical field of view, in radians, of `graphics.field_of_view`,
 * which is horizontal degrees on a 16:9 display. A wider display sees more
 * at the sides, and a narrower one less, at the same vertical view. */
float burrow_vertical_fov(float horizontal_degrees);

/* The way the controls held move the player, of unit length, or zero when
 * they cancel out or none is held. Forward, back, left and right are level,
 * relative to the yaw alone, so looking down does not fly into the floor;
 * up and down are along +Y. */
tb_vec3 burrow_move_direction(float yaw, const bool held[BURROW_CONTROL_COUNT]);

/* Turn the view by the mouse's motion, in the window's relative units:
 * positive `delta_x` is to the right, positive `delta_y` toward the player. */
void burrow_player_look(burrow_player *player, double delta_x, double delta_y,
                        const burrow_look_options *options);

/* Let go of every control, as when the window loses the focus and its keys
 * come up unseen. */
void burrow_player_let_go(burrow_player *player);

/* A window event, for the player: the movement keys, the mouse's motion
 * while it is captured, a click to capture it and Escape to release it.
 * Sets `request` to what the game should ask of the mouse. Returns true when
 * the player took the event, so that the game loop does not see it: Escape
 * that releases the mouse does not also quit. */
bool burrow_player_event(burrow_player *player, const tb_window_event *event,
                         const burrow_look_options *options, burrow_mouse_request *request);

/* One fixed tick of `seconds`: the player moves as the controls say. */
void burrow_player_tick(burrow_player *player, double seconds);

/* Where the eye is drawn, `alpha` of the way from the tick before the last
 * to the last: 0 to 1, as the game loop gives it. */
tb_vec3 burrow_player_eye(const burrow_player *player, double alpha);

#endif
