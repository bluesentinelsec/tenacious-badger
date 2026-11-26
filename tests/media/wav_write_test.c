/* WAV files written: the canonical header byte for byte, EXTENSIBLE where
 * the format's documentation requires it, every format and channel count
 * read back to the same samples, loops kept, and streams that refuse. */
#include "badger/sound.h"
#include "badger/stream.h"
#include "badger/wav.h"
#include "tb_test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Write a sound into a buffer stream; the caller closes it. */
static const unsigned char *write_sound(const tb_sound *sound, tb_stream *stream, size_t *length)
{
    tb_wav_error error = {0};
    tb_stream_open_buffer(NULL, stream);
    TB_TEST_REQUIRE_UINT(tb_wav_write(sound, stream, &error), ==, TB_WAV_OK);
    return tb_stream_buffer_bytes(stream, length);
}

static uint32_t u32_at(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint32_t u16_at(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
}

/* 16-bit stereo is the 44-byte header every reader knows. */
TB_TEST(media_wav_write, pcm_16_bit_stereo_has_the_canonical_header)
{
    static const unsigned char expected[48] = {
        'R', 'I', 'F', 'F', 40,  0,   0,   0,   'W',  'A',  'V', 'E', 'f',  'm',  't',  ' ',
        16,  0,   0,   0,   1,   0,   2,   0,   0x44, 0xAC, 0,   0,   0x10, 0xB1, 2,    0,
        4,   0,   16,  0,   'd', 'a', 't', 'a', 4,    0,    0,   0,   0x34, 0x12, 0xCC, 0xED};
    const tb_sound_spec spec = {.format = TB_SOUND_FORMAT_S16, .sample_rate = 44100, .channels = 2};
    const int16_t samples[2] = {0x1234, -0x1234};
    tb_sound *sound = NULL;
    tb_stream stream = {0};
    size_t length = 0;
    TB_TEST_REQUIRE_UINT(tb_sound_create_copy(&spec, samples, sizeof(samples), NULL, &sound), ==,
                         TB_SOUND_OK);
    const unsigned char *const bytes = write_sound(sound, &stream, &length);
    TB_TEST_REQUIRE_UINT(length, ==, sizeof(expected));
    TB_TEST_EXPECT_BYTES(bytes, expected, sizeof(expected));
    (void)tb_stream_close(&stream);
    tb_sound_destroy(sound);
}

/* Float of two channels or fewer is WAVE_FORMAT_IEEE_FLOAT with cbSize and
 * a fact chunk of its frames. */
TB_TEST(media_wav_write, float_has_a_fact_chunk)
{
    const tb_sound_spec spec = {.format = TB_SOUND_FORMAT_F32, .sample_rate = 48000, .channels = 1};
    tb_sound *sound = NULL;
    tb_stream stream = {0};
    size_t length = 0;
    TB_TEST_REQUIRE_UINT(tb_sound_create(&spec, 5, NULL, &sound), ==, TB_SOUND_OK);
    const unsigned char *const bytes = write_sound(sound, &stream, &length);
    TB_TEST_EXPECT_UINT(u32_at(bytes + 16), ==, 18);
    TB_TEST_EXPECT_UINT(u16_at(bytes + 20), ==, 3);
    TB_TEST_EXPECT_UINT(u16_at(bytes + 34), ==, 32);
    TB_TEST_EXPECT_UINT(u16_at(bytes + 36), ==, 0);
    TB_TEST_EXPECT_BYTES(bytes + 38, "fact", 4);
    TB_TEST_EXPECT_UINT(u32_at(bytes + 46), ==, 5);
    TB_TEST_EXPECT_BYTES(bytes + 50, "data", 4);
    TB_TEST_EXPECT_UINT(u32_at(bytes + 54), ==, 20);
    TB_TEST_EXPECT_UINT(length, ==, 58 + 20);
    TB_TEST_EXPECT_UINT(u32_at(bytes + 4), ==, length - 8);
    (void)tb_stream_close(&stream);
    tb_sound_destroy(sound);
}

/* EXTENSIBLE for more than 2 channels, more than 16 bits of integer, or a
 * mask other than the default; its sub-format is the plain tag. */
TB_TEST(media_wav_write, extensible_where_the_format_requires_it)
{
    typedef struct row
    {
        tb_sound_format format;
        uint32_t channels;
        uint32_t mask;
        bool extensible;
        uint32_t written_mask;
    } row;
    static const row rows[] = {
        {TB_SOUND_FORMAT_U8, 1, 0, false, 0},     {TB_SOUND_FORMAT_S16, 2, 0x3, false, 0},
        {TB_SOUND_FORMAT_S16, 1, 0x1, true, 0x1}, {TB_SOUND_FORMAT_S24, 1, 0, true, 0x4},
        {TB_SOUND_FORMAT_S32, 2, 0, true, 0x3},   {TB_SOUND_FORMAT_F32, 2, 0, false, 0},
        {TB_SOUND_FORMAT_F32, 6, 0, true, 0x3F},  {TB_SOUND_FORMAT_S16, 12, 0, true, 0},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        const tb_sound_spec spec = {.format = rows[index].format,
                                    .sample_rate = 8000,
                                    .channels = rows[index].channels,
                                    .channel_mask = rows[index].mask};
        tb_sound *sound = NULL;
        tb_stream stream = {0};
        size_t length = 0;
        TB_TEST_CONTEXT("row %zu", index);
        TB_TEST_REQUIRE_UINT(tb_sound_create(&spec, 1, NULL, &sound), ==, TB_SOUND_OK);
        const unsigned char *const bytes = write_sound(sound, &stream, &length);
        const uint32_t tag = u16_at(bytes + 20);
        const uint32_t plain = rows[index].format == TB_SOUND_FORMAT_F32 ? 3 : 1;
        if (rows[index].extensible)
        {
            TB_TEST_EXPECT_HEX(tag, ==, 0xFFFE);
            TB_TEST_EXPECT_UINT(u32_at(bytes + 16), ==, 40);
            TB_TEST_EXPECT_UINT(u16_at(bytes + 36), ==, 22);
            TB_TEST_EXPECT_HEX(u32_at(bytes + 40), ==, rows[index].written_mask);
            TB_TEST_EXPECT_UINT(u16_at(bytes + 44), ==, plain);
        }
        else
        {
            TB_TEST_EXPECT_HEX(tag, ==, plain);
        }
        (void)tb_stream_close(&stream);
        tb_sound_destroy(sound);
    }
}

/* Every format, of 1, 2, and 6 channels, of random samples, reads back as
 * it was written, with its spec. */
TB_TEST(media_wav_write, every_format_round_trips_exactly)
{
    static const uint32_t channel_counts[3] = {1, 2, 6};
    for (int format = TB_SOUND_FORMAT_U8; format <= TB_SOUND_FORMAT_F32; format++)
    {
        for (size_t count = 0; count < 3; count++)
        {
            const tb_sound_spec spec = {.format = (tb_sound_format)format,
                                        .sample_rate = 32000,
                                        .channels = channel_counts[count]};
            tb_sound *sound = NULL;
            tb_sound *back = NULL;
            tb_stream stream = {0};
            tb_sound_spec read_spec = {0};
            size_t length = 0;
            size_t written = 0;
            size_t read = 0;
            TB_TEST_CONTEXT("format %d, %u channels", format, channel_counts[count]);
            TB_TEST_REQUIRE_UINT(tb_sound_create(&spec, 37, NULL, &sound), ==, TB_SOUND_OK);
            unsigned char *const samples = tb_sound_edit_samples(sound, &length);
            /* Random bits, NaNs of F32 among them, which are kept bit for bit. */
            tb_test_random_bytes(samples, length);
            const unsigned char *const bytes = write_sound(sound, &stream, &written);
            const tb_wav_result result = tb_wav_read(bytes, written, NULL, &back, NULL);
            TB_TEST_REQUIRE_UINT(result, ==, TB_WAV_OK);
            tb_sound_get_spec(back, &read_spec);
            TB_TEST_EXPECT_UINT(read_spec.format, ==, spec.format);
            TB_TEST_EXPECT_UINT(read_spec.channels, ==, spec.channels);
            TB_TEST_EXPECT_UINT(read_spec.sample_rate, ==, spec.sample_rate);
            const void *const read_samples = tb_sound_samples(back, &read);
            TB_TEST_EXPECT_UINT(read, ==, length);
            TB_TEST_EXPECT_BYTES(read_samples, tb_sound_samples(sound, &length), length);
            /* An odd count of bytes is padded, and the padding is read past. */
            TB_TEST_EXPECT_UINT(written % 2, ==, 0);
            (void)tb_stream_close(&stream);
            tb_sound_destroy(back);
            tb_sound_destroy(sound);
        }
    }
}

TB_TEST(media_wav_write, loops_round_trip_through_smpl)
{
    const tb_sound_spec spec = {.format = TB_SOUND_FORMAT_U8, .sample_rate = 22050, .channels = 1};
    const tb_sound_loop loops[3] = {
        {.start = 0, .end = 3, .kind = TB_SOUND_LOOP_FORWARD},
        {.start = 1, .end = 2, .kind = TB_SOUND_LOOP_PING_PONG, .play_count = 4},
        {.start = 2, .end = 3, .kind = TB_SOUND_LOOP_BACKWARD},
    };
    tb_sound *sound = NULL;
    tb_sound *back = NULL;
    tb_stream stream = {0};
    size_t length = 0;
    TB_TEST_REQUIRE_UINT(tb_sound_create(&spec, 3, NULL, &sound), ==, TB_SOUND_OK);
    for (size_t index = 0; index < 3; index++)
    {
        TB_TEST_REQUIRE_UINT(tb_sound_add_loop(sound, &loops[index]), ==, TB_SOUND_OK);
    }
    const unsigned char *const bytes = write_sound(sound, &stream, &length);
    TB_TEST_REQUIRE_UINT(tb_wav_read(bytes, length, NULL, &back, NULL), ==, TB_WAV_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_loop_count(back), ==, 3);
    for (size_t index = 0; index < 3; index++)
    {
        tb_sound_loop loop = {0};
        tb_sound_get_loop(back, index, &loop);
        TB_TEST_EXPECT_UINT(loop.start, ==, loops[index].start);
        TB_TEST_EXPECT_UINT(loop.end, ==, loops[index].end);
        TB_TEST_EXPECT_UINT(loop.kind, ==, loops[index].kind);
        TB_TEST_EXPECT_UINT(loop.play_count, ==, loops[index].play_count);
    }
    (void)tb_stream_close(&stream);
    tb_sound_destroy(back);
    tb_sound_destroy(sound);
}

/* A stream that takes only so many bytes, then refuses. */
typedef struct limited_stream
{
    size_t room;
} limited_stream;

static tb_stream_result limited_write(void *state, const void *bytes, size_t length,
                                      size_t *transferred)
{
    limited_stream *const stream = state;
    (void)bytes;
    *transferred = 0;
    if (stream->room == 0)
    {
        return TB_STREAM_NO_SPACE;
    }
    *transferred = length < stream->room ? length : stream->room;
    stream->room -= *transferred;
    return TB_STREAM_OK;
}

static tb_stream_result limited_release(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

static const tb_stream_functions limited_functions = {.write_bytes = limited_write,
                                                      .release = limited_release};

/* Wherever the stream refuses, header, samples, padding or loops, the
 * write says so. */
TB_TEST(media_wav_write, a_stream_that_refuses_fails_the_write)
{
    const tb_sound_spec spec = {.format = TB_SOUND_FORMAT_U8, .sample_rate = 8000, .channels = 1};
    const tb_sound_loop loop = {.start = 0, .end = 1};
    tb_sound *sound = NULL;
    tb_wav_error error = {0};
    TB_TEST_REQUIRE_UINT(tb_sound_create(&spec, 5001, NULL, &sound), ==, TB_SOUND_OK);
    TB_TEST_REQUIRE_UINT(tb_sound_add_loop(sound, &loop), ==, TB_SOUND_OK);
    static const size_t rooms[] = {0, 10, 44, 4000, 44 + 5001, 44 + 5002, 44 + 5002 + 8};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(rooms); index++)
    {
        limited_stream state = {.room = rooms[index]};
        tb_stream stream = {.functions = &limited_functions, .state = &state};
        TB_TEST_CONTEXT("room %zu", rooms[index]);
        TB_TEST_EXPECT_UINT(tb_wav_write(sound, &stream, &error), ==, TB_WAV_OUTPUT_FAILED);
        TB_TEST_EXPECT_STRING(error.message, ==, "the stream refused what was written");
    }
    limited_stream state = {.room = 44 + 5002 + 8 + 60};
    tb_stream stream = {.functions = &limited_functions, .state = &state};
    TB_TEST_EXPECT_UINT(tb_wav_write(sound, &stream, &error), ==, TB_WAV_OK);
    TB_TEST_EXPECT_UINT(state.room, ==, 0);
    tb_sound_destroy(sound);
}

/* RIFF counts in 32 bits: 4 GiB of samples are refused before a byte is
 * written. The sound borrows a few bytes and claims more; none is touched. */
#if SIZE_MAX > UINT32_MAX
TB_TEST(media_wav_write, samples_past_4_gib_are_too_large)
{
    const tb_sound_spec spec = {.format = TB_SOUND_FORMAT_U8, .sample_rate = 8000, .channels = 1};
    static const unsigned char few[1] = {128};
    tb_sound *sound = NULL;
    tb_wav_error error = {0};
    limited_stream state = {0};
    tb_stream stream = {.functions = &limited_functions, .state = &state};
    TB_TEST_REQUIRE_UINT(tb_sound_create_borrowed(&spec, few, (size_t)UINT32_MAX, NULL, &sound), ==,
                         TB_SOUND_OK);
    TB_TEST_EXPECT_UINT(tb_wav_write(sound, &stream, &error), ==, TB_WAV_TOO_LARGE);
    TB_TEST_EXPECT_STRING(error.reason, ==, "more than 4 GiB of samples for RIFF");
    tb_sound_destroy(sound);
}
#endif
