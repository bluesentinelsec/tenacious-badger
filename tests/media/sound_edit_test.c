/* Changing sounds: formats converted exactly where they can be and rounded
 * where they cannot, channels mixed by the documented gains, rates changed
 * by linear interpolation, gains, fades, normalizing, and the frames and
 * loops that reversing, trimming, and appending leave. */
#include "badger/sound.h"
#include "tb_test.h"

#include <stdint.h>
#include <string.h>

static tb_sound *make_mono(tb_sound_format format, uint32_t rate, const float *values, size_t count)
{
    const tb_sound_spec spec = {.format = format, .sample_rate = rate, .channels = 1};
    tb_sound *sound = NULL;
    TB_TEST_REQUIRE_UINT(tb_sound_create(&spec, count, NULL, &sound), ==, TB_SOUND_OK);
    tb_sound_write_frames(sound, 0, count, values, count);
    return sound;
}

/* Every 16-bit value survives a trip through F32, and so does every value
 * of U8 and S24 that this sweep reaches. */
TB_TEST(media_sound_edit, integer_formats_round_trip_exactly_through_f32)
{
    enum
    {
        COUNT = 65536
    };
    const tb_sound_spec spec = {.format = TB_SOUND_FORMAT_S16, .sample_rate = 8000, .channels = 1};
    static int16_t values[COUNT];
    tb_sound *sound = NULL;
    size_t length = 0;
    for (size_t index = 0; index < COUNT; index++)
    {
        values[index] = (int16_t)((int32_t)index - 32768);
    }
    TB_TEST_REQUIRE_UINT(tb_sound_create_copy(&spec, values, sizeof(values), NULL, &sound), ==,
                         TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_format(sound, TB_SOUND_FORMAT_F32), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_format(sound, TB_SOUND_FORMAT_S24), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_format(sound, TB_SOUND_FORMAT_S32), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_format(sound, TB_SOUND_FORMAT_S16), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_BYTES(tb_sound_samples(sound, &length), values, sizeof(values));
    tb_sound_destroy(sound);

    const tb_sound_spec bytes_spec = {
        .format = TB_SOUND_FORMAT_U8, .sample_rate = 8000, .channels = 1};
    unsigned char bytes[256] = {0};
    for (size_t index = 0; index < sizeof(bytes); index++)
    {
        bytes[index] = (unsigned char)index;
    }
    TB_TEST_REQUIRE_UINT(tb_sound_create_copy(&bytes_spec, bytes, sizeof(bytes), NULL, &sound), ==,
                         TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_format(sound, TB_SOUND_FORMAT_F32), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_format(sound, TB_SOUND_FORMAT_U8), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_BYTES(tb_sound_samples(sound, &length), bytes, sizeof(bytes));
    tb_sound_destroy(sound);
}

/* Converting to fewer bits rounds to nearest, halves away from zero, and
 * clamps what is past full scale; S32 keeps 24 bits through F32. */
TB_TEST(media_sound_edit, fewer_bits_round_and_clamp)
{
    const float values[5] = {1.5f, -2.0f, 0.5f / 128.0f, -0.5f / 128.0f, 0.49f / 128.0f};
    tb_sound *sound = make_mono(TB_SOUND_FORMAT_F32, 8000, values, 5);
    size_t length = 0;
    TB_TEST_REQUIRE_UINT(tb_sound_convert_format(sound, TB_SOUND_FORMAT_U8), ==, TB_SOUND_OK);
    const unsigned char *const bytes = tb_sound_samples(sound, &length);
    TB_TEST_EXPECT_UINT(bytes[0], ==, 255);
    TB_TEST_EXPECT_UINT(bytes[1], ==, 0);
    TB_TEST_EXPECT_UINT(bytes[2], ==, 129);
    TB_TEST_EXPECT_UINT(bytes[3], ==, 127);
    TB_TEST_EXPECT_UINT(bytes[4], ==, 128);
    tb_sound_destroy(sound);

    const tb_sound_spec spec = {.format = TB_SOUND_FORMAT_S32, .sample_rate = 8000, .channels = 1};
    const int32_t wide[2] = {0x12345678, -0x7FFFFFFF};
    TB_TEST_REQUIRE_UINT(tb_sound_create_copy(&spec, wide, sizeof(wide), NULL, &sound), ==,
                         TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_format(sound, TB_SOUND_FORMAT_F32), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_format(sound, TB_SOUND_FORMAT_S32), ==, TB_SOUND_OK);
    const int32_t *const back = tb_sound_samples(sound, &length);
    TB_TEST_EXPECT_HEX((uint32_t)back[0] & 0xFFFFFF00u, ==, 0x12345600u);
    TB_TEST_EXPECT_INT(back[1], ==, INT32_MIN);
    tb_sound_destroy(sound);
}

TB_TEST(media_sound_edit, a_format_unknown_or_the_same_changes_nothing)
{
    const float values[2] = {0.25f, -0.25f};
    tb_sound *const sound = make_mono(TB_SOUND_FORMAT_S16, 8000, values, 2);
    TB_TEST_EXPECT_UINT(tb_sound_convert_format(sound, (tb_sound_format)9), ==, TB_SOUND_INVALID);
    TB_TEST_EXPECT_UINT(tb_sound_convert_format(sound, TB_SOUND_FORMAT_S16), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 1, 0), ==, -0.25f);
    tb_sound_destroy(sound);
}

/* Mono plays at full gain in every channel; many channels to one is their mean. */
TB_TEST(media_sound_edit, channels_mix_by_the_documented_gains)
{
    const float values[3] = {0.5f, -0.25f, 1.0f};
    tb_sound *const sound = make_mono(TB_SOUND_FORMAT_F32, 8000, values, 3);
    tb_sound_spec spec = {0};
    float frames[12] = {0};

    TB_TEST_REQUIRE_UINT(tb_sound_convert_channels(sound, 2, 0x3), ==, TB_SOUND_OK);
    tb_sound_read_frames(sound, 0, 3, frames, 6);
    TB_TEST_EXPECT_FLOAT(frames[0], ==, 0.5f);
    TB_TEST_EXPECT_FLOAT(frames[1], ==, 0.5f);
    TB_TEST_EXPECT_FLOAT(frames[5], ==, 1.0f);
    tb_sound_get_spec(sound, &spec);
    TB_TEST_EXPECT_HEX(spec.channel_mask, ==, 0x3);

    tb_sound_set_sample(sound, 0, 1, -0.5f);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_channels(sound, 1, 0), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 0, 0), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 1, 0), ==, -0.25f);

    /* Two to four keeps both and adds silence; four to three drops the last. */
    TB_TEST_REQUIRE_UINT(tb_sound_convert_channels(sound, 2, 0), ==, TB_SOUND_OK);
    tb_sound_set_sample(sound, 2, 1, 0.75f);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_channels(sound, 4, 0x33), ==, TB_SOUND_OK);
    tb_sound_read_frames(sound, 0, 3, frames, 12);
    TB_TEST_EXPECT_FLOAT(frames[8], ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(frames[9], ==, 0.75f);
    TB_TEST_EXPECT_FLOAT(frames[10], ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(frames[11], ==, 0.0f);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_channels(sound, 3, 0), ==, TB_SOUND_OK);
    tb_sound_read_frames(sound, 2, 1, frames, 3);
    TB_TEST_EXPECT_FLOAT(frames[1], ==, 0.75f);

    /* The same count changes only the mask. */
    TB_TEST_REQUIRE_UINT(tb_sound_convert_channels(sound, 3, 0x7), ==, TB_SOUND_OK);
    tb_sound_get_spec(sound, &spec);
    TB_TEST_EXPECT_HEX(spec.channel_mask, ==, 0x7);
    TB_TEST_EXPECT_UINT(tb_sound_convert_channels(sound, 0, 0), ==, TB_SOUND_INVALID);
    TB_TEST_EXPECT_UINT(tb_sound_convert_channels(sound, TB_SOUND_MAX_CHANNELS + 1, 0), ==,
                        TB_SOUND_INVALID);
    tb_sound_destroy(sound);
}

/* Doubling the rate puts a frame halfway between each two; the last
 * stands still, having no neighbour after it. */
TB_TEST(media_sound_edit, resampling_interpolates_between_neighbours)
{
    const float values[4] = {0.0f, 1000.0f / 32768.0f, 2000.0f / 32768.0f, 3000.0f / 32768.0f};
    tb_sound *const sound = make_mono(TB_SOUND_FORMAT_S16, 24000, values, 4);
    const tb_sound_loop loop = {.start = 2, .end = 4};
    const int16_t up[8] = {0, 500, 1000, 1500, 2000, 2500, 3000, 3000};
    const int16_t down[4] = {0, 1000, 2000, 3000};
    tb_sound_loop moved = {0};
    size_t length = 0;

    TB_TEST_REQUIRE_UINT(tb_sound_add_loop(sound, &loop), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_resample(sound, 48000), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_frame_count(sound), ==, 8);
    TB_TEST_EXPECT_BYTES(tb_sound_samples(sound, &length), up, sizeof(up));
    tb_sound_get_loop(sound, 0, &moved);
    TB_TEST_EXPECT_UINT(moved.start, ==, 4);
    TB_TEST_EXPECT_UINT(moved.end, ==, 8);
    TB_TEST_EXPECT_FLOAT(tb_sound_duration(sound), ==, 4.0 / 24000.0);

    TB_TEST_REQUIRE_UINT(tb_sound_resample(sound, 24000), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_BYTES(tb_sound_samples(sound, &length), down, sizeof(down));
    TB_TEST_EXPECT_UINT(tb_sound_resample(sound, 24000), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_resample(sound, 0), ==, TB_SOUND_INVALID);
    TB_TEST_EXPECT_UINT(tb_sound_resample(sound, TB_SOUND_MAX_RATE + 1), ==, TB_SOUND_INVALID);
    tb_sound_destroy(sound);
}

/* The new count is rounded up, so no time is lost; a loop's start rounds
 * down and its end up, so it never shrinks to nothing. */
TB_TEST(media_sound_edit, resampling_rounds_the_length_up)
{
    const float values[3] = {0.1f, 0.2f, 0.3f};
    tb_sound *const sound = make_mono(TB_SOUND_FORMAT_F32, 44100, values, 3);
    const tb_sound_loop short_loop = {.start = 1, .end = 2};
    tb_sound_loop moved = {0};
    TB_TEST_REQUIRE_UINT(tb_sound_add_loop(sound, &short_loop), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_resample(sound, 48000), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_frame_count(sound), ==, 4);
    tb_sound_get_loop(sound, 0, &moved);
    TB_TEST_EXPECT_UINT(moved.start, ==, 1);
    TB_TEST_EXPECT_UINT(moved.end, ==, 3);
    TB_TEST_REQUIRE_UINT(tb_sound_resample(sound, 1), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_frame_count(sound), ==, 1);
    tb_sound_get_loop(sound, 0, &moved);
    TB_TEST_EXPECT_UINT(moved.start, ==, 0);
    TB_TEST_EXPECT_UINT(moved.end, ==, 1);
    tb_sound_destroy(sound);

    tb_sound *empty = NULL;
    const tb_sound_spec spec = {.format = TB_SOUND_FORMAT_S16, .sample_rate = 8000, .channels = 2};
    TB_TEST_REQUIRE_UINT(tb_sound_create(&spec, 0, NULL, &empty), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_resample(empty, 16000), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_frame_count(empty), ==, 0);
    tb_sound_destroy(empty);
}

TB_TEST(media_sound_edit, gain_scales_and_clamps_integers_only)
{
    const float values[3] = {0.5f, -0.75f, 0.25f};
    tb_sound *const integer = make_mono(TB_SOUND_FORMAT_S16, 8000, values, 3);
    tb_sound *const floating = make_mono(TB_SOUND_FORMAT_F32, 8000, values, 3);

    tb_sound_apply_gain(integer, 4.0f);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(integer, 0, 0), ==, 32767.0f / 32768.0f);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(integer, 1, 0), ==, -1.0f);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(integer, 2, 0), ==, 1.0f - 1.0f / 32768.0f);
    tb_sound_apply_gain(floating, -4.0f);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(floating, 0, 0), ==, -2.0f);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(floating, 1, 0), ==, 3.0f);
    tb_sound_destroy(floating);
    tb_sound_destroy(integer);
}

/* A fade's first frame takes the first gain and its last the last. */
TB_TEST(media_sound_edit, fades_ramp_from_the_first_frame_to_the_last)
{
    const float ones[6] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    const float expected[6] = {0.0f, 0.5f, 1.0f, 1.0f, 0.5f, 1.0f};
    float faded[6] = {0};
    tb_sound *const sound = make_mono(TB_SOUND_FORMAT_F32, 8000, ones, 6);

    tb_sound_fade(sound, 0, 3, 0.0f, 1.0f);
    tb_sound_fade(sound, 3, 2, 1.0f, 0.5f);
    tb_sound_fade(sound, 5, 0, 0.0f, 0.0f);
    tb_sound_read_frames(sound, 0, 6, faded, 6);
    for (size_t index = 0; index < 6; index++)
    {
        TB_TEST_EXPECT_FLOAT(faded[index], ==, expected[index]);
    }
    tb_sound_fade(sound, 5, 1, 0.0f, 0.25f);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 5, 0), ==, 0.25f);
    tb_sound_destroy(sound);
}

TB_TEST(media_sound_edit, normalizing_scales_the_peak)
{
    const float values[3] = {0.125f, -0.25f, 0.0f};
    tb_sound *const sound = make_mono(TB_SOUND_FORMAT_S16, 8000, values, 3);
    TB_TEST_EXPECT_FLOAT(tb_sound_normalize(sound, 0.5f), ==, 2.0f);
    TB_TEST_EXPECT_FLOAT(tb_sound_peak(sound), ==, 0.5f);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 0, 0), ==, 0.25f);
    tb_sound_destroy(sound);

    const float silence[2] = {0.0f, 0.0f};
    tb_sound *const quiet = make_mono(TB_SOUND_FORMAT_S16, 8000, silence, 2);
    TB_TEST_EXPECT_FLOAT(tb_sound_normalize(quiet, 1.0f), ==, 1.0f);
    tb_sound_destroy(quiet);
}

TB_TEST(media_sound_edit, reversing_mirrors_frames_and_loops)
{
    const float values[5] = {0.0f, 0.125f, 0.25f, 0.375f, 0.5f};
    tb_sound *const sound = make_mono(TB_SOUND_FORMAT_F32, 8000, values, 5);
    const tb_sound_loop loop = {.start = 1, .end = 3};
    tb_sound_loop mirrored = {0};
    TB_TEST_REQUIRE_UINT(tb_sound_add_loop(sound, &loop), ==, TB_SOUND_OK);
    tb_sound_reverse(sound);
    for (size_t index = 0; index < 5; index++)
    {
        TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, index, 0), ==, values[4 - index]);
    }
    tb_sound_get_loop(sound, 0, &mirrored);
    TB_TEST_EXPECT_UINT(mirrored.start, ==, 2);
    TB_TEST_EXPECT_UINT(mirrored.end, ==, 4);
    tb_sound_trim(sound, 0, 1);
    tb_sound_reverse(sound);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 0, 0), ==, 0.5f);
    tb_sound_destroy(sound);
}

TB_TEST(media_sound_edit, trimming_keeps_a_range_and_its_loops)
{
    const float values[6] = {0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f};
    tb_sound *const sound = make_mono(TB_SOUND_FORMAT_F32, 8000, values, 6);
    const tb_sound_loop inside = {.start = 2, .end = 4};
    const tb_sound_loop outside = {.start = 0, .end = 6};
    tb_sound_loop kept = {0};
    TB_TEST_REQUIRE_UINT(tb_sound_add_loop(sound, &outside), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_add_loop(sound, &inside), ==, TB_SOUND_OK);
    tb_sound_trim(sound, 1, 4);
    TB_TEST_REQUIRE_UINT(tb_sound_frame_count(sound), ==, 4);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 0, 0), ==, 0.125f);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 3, 0), ==, 0.5f);
    TB_TEST_REQUIRE_UINT(tb_sound_loop_count(sound), ==, 1);
    tb_sound_get_loop(sound, 0, &kept);
    TB_TEST_EXPECT_UINT(kept.start, ==, 1);
    TB_TEST_EXPECT_UINT(kept.end, ==, 3);
    tb_sound_destroy(sound);
}

/* Dropping frames from the end needs no copy of borrowed samples; dropping
 * them all leaves a sound that owns none. */
TB_TEST(media_sound_edit, trimming_a_borrowed_sound)
{
    const tb_sound_spec spec = {.format = TB_SOUND_FORMAT_S16, .sample_rate = 8000, .channels = 1};
    const int16_t samples[4] = {1, 2, 3, 4};
    tb_sound *sound = NULL;
    size_t length = 0;
    TB_TEST_REQUIRE_UINT(tb_sound_create_borrowed(&spec, samples, sizeof(samples), NULL, &sound),
                         ==, TB_SOUND_OK);
    tb_sound_trim(sound, 0, 3);
    TB_TEST_EXPECT(!tb_sound_owns_samples(sound));
    TB_TEST_EXPECT_POINTER(tb_sound_samples(sound, &length), ==, samples);
    TB_TEST_EXPECT_UINT(length, ==, 6);
    tb_sound_trim(sound, 1, 2);
    TB_TEST_EXPECT(tb_sound_owns_samples(sound));
    TB_TEST_EXPECT_INT(samples[0], ==, 1);
    tb_sound_destroy(sound);

    TB_TEST_REQUIRE_UINT(tb_sound_create_borrowed(&spec, samples, sizeof(samples), NULL, &sound),
                         ==, TB_SOUND_OK);
    tb_sound_trim(sound, 4, 0);
    TB_TEST_EXPECT(tb_sound_owns_samples(sound));
    TB_TEST_EXPECT_POINTER(tb_sound_samples(sound, &length), ==, NULL);
    tb_sound_destroy(sound);
}

TB_TEST(media_sound_edit, appending_joins_frames_and_moves_the_loops)
{
    const float first[2] = {0.25f, 0.5f};
    const float second[3] = {-0.25f, -0.5f, -0.75f};
    tb_sound *const sound = make_mono(TB_SOUND_FORMAT_S16, 8000, first, 2);
    tb_sound *const other = make_mono(TB_SOUND_FORMAT_S16, 8000, second, 3);
    const tb_sound_loop loop = {.start = 1, .end = 3};
    tb_sound_loop moved = {0};

    TB_TEST_REQUIRE_UINT(tb_sound_add_loop(other, &loop), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_append(sound, other), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_frame_count(sound), ==, 5);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 1, 0), ==, 0.5f);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 4, 0), ==, -0.75f);
    tb_sound_get_loop(sound, 0, &moved);
    TB_TEST_EXPECT_UINT(moved.start, ==, 3);
    TB_TEST_EXPECT_UINT(moved.end, ==, 5);

    /* A sound appended to itself doubles, its loops with it. */
    TB_TEST_REQUIRE_UINT(tb_sound_append(sound, sound), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_frame_count(sound), ==, 10);
    TB_TEST_EXPECT_UINT(tb_sound_loop_count(sound), ==, 2);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 5, 0), ==, 0.25f);

    tb_sound_trim(other, 0, 0);
    TB_TEST_EXPECT_UINT(tb_sound_append(sound, other), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_frame_count(sound), ==, 10);
    TB_TEST_REQUIRE_UINT(tb_sound_append(other, sound), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_frame_count(other), ==, 10);
    TB_TEST_REQUIRE_UINT(tb_sound_resample(other, 16000), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_append(sound, other), ==, TB_SOUND_MISMATCH);
    TB_TEST_REQUIRE_UINT(tb_sound_resample(other, 8000), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_format(other, TB_SOUND_FORMAT_F32), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_append(sound, other), ==, TB_SOUND_MISMATCH);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_format(other, TB_SOUND_FORMAT_S16), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_convert_channels(other, 2, 0), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_append(sound, other), ==, TB_SOUND_MISMATCH);
    tb_sound_destroy(other);
    tb_sound_destroy(sound);
}

TB_TEST(media_sound_edit, appending_stops_adding_loops_when_full)
{
    const float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    tb_sound *const sound = make_mono(TB_SOUND_FORMAT_U8, 8000, values, 4);
    for (size_t index = 0; index < TB_SOUND_MAX_LOOPS; index++)
    {
        const tb_sound_loop loop = {.start = 0, .end = 4};
        TB_TEST_REQUIRE_UINT(tb_sound_add_loop(sound, &loop), ==, TB_SOUND_OK);
    }
    TB_TEST_REQUIRE_UINT(tb_sound_append(sound, sound), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_loop_count(sound), ==, TB_SOUND_MAX_LOOPS);
    TB_TEST_EXPECT_UINT(tb_sound_frame_count(sound), ==, 8);
    tb_sound_destroy(sound);
}

/* A sound so long that a change would need more bytes than there are is
 * refused before anything is allocated or read. The sound borrows a few
 * bytes and claims more; none of them is touched. */
TB_TEST(media_sound_edit, changes_too_large_for_memory_are_refused)
{
    const tb_sound_spec spec = {.format = TB_SOUND_FORMAT_U8, .sample_rate = 8000, .channels = 1};
    static const unsigned char few[1] = {128};
    tb_sound *sound = NULL;
    TB_TEST_REQUIRE_UINT(tb_sound_create_borrowed(&spec, few, SIZE_MAX / 2 + 1, NULL, &sound), ==,
                         TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_convert_format(sound, TB_SOUND_FORMAT_F32), ==,
                        TB_SOUND_TOO_LARGE);
    TB_TEST_EXPECT_UINT(tb_sound_convert_channels(sound, 2, 0), ==, TB_SOUND_TOO_LARGE);
    TB_TEST_EXPECT_UINT(tb_sound_resample(sound, 16000), ==, TB_SOUND_TOO_LARGE);
    TB_TEST_EXPECT_UINT(tb_sound_append(sound, sound), ==, TB_SOUND_TOO_LARGE);
    TB_TEST_EXPECT(!tb_sound_owns_samples(sound));
    tb_sound_destroy(sound);
}
