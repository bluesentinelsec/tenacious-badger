/* Sounds: making them silent, from copies and from borrowed samples, the
 * mapping of every format's samples to floats and back, reading and
 * writing frames, loops, and the allocator every sound uses. */
#include "badger/sound.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <float.h>
#include <stdint.h>
#include <string.h>

static const tb_sound_spec stereo16 = {
    .format = TB_SOUND_FORMAT_S16, .sample_rate = 48000, .channels = 2};

static tb_sound_spec spec_of(tb_sound_format format, uint32_t channels)
{
    return (tb_sound_spec){.format = format, .sample_rate = 44100, .channels = channels};
}

/* Silence is 128 for U8 and zero bytes for every other format. */
TB_TEST(media_sound, a_new_sound_is_silent_in_every_format)
{
    for (int format = TB_SOUND_FORMAT_U8; format <= TB_SOUND_FORMAT_F32; format++)
    {
        const tb_sound_spec spec = spec_of((tb_sound_format)format, 3);
        tb_sound *sound = NULL;
        size_t length = 0;
        TB_TEST_CONTEXT("format %d", format);
        TB_TEST_REQUIRE_UINT(tb_sound_create(&spec, 10, NULL, &sound), ==, TB_SOUND_OK);
        const unsigned char *const bytes = tb_sound_samples(sound, &length);
        TB_TEST_EXPECT_UINT(length, ==, 30 * tb_sound_format_size(spec.format));
        TB_TEST_EXPECT_UINT(tb_sound_frame_count(sound), ==, 10);
        TB_TEST_EXPECT_UINT(tb_sound_frame_size(sound), ==, 3 * tb_sound_format_size(spec.format));
        for (size_t index = 0; index < length; index++)
        {
            TB_TEST_EXPECT_UINT(bytes[index], ==, format == TB_SOUND_FORMAT_U8 ? 128 : 0);
        }
        TB_TEST_EXPECT_FLOAT(tb_sound_peak(sound), ==, 0.0f);
        tb_sound_destroy(sound);
    }
}

TB_TEST(media_sound, specs_out_of_range_are_refused)
{
    const tb_sound_spec bad[] = {
        {.format = (tb_sound_format)TB_SOUND_FORMAT_COUNT, .sample_rate = 48000, .channels = 1},
        {.format = TB_SOUND_FORMAT_S16, .sample_rate = 0, .channels = 1},
        {.format = TB_SOUND_FORMAT_S16, .sample_rate = TB_SOUND_MAX_RATE + 1, .channels = 1},
        {.format = TB_SOUND_FORMAT_S16, .sample_rate = 48000, .channels = 0},
        {.format = TB_SOUND_FORMAT_S16,
         .sample_rate = 48000,
         .channels = TB_SOUND_MAX_CHANNELS + 1},
    };
    const int16_t samples[2] = {0};
    for (size_t row = 0; row < TB_TEST_COUNT_OF(bad); row++)
    {
        static int marker = 0;
        tb_sound *sound = (tb_sound *)(void *)&marker;
        TB_TEST_CONTEXT("row %zu", row);
        TB_TEST_EXPECT_UINT(tb_sound_create(&bad[row], 1, NULL, &sound), ==, TB_SOUND_INVALID);
        TB_TEST_EXPECT_POINTER(sound, ==, NULL);
        TB_TEST_EXPECT_UINT(tb_sound_create_copy(&bad[row], samples, sizeof(samples), NULL, &sound),
                            ==, TB_SOUND_INVALID);
        TB_TEST_EXPECT_UINT(
            tb_sound_create_borrowed(&bad[row], samples, sizeof(samples), NULL, &sound), ==,
            TB_SOUND_INVALID);
    }
    tb_sound *sound = NULL;
    TB_TEST_EXPECT_UINT(tb_sound_create(&stereo16, SIZE_MAX / 2, NULL, &sound), ==,
                        TB_SOUND_TOO_LARGE);
    /* Three bytes are not whole frames of 4. */
    TB_TEST_EXPECT_UINT(tb_sound_create_copy(&stereo16, samples, 3, NULL, &sound), ==,
                        TB_SOUND_INVALID);
    TB_TEST_EXPECT_UINT(tb_sound_create_borrowed(&stereo16, samples, 3, NULL, &sound), ==,
                        TB_SOUND_INVALID);
}

TB_TEST(media_sound, an_empty_sound_has_no_samples)
{
    tb_sound *sound = NULL;
    size_t length = 1;
    TB_TEST_REQUIRE_UINT(tb_sound_create(&stereo16, 0, NULL, &sound), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_POINTER(tb_sound_samples(sound, &length), ==, NULL);
    TB_TEST_EXPECT_UINT(length, ==, 0);
    TB_TEST_EXPECT_FLOAT(tb_sound_duration(sound), ==, 0.0);
    tb_sound_destroy(sound);
    TB_TEST_REQUIRE_UINT(tb_sound_create_copy(&stereo16, NULL, 0, NULL, &sound), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_frame_count(sound), ==, 0);
    tb_sound_destroy(sound);
    TB_TEST_REQUIRE_UINT(tb_sound_create_borrowed(&stereo16, NULL, 0, NULL, &sound), ==,
                         TB_SOUND_OK);
    TB_TEST_EXPECT(tb_sound_owns_samples(sound));
    tb_sound_destroy(sound);
    tb_sound_destroy(NULL);
}

TB_TEST(media_sound, a_copy_is_independent_of_its_source)
{
    int16_t samples[4] = {1, -2, 300, -400};
    tb_sound *sound = NULL;
    size_t length = 0;
    TB_TEST_REQUIRE_UINT(tb_sound_create_copy(&stereo16, samples, sizeof(samples), NULL, &sound),
                         ==, TB_SOUND_OK);
    samples[0] = 99;
    const int16_t *const kept = tb_sound_samples(sound, &length);
    TB_TEST_EXPECT_UINT(length, ==, sizeof(samples));
    TB_TEST_EXPECT_INT(kept[0], ==, 1);
    TB_TEST_EXPECT_INT(kept[3], ==, -400);
    TB_TEST_EXPECT(tb_sound_owns_samples(sound));
    TB_TEST_EXPECT_UINT(tb_sound_frame_count(sound), ==, 2);
    tb_sound_destroy(sound);
}

/* A borrowed sound reads in place and copies before its first write, so
 * the caller's samples are never changed. */
TB_TEST(media_sound, a_borrowed_sound_never_writes_the_callers_samples)
{
    const int16_t samples[4] = {1000, 2000, 3000, 4000};
    tb_sound *sound = NULL;
    size_t length = 0;
    TB_TEST_REQUIRE_UINT(
        tb_sound_create_borrowed(&stereo16, samples, sizeof(samples), NULL, &sound), ==,
        TB_SOUND_OK);
    TB_TEST_EXPECT(!tb_sound_owns_samples(sound));
    TB_TEST_EXPECT_POINTER(tb_sound_samples(sound, &length), ==, samples);
    tb_sound_set_sample(sound, 0, 0, 0.0f);
    TB_TEST_EXPECT(tb_sound_owns_samples(sound));
    TB_TEST_EXPECT_INT(samples[0], ==, 1000);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 0, 0), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 1, 1), ==, 4000.0f / 32768.0f);
    tb_sound_destroy(sound);

    TB_TEST_REQUIRE_UINT(
        tb_sound_create_borrowed(&stereo16, samples, sizeof(samples), NULL, &sound), ==,
        TB_SOUND_OK);
    int16_t *const edited = tb_sound_edit_samples(sound, &length);
    TB_TEST_EXPECT_POINTER(edited, !=, samples);
    TB_TEST_EXPECT_UINT(length, ==, sizeof(samples));
    edited[1] = 7;
    TB_TEST_EXPECT_INT(samples[1], ==, 2000);
    tb_sound_destroy(sound);
}

TB_TEST(media_sound, duplicates_and_ranges_copy_frames_and_the_loops_within)
{
    const int16_t samples[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const tb_sound_spec mono = spec_of(TB_SOUND_FORMAT_S16, 1);
    tb_sound *sound = NULL;
    size_t length = 0;
    TB_TEST_REQUIRE_UINT(tb_sound_create_borrowed(&mono, samples, sizeof(samples), NULL, &sound),
                         ==, TB_SOUND_OK);
    const tb_sound_loop inside = {.start = 3, .end = 5, .play_count = 2};
    const tb_sound_loop across = {.start = 1, .end = 9, .kind = TB_SOUND_LOOP_PING_PONG};
    TB_TEST_REQUIRE_UINT(tb_sound_add_loop(sound, &inside), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_add_loop(sound, &across), ==, TB_SOUND_OK);

    tb_sound *const whole = tb_sound_duplicate(sound, NULL);
    TB_TEST_EXPECT(tb_sound_owns_samples(whole));
    TB_TEST_EXPECT_BYTES(tb_sound_samples(whole, &length), samples, sizeof(samples));
    TB_TEST_EXPECT_UINT(tb_sound_loop_count(whole), ==, 2);

    tb_sound *const range = tb_sound_copy_range(sound, 2, 4, NULL);
    const int16_t *const part = tb_sound_samples(range, &length);
    TB_TEST_EXPECT_UINT(length, ==, 8);
    TB_TEST_EXPECT_INT(part[0], ==, 2);
    TB_TEST_EXPECT_INT(part[3], ==, 5);
    TB_TEST_REQUIRE_UINT(tb_sound_loop_count(range), ==, 1);
    tb_sound_loop loop = {0};
    tb_sound_get_loop(range, 0, &loop);
    TB_TEST_EXPECT_UINT(loop.start, ==, 1);
    TB_TEST_EXPECT_UINT(loop.end, ==, 3);
    TB_TEST_EXPECT_UINT(loop.play_count, ==, 2);

    tb_sound *const nothing = tb_sound_copy_range(sound, 10, 0, NULL);
    TB_TEST_EXPECT_UINT(tb_sound_frame_count(nothing), ==, 0);
    tb_sound_destroy(nothing);
    tb_sound_destroy(range);
    tb_sound_destroy(whole);
    tb_sound_destroy(sound);
}

/* The mapping of the file's comment: n bits over 2^(n-1), back rounded half
 * away from zero and clamped. */
TB_TEST(media_sound, samples_map_to_floats_and_back)
{
    typedef struct row
    {
        tb_sound_format format;
        float written;
        float read;
    } row;
    static const row rows[] = {
        {TB_SOUND_FORMAT_U8, 0.0f, 0.0f},
        {TB_SOUND_FORMAT_U8, -1.0f, -1.0f},
        {TB_SOUND_FORMAT_U8, 1.0f, 127.0f / 128.0f},
        {TB_SOUND_FORMAT_U8, 0.5f, 0.5f},
        {TB_SOUND_FORMAT_U8, -3.0f, -1.0f},
        {TB_SOUND_FORMAT_S16, 1.0f, 32767.0f / 32768.0f},
        {TB_SOUND_FORMAT_S16, -1.0f, -1.0f},
        {TB_SOUND_FORMAT_S16, 1.5f / 32768.0f, 2.0f / 32768.0f},
        {TB_SOUND_FORMAT_S16, -1.5f / 32768.0f, -2.0f / 32768.0f},
        {TB_SOUND_FORMAT_S16, 1.49f / 32768.0f, 1.0f / 32768.0f},
        {TB_SOUND_FORMAT_S24, -1.0f, -1.0f},
        {TB_SOUND_FORMAT_S24, 1.0f, 8388607.0f / 8388608.0f},
        {TB_SOUND_FORMAT_S24, -0.25f, -0.25f},
        {TB_SOUND_FORMAT_S32, -1.0f, -1.0f},
        {TB_SOUND_FORMAT_S32, 2.0f, 1.0f},
        {TB_SOUND_FORMAT_S32, 0.125f, 0.125f},
        {TB_SOUND_FORMAT_F32, 3.5f, 3.5f},
        {TB_SOUND_FORMAT_F32, -0.0f, -0.0f},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        const tb_sound_spec spec = spec_of(rows[index].format, 1);
        tb_sound *sound = NULL;
        TB_TEST_CONTEXT("row %zu", index);
        TB_TEST_REQUIRE_UINT(tb_sound_create(&spec, 1, NULL, &sound), ==, TB_SOUND_OK);
        tb_sound_set_sample(sound, 0, 0, rows[index].written);
        TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 0, 0), ==, rows[index].read);
        tb_sound_destroy(sound);
    }
}

/* S24 is three bytes, least significant first, and its sign is bit 23. */
TB_TEST(media_sound, s24_is_packed_little_endian)
{
    const tb_sound_spec spec = spec_of(TB_SOUND_FORMAT_S24, 2);
    const unsigned char bytes[6] = {0x01, 0x00, 0x80, 0xFF, 0xFF, 0x7F};
    tb_sound *sound = NULL;
    size_t length = 0;
    TB_TEST_REQUIRE_UINT(tb_sound_create_copy(&spec, bytes, sizeof(bytes), NULL, &sound), ==,
                         TB_SOUND_OK);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 0, 0), ==, -8388607.0f / 8388608.0f);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 0, 1), ==, 8388607.0f / 8388608.0f);
    tb_sound_set_sample(sound, 0, 1, -1.0f / 8388608.0f);
    const unsigned char *const stored = tb_sound_samples(sound, &length);
    TB_TEST_EXPECT_UINT(stored[3], ==, 0xFF);
    TB_TEST_EXPECT_UINT(stored[4], ==, 0xFF);
    TB_TEST_EXPECT_UINT(stored[5], ==, 0xFF);
    tb_sound_destroy(sound);
}

static float float_from_bits(uint32_t bits)
{
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

TB_TEST(media_sound, nan_is_silence_and_infinity_clamps)
{
    const float nan = float_from_bits(0x7FC00000u);
    const float negative_infinity = float_from_bits(0xFF800000u);
    for (int format = TB_SOUND_FORMAT_U8; format <= TB_SOUND_FORMAT_F32; format++)
    {
        const tb_sound_spec spec = spec_of((tb_sound_format)format, 1);
        tb_sound *sound = NULL;
        TB_TEST_CONTEXT("format %d", format);
        TB_TEST_REQUIRE_UINT(tb_sound_create(&spec, 2, NULL, &sound), ==, TB_SOUND_OK);
        tb_sound_set_sample(sound, 0, 0, nan);
        TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 0, 0), ==, 0.0f);
        tb_sound_set_sample(sound, 1, 0, negative_infinity);
        const float lowest = format == TB_SOUND_FORMAT_F32 ? -FLT_MAX : -1.0f;
        TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 1, 0), ==, lowest);
        tb_sound_destroy(sound);
    }
}

TB_TEST(media_sound, frames_read_and_write_as_interleaved_floats)
{
    const tb_sound_spec spec = spec_of(TB_SOUND_FORMAT_S16, 2);
    const float written[6] = {0.5f, -0.5f, 0.25f, -0.25f, 1.0f, -1.0f};
    float read[6] = {0};
    tb_sound *sound = NULL;
    TB_TEST_REQUIRE_UINT(tb_sound_create(&spec, 4, NULL, &sound), ==, TB_SOUND_OK);
    tb_sound_write_frames(sound, 1, 3, written, TB_TEST_COUNT_OF(written));
    tb_sound_read_frames(sound, 1, 3, read, TB_TEST_COUNT_OF(read));
    for (size_t index = 0; index < 4; index++)
    {
        TB_TEST_EXPECT_FLOAT(read[index], ==, written[index]);
    }
    /* +1 is past the largest 16-bit sample; -1 is the smallest. */
    TB_TEST_EXPECT_FLOAT(read[4], ==, 32767.0f / 32768.0f);
    TB_TEST_EXPECT_FLOAT(read[5], ==, -1.0f);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 0, 0), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_sound_peak(sound), ==, 1.0f);
    tb_sound_write_frames(sound, 4, 0, NULL, 0);
    tb_sound_read_frames(sound, 4, 0, NULL, 0);
    tb_sound_destroy(sound);
}

TB_TEST(media_sound, queries_describe_the_sound)
{
    const tb_sound_spec spec = {
        .format = TB_SOUND_FORMAT_F32, .sample_rate = 22050, .channels = 6, .channel_mask = 0x3F};
    tb_sound_spec got = {0};
    tb_sound *sound = NULL;
    TB_TEST_REQUIRE_UINT(tb_sound_create(&spec, 44100, NULL, &sound), ==, TB_SOUND_OK);
    tb_sound_get_spec(sound, &got);
    TB_TEST_EXPECT_UINT(got.format, ==, TB_SOUND_FORMAT_F32);
    TB_TEST_EXPECT_UINT(got.sample_rate, ==, 22050);
    TB_TEST_EXPECT_UINT(got.channels, ==, 6);
    TB_TEST_EXPECT_HEX(got.channel_mask, ==, 0x3F);
    TB_TEST_EXPECT_FLOAT(tb_sound_duration(sound), ==, 2.0);
    TB_TEST_EXPECT_UINT(tb_sound_frame_size(sound), ==, 24);
    tb_sound_destroy(sound);
}

TB_TEST(media_sound, sizes_masks_and_names)
{
    TB_TEST_EXPECT_UINT(tb_sound_format_size(TB_SOUND_FORMAT_U8), ==, 1);
    TB_TEST_EXPECT_UINT(tb_sound_format_size(TB_SOUND_FORMAT_S16), ==, 2);
    TB_TEST_EXPECT_UINT(tb_sound_format_size(TB_SOUND_FORMAT_S24), ==, 3);
    TB_TEST_EXPECT_UINT(tb_sound_format_size(TB_SOUND_FORMAT_S32), ==, 4);
    TB_TEST_EXPECT_UINT(tb_sound_format_size(TB_SOUND_FORMAT_F32), ==, 4);
    TB_TEST_EXPECT_UINT(tb_sound_format_size((tb_sound_format)99), ==, 0);
    TB_TEST_EXPECT_HEX(tb_sound_default_channel_mask(0), ==, 0);
    TB_TEST_EXPECT_HEX(tb_sound_default_channel_mask(1), ==, TB_SOUND_FRONT_CENTER);
    TB_TEST_EXPECT_HEX(tb_sound_default_channel_mask(2), ==, 0x3);
    TB_TEST_EXPECT_HEX(tb_sound_default_channel_mask(6), ==, 0x3F);
    TB_TEST_EXPECT_HEX(tb_sound_default_channel_mask(8), ==, 0x63F);
    TB_TEST_EXPECT_HEX(tb_sound_default_channel_mask(9), ==, 0);
    TB_TEST_EXPECT_STRING(tb_sound_result_name(TB_SOUND_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_sound_result_name(TB_SOUND_INVALID), ==, "invalid argument");
    TB_TEST_EXPECT_STRING(tb_sound_result_name(TB_SOUND_TOO_LARGE), ==, "too large");
    TB_TEST_EXPECT_STRING(tb_sound_result_name(TB_SOUND_MISMATCH), ==, "sounds differ");
    TB_TEST_EXPECT_STRING(tb_sound_result_name(TB_SOUND_FULL), ==, "too many loops");
    TB_TEST_EXPECT_STRING(tb_sound_result_name((tb_sound_result)99), ==, "unknown result");
}

TB_TEST(media_sound, loops_are_checked_kept_and_cleared)
{
    tb_sound *sound = NULL;
    tb_sound_loop loop = {0};
    TB_TEST_REQUIRE_UINT(tb_sound_create(&stereo16, 100, NULL, &sound), ==, TB_SOUND_OK);
    const tb_sound_loop empty = {.start = 5, .end = 5};
    const tb_sound_loop past = {.start = 5, .end = 101};
    const tb_sound_loop unknown = {.start = 0, .end = 10, .kind = (tb_sound_loop_kind)7};
    TB_TEST_EXPECT_UINT(tb_sound_add_loop(sound, &empty), ==, TB_SOUND_INVALID);
    TB_TEST_EXPECT_UINT(tb_sound_add_loop(sound, &past), ==, TB_SOUND_INVALID);
    TB_TEST_EXPECT_UINT(tb_sound_add_loop(sound, &unknown), ==, TB_SOUND_INVALID);
    for (size_t index = 0; index < TB_SOUND_MAX_LOOPS; index++)
    {
        const tb_sound_loop good = {
            .start = index, .end = 100, .kind = TB_SOUND_LOOP_BACKWARD, .play_count = 3};
        TB_TEST_REQUIRE_UINT(tb_sound_add_loop(sound, &good), ==, TB_SOUND_OK);
    }
    const tb_sound_loop more = {.start = 0, .end = 1};
    TB_TEST_EXPECT_UINT(tb_sound_add_loop(sound, &more), ==, TB_SOUND_FULL);
    TB_TEST_EXPECT_UINT(tb_sound_loop_count(sound), ==, TB_SOUND_MAX_LOOPS);
    tb_sound_get_loop(sound, 7, &loop);
    TB_TEST_EXPECT_UINT(loop.start, ==, 7);
    TB_TEST_EXPECT_UINT(loop.kind, ==, TB_SOUND_LOOP_BACKWARD);
    tb_sound_clear_loops(sound);
    TB_TEST_EXPECT_UINT(tb_sound_loop_count(sound), ==, 0);
    tb_sound_destroy(sound);
}

/* Every block a sound allocates comes from its allocator and goes back to it. */
TB_TEST(media_sound, sounds_live_in_their_allocator)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    const int16_t samples[4] = {1, 2, 3, 4};
    tb_sound *sound = NULL;
    tb_sound *borrowed = NULL;

    TB_TEST_REQUIRE_UINT(tb_sound_create(&stereo16, 8, &allocator, &sound), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(
        tb_sound_create_borrowed(&stereo16, samples, sizeof(samples), &allocator, &borrowed), ==,
        TB_SOUND_OK);
    tb_sound_apply_gain(borrowed, 2.0f);
    TB_TEST_EXPECT_UINT(tb_sound_convert_format(sound, TB_SOUND_FORMAT_F32), ==, TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_sound_append(borrowed, borrowed), ==, TB_SOUND_OK);
    tb_sound *const copy = tb_sound_duplicate(borrowed, &allocator);
    TB_TEST_EXPECT(tracker.live_block_count > 0);
    tb_sound_destroy(copy);
    tb_sound_destroy(borrowed);
    tb_sound_destroy(sound);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}
