/* Burrow's player: the view's arithmetic, the controls, and flying. */
#include "burrow_player.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* The aspect `graphics.field_of_view` is given at: 16:9. */
#define BURROW_FOV_ASPECT (16.0f / 9.0f)

static const tb_vec3 burrow_up = {0.0f, 1.0f, 0.0f};
static const tb_vec3 burrow_across = {1.0f, 0.0f, 0.0f};
static const tb_vec3 burrow_ahead = {0.0f, 0.0f, -1.0f};

burrow_player burrow_player_spawn(tb_vec3 position, float yaw)
{
    burrow_player player = {0};

    player.position = position;
    player.previous_position = position;
    player.yaw = tb_wrap_angle(yaw);
    return player;
}

float burrow_clamp_pitch(float pitch)
{
    return tb_clamp(pitch, -BURROW_MOST_PITCH, BURROW_MOST_PITCH);
}

tb_quat burrow_view_orientation(float yaw, float pitch)
{
    const tb_quat turn = tb_quat_from_axis_angle(burrow_up, yaw);
    const tb_quat tilt = tb_quat_from_axis_angle(burrow_across, pitch);

    return tb_quat_normalize(tb_quat_mul(turn, tilt));
}

void burrow_view_basis(float yaw, float pitch, tb_vec3 *forward, tb_vec3 *right, tb_vec3 *up)
{
    const tb_quat orientation = burrow_view_orientation(yaw, pitch);

    *forward = tb_quat_rotate(orientation, burrow_ahead);
    *right = tb_quat_rotate(orientation, burrow_across);
    *up = tb_quat_rotate(orientation, burrow_up);
}

float burrow_vertical_fov(float horizontal_degrees)
{
    const float half_width = tanf(tb_radians(horizontal_degrees) * 0.5f);

    return 2.0f * atanf(half_width / BURROW_FOV_ASPECT);
}

/* +1 when only the first is held, -1 when only the second, else 0. */
static float burrow_axis(const bool held[BURROW_CONTROL_COUNT], burrow_control positive,
                         burrow_control negative)
{
    float value = 0.0f;

    if (held[positive])
    {
        value += 1.0f;
    }
    if (held[negative])
    {
        value -= 1.0f;
    }
    return value;
}

tb_vec3 burrow_move_direction(float yaw, const bool held[BURROW_CONTROL_COUNT])
{
    const float ahead = burrow_axis(held, BURROW_CONTROL_FORWARD, BURROW_CONTROL_BACK);
    const float aside = burrow_axis(held, BURROW_CONTROL_RIGHT, BURROW_CONTROL_LEFT);
    const float rise = burrow_axis(held, BURROW_CONTROL_UP, BURROW_CONTROL_DOWN);
    /* Level forward and right for the yaw alone: (0, 0, -1) and (1, 0, 0)
     * turned about +Y. */
    const tb_vec3 forward = {-sinf(yaw), 0.0f, -cosf(yaw)};
    const tb_vec3 right = {cosf(yaw), 0.0f, -sinf(yaw)};
    tb_vec3 move = tb_vec3_scale(forward, ahead);

    move = tb_vec3_add(move, tb_vec3_scale(right, aside));
    move.y += rise;
    return tb_vec3_normalize(move);
}

void burrow_player_look(burrow_player *player, double delta_x, double delta_y,
                        const burrow_look_options *options)
{
    const float step = BURROW_LOOK_RADIANS * options->sensitivity;
    const float vertical = options->invert_y ? -1.0f : 1.0f;

    /* The mouse to the right turns right, a negative yaw; toward the player
     * looks down, a negative pitch, unless inverted. */
    player->yaw = tb_wrap_angle(player->yaw - (float)delta_x * step);
    player->pitch = burrow_clamp_pitch(player->pitch - (float)delta_y * step * vertical);
}

void burrow_player_let_go(burrow_player *player)
{
    memset(player->held, 0, sizeof(player->held));
}

/* The keys and the controls they are bound to. */
static const struct
{
    tb_key key;
    burrow_control control;
} burrow_bindings[] = {
    {TB_KEY_W, BURROW_CONTROL_FORWARD},
    {TB_KEY_S, BURROW_CONTROL_BACK},
    {TB_KEY_A, BURROW_CONTROL_LEFT},
    {TB_KEY_D, BURROW_CONTROL_RIGHT},
    {TB_KEY_SPACE, BURROW_CONTROL_UP},
    {TB_KEY_LEFT_CONTROL, BURROW_CONTROL_DOWN},
    {TB_KEY_RIGHT_CONTROL, BURROW_CONTROL_DOWN},
    {TB_KEY_C, BURROW_CONTROL_DOWN},
    {TB_KEY_LEFT_SHIFT, BURROW_CONTROL_SPRINT},
    {TB_KEY_RIGHT_SHIFT, BURROW_CONTROL_SPRINT},
};

/* The control a key is bound to; BURROW_CONTROL_COUNT for none. */
static burrow_control burrow_control_of(tb_key key)
{
    for (size_t index = 0; index < sizeof(burrow_bindings) / sizeof(burrow_bindings[0]); index++)
    {
        if (burrow_bindings[index].key == key)
        {
            return burrow_bindings[index].control;
        }
    }
    return BURROW_CONTROL_COUNT;
}

static bool burrow_player_key(burrow_player *player, const tb_window_event *event,
                              burrow_mouse_request *request)
{
    const bool down = event->type == TB_WINDOW_EVENT_KEY_DOWN;
    const burrow_control control = burrow_control_of(event->key);

    if (event->key == TB_KEY_ESCAPE && down && player->captured)
    {
        *request = BURROW_MOUSE_RELEASE;
        return true;
    }
    if (control == BURROW_CONTROL_COUNT)
    {
        return false;
    }
    player->held[control] = down;
    return true;
}

bool burrow_player_event(burrow_player *player, const tb_window_event *event,
                         const burrow_look_options *options, burrow_mouse_request *request)
{
    const tb_window_event_type type = event->type;

    *request = BURROW_MOUSE_KEEP;
    if (type == TB_WINDOW_EVENT_KEY_DOWN || type == TB_WINDOW_EVENT_KEY_UP)
    {
        return burrow_player_key(player, event, request);
    }
    if (type == TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN && event->button == TB_MOUSE_BUTTON_LEFT &&
        !player->captured)
    {
        *request = BURROW_MOUSE_CAPTURE;
        return true;
    }
    if (type == TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION && player->captured)
    {
        burrow_player_look(player, event->delta_x, event->delta_y, options);
        return true;
    }
    if (type == TB_WINDOW_EVENT_FOCUS_LOST)
    {
        burrow_player_let_go(player);
    }
    return false;
}

void burrow_player_tick(burrow_player *player, double seconds)
{
    const tb_vec3 direction = burrow_move_direction(player->yaw, player->held);
    float speed = BURROW_FLY_SPEED;

    if (player->held[BURROW_CONTROL_SPRINT])
    {
        speed *= BURROW_SPRINT_FACTOR;
    }
    player->previous_position = player->position;
    player->position =
        tb_vec3_add(player->position, tb_vec3_scale(direction, speed * (float)seconds));
}

tb_vec3 burrow_player_eye(const burrow_player *player, double alpha)
{
    const float between = tb_clamp((float)alpha, 0.0f, 1.0f);

    return tb_vec3_lerp(player->previous_position, player->position, between);
}
