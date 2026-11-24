/* Burrow's HUD must show a frame rate that can be read: the first frame's
 * at once, then an average that changes a few times a second, not every
 * frame. */
#include "burrow_hud.h"

#include "tb_test.h"

TB_TEST(burrow_hud, no_frame_shows_no_rate)
{
    const burrow_frame_rate rate = {0};

    TB_TEST_EXPECT_FLOAT(burrow_frame_rate_per_second(&rate), ==, 0.0);
}

TB_TEST(burrow_hud, the_first_frame_shows_at_once)
{
    burrow_frame_rate rate = {0};

    TB_TEST_EXPECT(burrow_frame_rate_add(&rate, 0.02));
    TB_TEST_EXPECT_FLOAT_NEAR(burrow_frame_rate_per_second(&rate), 50.0, 1e-9);
}

TB_TEST(burrow_hud, the_rate_shown_changes_a_few_times_a_second)
{
    burrow_frame_rate rate = {0};
    int changes = 0;

    /* Frames of a 64th of a second, which binary fractions hold exactly. */
    const double frame_seconds = 1.0 / 64.0;

    (void)burrow_frame_rate_add(&rate, frame_seconds);
    /* A second of frames: four changes, one every quarter. */
    for (int frame = 0; frame < 64; frame++)
    {
        changes += burrow_frame_rate_add(&rate, frame_seconds) ? 1 : 0;
    }
    TB_TEST_EXPECT_INT(changes, ==, (int)(1.0 / BURROW_RATE_SHOWN_EVERY));
    TB_TEST_EXPECT_FLOAT_NEAR(burrow_frame_rate_per_second(&rate), 64.0, 1e-9);
}

TB_TEST(burrow_hud, the_average_moves_toward_slower_frames)
{
    burrow_frame_rate rate = {0};

    (void)burrow_frame_rate_add(&rate, 0.01);
    (void)burrow_frame_rate_add(&rate, 0.25);
    /* Shown a quarter second after the first: a tenth of the way from the
     * fast frame to the slow one, so that one hitch does not swing it. */
    TB_TEST_EXPECT_FLOAT_NEAR(burrow_frame_rate_per_second(&rate), 1.0 / 0.034, 1e-6);
}
