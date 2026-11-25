/* Burrow's menus as a state machine: the game starts at the main menu;
 * Play plays; Escape pauses; the pause menu resumes, opens the options,
 * leaves for the main menu, or quits; the options go back where they were
 * opened from; and a choice that means nothing on a screen changes
 * nothing. The main menu's camera circles the room, looking at it. */
#include "burrow_menu.h"

#include "tb_test.h"

#include <math.h>

TB_TEST(burrow_menu, the_game_starts_at_the_main_menu)
{
    const burrow_menu menu = burrow_menu_start();

    TB_TEST_EXPECT_INT(menu.screen, ==, BURROW_SCREEN_MAIN);
    TB_TEST_EXPECT(!menu.in_game);
    TB_TEST_EXPECT(!menu.quitting);
    TB_TEST_EXPECT(!burrow_menu_simulates(&menu));
}

TB_TEST(burrow_menu, play_pause_resume)
{
    burrow_menu menu = burrow_menu_start();

    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_PLAY));
    TB_TEST_EXPECT_INT(menu.screen, ==, BURROW_SCREEN_PLAYING);
    TB_TEST_EXPECT(menu.in_game);
    TB_TEST_EXPECT(burrow_menu_simulates(&menu));

    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_PAUSE));
    TB_TEST_EXPECT_INT(menu.screen, ==, BURROW_SCREEN_PAUSE);
    TB_TEST_EXPECT(!burrow_menu_simulates(&menu));
    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_RESUME));
    TB_TEST_EXPECT_INT(menu.screen, ==, BURROW_SCREEN_PLAYING);

    /* Back from the pause menu, Escape again, resumes as well. */
    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_PAUSE));
    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_BACK));
    TB_TEST_EXPECT_INT(menu.screen, ==, BURROW_SCREEN_PLAYING);
}

/* The options go back to the menu they came from. */
TB_TEST(burrow_menu, options_go_back_where_they_came_from)
{
    burrow_menu menu = burrow_menu_start();

    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_OPTIONS));
    TB_TEST_EXPECT_INT(menu.screen, ==, BURROW_SCREEN_OPTIONS);
    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_BACK));
    TB_TEST_EXPECT_INT(menu.screen, ==, BURROW_SCREEN_MAIN);

    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_PLAY));
    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_PAUSE));
    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_OPTIONS));
    TB_TEST_EXPECT(!burrow_menu_simulates(&menu));
    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_BACK));
    TB_TEST_EXPECT_INT(menu.screen, ==, BURROW_SCREEN_PAUSE);
}

/* Quitting to the menu ends the game under way; the next Play starts
 * another. */
TB_TEST(burrow_menu, quitting_to_the_menu_ends_the_game)
{
    burrow_menu menu = burrow_menu_start();

    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_PLAY));
    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_PAUSE));
    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_QUIT_TO_MENU));
    TB_TEST_EXPECT_INT(menu.screen, ==, BURROW_SCREEN_MAIN);
    TB_TEST_EXPECT(!menu.in_game);
}

TB_TEST(burrow_menu, quit_from_either_menu)
{
    burrow_menu menu = burrow_menu_start();

    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_QUIT));
    TB_TEST_EXPECT(menu.quitting);
    TB_TEST_EXPECT(!burrow_menu_simulates(&menu));
    /* Nothing more is chosen once quitting. */
    TB_TEST_EXPECT(!burrow_menu_choose(&menu, BURROW_CHOICE_PLAY));

    menu = burrow_menu_start();
    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_PLAY));
    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_PAUSE));
    TB_TEST_REQUIRE(burrow_menu_choose(&menu, BURROW_CHOICE_QUIT));
    TB_TEST_EXPECT(menu.quitting);
}

/* What a screen does not offer changes nothing: the main menu has no
 * Resume and Back does not leave it; playing only pauses; the options only
 * go back. */
TB_TEST(burrow_menu, a_choice_a_screen_does_not_offer_changes_nothing)
{
    static const struct
    {
        burrow_screen screen;
        burrow_choice choice;
    } refused[] = {
        {BURROW_SCREEN_MAIN, BURROW_CHOICE_RESUME},
        {BURROW_SCREEN_MAIN, BURROW_CHOICE_BACK},
        {BURROW_SCREEN_MAIN, BURROW_CHOICE_PAUSE},
        {BURROW_SCREEN_MAIN, BURROW_CHOICE_QUIT_TO_MENU},
        {BURROW_SCREEN_PLAYING, BURROW_CHOICE_PLAY},
        {BURROW_SCREEN_PLAYING, BURROW_CHOICE_BACK},
        {BURROW_SCREEN_PLAYING, BURROW_CHOICE_QUIT},
        {BURROW_SCREEN_PAUSE, BURROW_CHOICE_PLAY},
        {BURROW_SCREEN_PAUSE, BURROW_CHOICE_PAUSE},
        {BURROW_SCREEN_OPTIONS, BURROW_CHOICE_PLAY},
        {BURROW_SCREEN_OPTIONS, BURROW_CHOICE_QUIT},
        {BURROW_SCREEN_COUNT, BURROW_CHOICE_BACK},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(refused); index++)
    {
        burrow_menu menu = burrow_menu_start();
        menu.screen = refused[index].screen;
        TB_TEST_CONTEXT("case %zu", index);
        TB_TEST_EXPECT(!burrow_menu_choose(&menu, refused[index].choice));
        TB_TEST_EXPECT_INT(menu.screen, ==, refused[index].screen);
        TB_TEST_EXPECT(!menu.quitting);
    }
    tb_test_context_clear();
}

TB_TEST(burrow_menu, screens_and_choices_have_names)
{
    TB_TEST_EXPECT_STRING(burrow_screen_name(BURROW_SCREEN_PLAYING), ==, "playing");
    TB_TEST_EXPECT_STRING(burrow_screen_name(BURROW_SCREEN_OPTIONS), ==, "options");
    TB_TEST_EXPECT_STRING(burrow_screen_name(BURROW_SCREEN_COUNT), ==, "");
    TB_TEST_EXPECT_INT(burrow_choice_named("quit_to_menu"), ==, BURROW_CHOICE_QUIT_TO_MENU);
    TB_TEST_EXPECT_INT(burrow_choice_named("back"), ==, BURROW_CHOICE_BACK);
    TB_TEST_EXPECT_INT(burrow_choice_named("jump"), ==, BURROW_CHOICE_COUNT);
}

/* The camera goes once around the room in its time, at one height and one
 * distance, looking down at the middle all the way. */
TB_TEST(burrow_menu, the_main_menu_camera_circles_the_room)
{
    tb_vec3 start = {0};
    tb_vec3 eye = {0};
    float yaw = 0.0f;
    float pitch = 0.0f;

    burrow_orbit_view(0.0, &start, &yaw, &pitch);
    TB_TEST_EXPECT_FLOAT_NEAR(start.x, 0.0, 1e-5);
    TB_TEST_EXPECT_FLOAT(start.z, >, 0.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(yaw, 0.0, 1e-6);
    TB_TEST_EXPECT_FLOAT(pitch, <, 0.0f);

    for (int step = 1; step < 8; step++)
    {
        const double seconds = BURROW_ORBIT_SECONDS * step / 8.0;
        burrow_orbit_view(seconds, &eye, &yaw, &pitch);
        TB_TEST_EXPECT_FLOAT_NEAR(eye.y, start.y, 1e-5);
        TB_TEST_EXPECT_FLOAT_NEAR(sqrtf(eye.x * eye.x + eye.z * eye.z), start.z, 1e-4);
        /* Looking at the middle: the view's level direction points back
         * along the eye's place from it. */
        const float toward_x = -sinf(yaw);
        const float toward_z = -cosf(yaw);
        TB_TEST_EXPECT_FLOAT_NEAR(toward_x * eye.x + toward_z * eye.z, -start.z, 1e-3);
    }
    burrow_orbit_view(BURROW_ORBIT_SECONDS, &eye, &yaw, &pitch);
    TB_TEST_EXPECT_FLOAT_NEAR(eye.x, start.x, 1e-4);
    TB_TEST_EXPECT_FLOAT_NEAR(eye.z, start.z, 1e-4);
}
