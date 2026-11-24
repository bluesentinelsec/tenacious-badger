/* Burrow's player must look the way the mouse moves, never past straight up
 * or down, fly relative to where it faces but level whatever the pitch, at
 * a speed that does not depend on how many keys are held, and draw from
 * between its last two ticks. Its events must capture the mouse on a click,
 * free it on Escape without quitting, and let go of every key when the
 * window loses the focus. */
#include "burrow_player.h"

#include "tb_test.h"

#include <math.h>

#define NEAR 1e-5

static void expect_vec3(tb_vec3 actual, tb_vec3 expected)
{
    TB_TEST_EXPECT_FLOAT_NEAR(actual.x, expected.x, NEAR);
    TB_TEST_EXPECT_FLOAT_NEAR(actual.y, expected.y, NEAR);
    TB_TEST_EXPECT_FLOAT_NEAR(actual.z, expected.z, NEAR);
}

static tb_window_event key_event(tb_key key, bool down)
{
    tb_window_event event = {0};

    event.type = down ? TB_WINDOW_EVENT_KEY_DOWN : TB_WINDOW_EVENT_KEY_UP;
    event.key = key;
    return event;
}

static const burrow_look_options plain_look = {.sensitivity = 1.0f, .invert_y = false};

TB_TEST(burrow_player, spawns_level_holding_nothing_and_not_yet_looking)
{
    const burrow_player player = burrow_player_spawn((tb_vec3){1.0f, 2.0f, 3.0f}, TB_TAU + 0.5f);

    expect_vec3(player.position, (tb_vec3){1.0f, 2.0f, 3.0f});
    expect_vec3(player.previous_position, player.position);
    /* A yaw past a full turn is the same direction, kept within a turn. */
    TB_TEST_EXPECT_FLOAT_NEAR(player.yaw, 0.5f, NEAR);
    TB_TEST_EXPECT_FLOAT(player.pitch, ==, 0.0f);
    TB_TEST_EXPECT(!player.captured);
    for (int control = 0; control < BURROW_CONTROL_COUNT; control++)
    {
        TB_TEST_EXPECT(!player.held[control]);
    }
}

TB_TEST(burrow_player, a_level_view_looks_down_negative_z)
{
    tb_vec3 forward = {0};
    tb_vec3 right = {0};
    tb_vec3 up = {0};

    burrow_view_basis(0.0f, 0.0f, &forward, &right, &up);
    expect_vec3(forward, (tb_vec3){0.0f, 0.0f, -1.0f});
    expect_vec3(right, (tb_vec3){1.0f, 0.0f, 0.0f});
    expect_vec3(up, (tb_vec3){0.0f, 1.0f, 0.0f});
}

TB_TEST(burrow_player, a_positive_yaw_turns_left)
{
    tb_vec3 forward = {0};
    tb_vec3 right = {0};
    tb_vec3 up = {0};

    burrow_view_basis(TB_PI * 0.5f, 0.0f, &forward, &right, &up);
    expect_vec3(forward, (tb_vec3){-1.0f, 0.0f, 0.0f});
    expect_vec3(right, (tb_vec3){0.0f, 0.0f, -1.0f});
    expect_vec3(up, (tb_vec3){0.0f, 1.0f, 0.0f});
}

TB_TEST(burrow_player, a_positive_pitch_looks_up_and_keeps_right_level)
{
    const float half = sqrtf(0.5f);
    tb_vec3 forward = {0};
    tb_vec3 right = {0};
    tb_vec3 up = {0};

    burrow_view_basis(0.0f, TB_PI * 0.25f, &forward, &right, &up);
    expect_vec3(forward, (tb_vec3){0.0f, half, -half});
    expect_vec3(right, (tb_vec3){1.0f, 0.0f, 0.0f});
    expect_vec3(up, (tb_vec3){0.0f, half, half});
}

TB_TEST(burrow_player, every_basis_is_of_unit_axes_at_right_angles)
{
    static const float yaws[] = {-3.0f, -1.2f, 0.0f, 0.7f, 2.9f};
    static const float pitches[] = {-1.5f, -0.4f, 0.0f, 0.9f, 1.5f};

    for (size_t a = 0; a < TB_TEST_COUNT_OF(yaws); a++)
    {
        for (size_t b = 0; b < TB_TEST_COUNT_OF(pitches); b++)
        {
            tb_vec3 forward = {0};
            tb_vec3 right = {0};
            tb_vec3 up = {0};
            burrow_view_basis(yaws[a], pitches[b], &forward, &right, &up);
            TB_TEST_CONTEXT("yaw %g, pitch %g", (double)yaws[a], (double)pitches[b]);
            TB_TEST_EXPECT_FLOAT_NEAR(tb_vec3_length(forward), 1.0, NEAR);
            TB_TEST_EXPECT_FLOAT_NEAR(tb_vec3_length(right), 1.0, NEAR);
            TB_TEST_EXPECT_FLOAT_NEAR(tb_vec3_dot(forward, right), 0.0, NEAR);
            TB_TEST_EXPECT_FLOAT_NEAR(tb_vec3_dot(forward, up), 0.0, NEAR);
            /* Right-handed: right cross up is backward. */
            expect_vec3(tb_vec3_cross(right, up), tb_vec3_negate(forward));
            TB_TEST_EXPECT_FLOAT_NEAR(right.y, 0.0, NEAR);
            TB_TEST_EXPECT_FLOAT_NEAR(forward.y, sinf(pitches[b]), NEAR);
        }
    }
}

TB_TEST(burrow_player, the_orientation_turns_the_camera_to_the_basis)
{
    const tb_quat orientation = burrow_view_orientation(0.8f, -0.3f);
    tb_vec3 forward = {0};
    tb_vec3 right = {0};
    tb_vec3 up = {0};

    burrow_view_basis(0.8f, -0.3f, &forward, &right, &up);
    /* A camera looks down -Z before its orientation turns it. */
    expect_vec3(tb_quat_rotate(orientation, (tb_vec3){0.0f, 0.0f, -1.0f}), forward);
    expect_vec3(tb_quat_rotate(orientation, (tb_vec3){1.0f, 0.0f, 0.0f}), right);
}

TB_TEST(burrow_player, pitch_stops_short_of_straight_up_and_down)
{
    TB_TEST_EXPECT_FLOAT(burrow_clamp_pitch(3.0f), ==, BURROW_MOST_PITCH);
    TB_TEST_EXPECT_FLOAT(burrow_clamp_pitch(-3.0f), ==, -BURROW_MOST_PITCH);
    TB_TEST_EXPECT_FLOAT(burrow_clamp_pitch(0.25f), ==, 0.25f);
    TB_TEST_EXPECT(BURROW_MOST_PITCH < TB_PI * 0.5f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_degrees(BURROW_MOST_PITCH), 89.0, 1e-3);
}

TB_TEST(burrow_player, the_mouse_turns_the_view_the_way_it_moves)
{
    burrow_player player = burrow_player_spawn((tb_vec3){0}, 0.0f);

    /* To the right turns right: a negative yaw. */
    burrow_player_look(&player, 100.0, 0.0, &plain_look);
    TB_TEST_EXPECT_FLOAT_NEAR(player.yaw, -100.0f * BURROW_LOOK_RADIANS, NEAR);
    /* Toward the player looks down. */
    burrow_player_look(&player, 0.0, 50.0, &plain_look);
    TB_TEST_EXPECT_FLOAT_NEAR(player.pitch, -50.0f * BURROW_LOOK_RADIANS, NEAR);
}

TB_TEST(burrow_player, sensitivity_scales_and_inverting_flips_the_vertical)
{
    const burrow_look_options inverted = {.sensitivity = 2.0f, .invert_y = true};
    burrow_player player = burrow_player_spawn((tb_vec3){0}, 0.0f);

    burrow_player_look(&player, 10.0, 10.0, &inverted);
    TB_TEST_EXPECT_FLOAT_NEAR(player.yaw, -20.0f * BURROW_LOOK_RADIANS, NEAR);
    TB_TEST_EXPECT_FLOAT_NEAR(player.pitch, 20.0f * BURROW_LOOK_RADIANS, NEAR);
}

TB_TEST(burrow_player, looking_far_holds_the_pitch_and_wraps_the_yaw)
{
    burrow_player player = burrow_player_spawn((tb_vec3){0}, 0.0f);
    const double full_turn = (double)(TB_TAU / BURROW_LOOK_RADIANS);

    burrow_player_look(&player, 0.0, -100000.0, &plain_look);
    TB_TEST_EXPECT_FLOAT(player.pitch, ==, BURROW_MOST_PITCH);
    burrow_player_look(&player, 0.0, 200000.0, &plain_look);
    TB_TEST_EXPECT_FLOAT(player.pitch, ==, -BURROW_MOST_PITCH);
    /* A turn and a quarter to the left ends a quarter turn round, within a turn. */
    burrow_player_look(&player, -full_turn * 1.25, 0.0, &plain_look);
    TB_TEST_EXPECT(player.yaw >= -TB_PI && player.yaw <= TB_PI);
    TB_TEST_EXPECT_FLOAT_NEAR(fabsf(player.yaw), TB_PI * 0.5f, 1e-3);
}

TB_TEST(burrow_player, the_field_of_view_is_horizontal_at_sixteen_by_nine)
{
    /* 90 degrees across a 16:9 view is about 58.7 degrees up and down. */
    TB_TEST_EXPECT_FLOAT_NEAR(tb_degrees(burrow_vertical_fov(90.0f)), 58.7155, 1e-3);
    for (float degrees = 60.0f; degrees <= 120.0f; degrees += 15.0f)
    {
        const float vertical = burrow_vertical_fov(degrees);
        TB_TEST_CONTEXT("%g degrees", (double)degrees);
        TB_TEST_EXPECT_FLOAT_NEAR(tanf(vertical * 0.5f) * 16.0f / 9.0f,
                                  tanf(tb_radians(degrees) * 0.5f), NEAR);
    }
}

TB_TEST(burrow_player, moving_follows_the_yaw_and_stays_level)
{
    bool held[BURROW_CONTROL_COUNT] = {0};

    held[BURROW_CONTROL_FORWARD] = true;
    expect_vec3(burrow_move_direction(0.0f, held), (tb_vec3){0.0f, 0.0f, -1.0f});
    expect_vec3(burrow_move_direction(TB_PI * 0.5f, held), (tb_vec3){-1.0f, 0.0f, 0.0f});
    held[BURROW_CONTROL_FORWARD] = false;
    held[BURROW_CONTROL_RIGHT] = true;
    expect_vec3(burrow_move_direction(0.0f, held), (tb_vec3){1.0f, 0.0f, 0.0f});
    expect_vec3(burrow_move_direction(TB_PI * 0.5f, held), (tb_vec3){0.0f, 0.0f, -1.0f});
    held[BURROW_CONTROL_RIGHT] = false;
    held[BURROW_CONTROL_BACK] = true;
    expect_vec3(burrow_move_direction(TB_PI, held), (tb_vec3){0.0f, 0.0f, -1.0f});
}

TB_TEST(burrow_player, moving_is_as_fast_whatever_is_held)
{
    const float half = sqrtf(0.5f);
    bool held[BURROW_CONTROL_COUNT] = {0};

    expect_vec3(burrow_move_direction(0.3f, held), (tb_vec3){0});
    held[BURROW_CONTROL_FORWARD] = true;
    held[BURROW_CONTROL_LEFT] = true;
    expect_vec3(burrow_move_direction(0.0f, held), (tb_vec3){-half, 0.0f, -half});
    held[BURROW_CONTROL_UP] = true;
    TB_TEST_EXPECT_FLOAT_NEAR(tb_vec3_length(burrow_move_direction(1.0f, held)), 1.0, NEAR);
    /* Opposites cancel. */
    held[BURROW_CONTROL_FORWARD] = false;
    held[BURROW_CONTROL_LEFT] = false;
    held[BURROW_CONTROL_DOWN] = true;
    expect_vec3(burrow_move_direction(0.0f, held), (tb_vec3){0});
    held[BURROW_CONTROL_UP] = false;
    expect_vec3(burrow_move_direction(0.0f, held), (tb_vec3){0.0f, -1.0f, 0.0f});
}

TB_TEST(burrow_player, a_tick_flies_at_the_speed_and_sprinting_is_faster)
{
    burrow_player player = burrow_player_spawn((tb_vec3){0.0f, 1.0f, 0.0f}, 0.0f);

    player.pitch = -1.0f; /* looking down does not fly into the floor */
    player.held[BURROW_CONTROL_FORWARD] = true;
    burrow_player_tick(&player, 0.5);
    expect_vec3(player.previous_position, (tb_vec3){0.0f, 1.0f, 0.0f});
    expect_vec3(player.position, (tb_vec3){0.0f, 1.0f, -0.5f * BURROW_FLY_SPEED});
    player.held[BURROW_CONTROL_SPRINT] = true;
    burrow_player_tick(&player, 0.5);
    expect_vec3(player.position,
                (tb_vec3){0.0f, 1.0f, -0.5f * BURROW_FLY_SPEED * (1.0f + BURROW_SPRINT_FACTOR)});
    /* Nothing held: the player stays, and so does where it is drawn. */
    burrow_player_let_go(&player);
    burrow_player_tick(&player, 0.5);
    expect_vec3(player.previous_position, player.position);
}

TB_TEST(burrow_player, frames_draw_between_the_last_two_ticks)
{
    burrow_player player = burrow_player_spawn((tb_vec3){0}, 0.0f);

    player.held[BURROW_CONTROL_UP] = true;
    burrow_player_tick(&player, 1.0);
    expect_vec3(burrow_player_eye(&player, 0.0), (tb_vec3){0});
    expect_vec3(burrow_player_eye(&player, 0.25), (tb_vec3){0.0f, 0.25f * BURROW_FLY_SPEED, 0.0f});
    expect_vec3(burrow_player_eye(&player, 1.0), player.position);
    /* Past either end, the ends. */
    expect_vec3(burrow_player_eye(&player, 2.0), player.position);
    expect_vec3(burrow_player_eye(&player, -1.0), (tb_vec3){0});
}

TB_TEST(burrow_player, keys_hold_and_release_their_controls)
{
    static const struct
    {
        tb_key key;
        burrow_control control;
    } bound[] = {{TB_KEY_W, BURROW_CONTROL_FORWARD},
                 {TB_KEY_S, BURROW_CONTROL_BACK},
                 {TB_KEY_A, BURROW_CONTROL_LEFT},
                 {TB_KEY_D, BURROW_CONTROL_RIGHT},
                 {TB_KEY_SPACE, BURROW_CONTROL_UP},
                 {TB_KEY_LEFT_CONTROL, BURROW_CONTROL_DOWN},
                 {TB_KEY_C, BURROW_CONTROL_DOWN},
                 {TB_KEY_RIGHT_CONTROL, BURROW_CONTROL_DOWN},
                 {TB_KEY_LEFT_SHIFT, BURROW_CONTROL_SPRINT},
                 {TB_KEY_RIGHT_SHIFT, BURROW_CONTROL_SPRINT}};
    burrow_player player = burrow_player_spawn((tb_vec3){0}, 0.0f);
    burrow_mouse_request request = BURROW_MOUSE_CAPTURE;

    for (size_t index = 0; index < TB_TEST_COUNT_OF(bound); index++)
    {
        tb_window_event event = key_event(bound[index].key, true);
        TB_TEST_CONTEXT("%s", tb_key_name(bound[index].key));
        TB_TEST_EXPECT(burrow_player_event(&player, &event, &plain_look, &request));
        TB_TEST_EXPECT(player.held[bound[index].control]);
        TB_TEST_EXPECT_INT(request, ==, BURROW_MOUSE_KEEP);
        event = key_event(bound[index].key, false);
        TB_TEST_EXPECT(burrow_player_event(&player, &event, &plain_look, &request));
        TB_TEST_EXPECT(!player.held[bound[index].control]);
    }
    /* A key bound to nothing is left to the game loop. */
    const tb_window_event other = key_event(TB_KEY_F1, true);
    TB_TEST_EXPECT(!burrow_player_event(&player, &other, &plain_look, &request));
}

TB_TEST(burrow_player, escape_frees_the_mouse_and_only_then_quits)
{
    burrow_player player = burrow_player_spawn((tb_vec3){0}, 0.0f);
    burrow_mouse_request request = BURROW_MOUSE_KEEP;
    const tb_window_event escape = key_event(TB_KEY_ESCAPE, true);
    const tb_window_event escape_up = key_event(TB_KEY_ESCAPE, false);

    /* Looking, Escape frees the mouse, and the loop does not see it. */
    player.captured = true;
    TB_TEST_EXPECT(burrow_player_event(&player, &escape, &plain_look, &request));
    TB_TEST_EXPECT_INT(request, ==, BURROW_MOUSE_RELEASE);
    TB_TEST_EXPECT(!burrow_player_event(&player, &escape_up, &plain_look, &request));
    TB_TEST_EXPECT_INT(request, ==, BURROW_MOUSE_KEEP);
    /* Free, Escape is the loop's, which quits. */
    player.captured = false;
    TB_TEST_EXPECT(!burrow_player_event(&player, &escape, &plain_look, &request));
    TB_TEST_EXPECT_INT(request, ==, BURROW_MOUSE_KEEP);
}

TB_TEST(burrow_player, a_left_click_captures_a_free_mouse)
{
    burrow_player player = burrow_player_spawn((tb_vec3){0}, 0.0f);
    burrow_mouse_request request = BURROW_MOUSE_KEEP;
    tb_window_event click = {.type = TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN,
                             .button = TB_MOUSE_BUTTON_LEFT};

    TB_TEST_EXPECT(burrow_player_event(&player, &click, &plain_look, &request));
    TB_TEST_EXPECT_INT(request, ==, BURROW_MOUSE_CAPTURE);
    /* Once captured, a click is left for firing, later. */
    player.captured = true;
    TB_TEST_EXPECT(!burrow_player_event(&player, &click, &plain_look, &request));
    TB_TEST_EXPECT_INT(request, ==, BURROW_MOUSE_KEEP);
    player.captured = false;
    click.button = TB_MOUSE_BUTTON_RIGHT;
    TB_TEST_EXPECT(!burrow_player_event(&player, &click, &plain_look, &request));
    TB_TEST_EXPECT_INT(request, ==, BURROW_MOUSE_KEEP);
}

TB_TEST(burrow_player, the_mouse_turns_the_view_only_while_captured)
{
    burrow_player player = burrow_player_spawn((tb_vec3){0}, 0.0f);
    burrow_mouse_request request = BURROW_MOUSE_KEEP;
    const tb_window_event motion = {
        .type = TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION, .delta_x = 10.0, .delta_y = -10.0};

    TB_TEST_EXPECT(!burrow_player_event(&player, &motion, &plain_look, &request));
    TB_TEST_EXPECT_FLOAT(player.yaw, ==, 0.0f);
    player.captured = true;
    TB_TEST_EXPECT(burrow_player_event(&player, &motion, &plain_look, &request));
    TB_TEST_EXPECT_FLOAT_NEAR(player.yaw, -10.0f * BURROW_LOOK_RADIANS, NEAR);
    TB_TEST_EXPECT_FLOAT_NEAR(player.pitch, 10.0f * BURROW_LOOK_RADIANS, NEAR);
}

TB_TEST(burrow_player, losing_the_focus_lets_go_of_every_key)
{
    burrow_player player = burrow_player_spawn((tb_vec3){0}, 0.0f);
    burrow_mouse_request request = BURROW_MOUSE_KEEP;
    const tb_window_event lost = {.type = TB_WINDOW_EVENT_FOCUS_LOST};

    player.held[BURROW_CONTROL_FORWARD] = true;
    player.held[BURROW_CONTROL_SPRINT] = true;
    /* The loop sees it too, to pause. */
    TB_TEST_EXPECT(!burrow_player_event(&player, &lost, &plain_look, &request));
    TB_TEST_EXPECT(!player.held[BURROW_CONTROL_FORWARD]);
    TB_TEST_EXPECT(!player.held[BURROW_CONTROL_SPRINT]);
    TB_TEST_EXPECT_INT(request, ==, BURROW_MOUSE_KEEP);
}
