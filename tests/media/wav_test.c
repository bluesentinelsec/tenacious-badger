/* WAV files read: every format of the table in badger/wav.h, built byte by
 * byte, with the samples each must decode to; chunks skipped and padded;
 * loops; RF64; streams that cannot seek; the reader a block at a time; and
 * files read through a VFS. */
#include "badger/sound.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/vfs.h"
#include "badger/wav.h"
#include "tb_test.h"
#include "wav_builder.h"

#include <float.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Read a built file, requiring success. */
static tb_sound *read_built(const wav_builder *file)
{
    tb_sound *sound = NULL;
    tb_wav_error error = {0};
    const tb_wav_result result = tb_wav_read(file->bytes, file->length, NULL, &sound, &error);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(result, ==, TB_WAV_OK);
    TB_TEST_REQUIRE_STRING(error.message, ==, "");
    return sound;
}

static void expect_spec(const tb_sound *sound, tb_sound_format format, uint32_t rate,
                        uint32_t channels, size_t frames)
{
    tb_sound_spec spec = {0};
    tb_sound_get_spec(sound, &spec);
    TB_TEST_EXPECT_UINT(spec.format, ==, format);
    TB_TEST_EXPECT_UINT(spec.sample_rate, ==, rate);
    TB_TEST_EXPECT_UINT(spec.channels, ==, channels);
    TB_TEST_EXPECT_UINT(tb_sound_frame_count(sound), ==, frames);
}

TB_TEST(media_wav, pcm_16_bit_stereo)
{
    wav_builder file = {0};
    const int16_t expected[6] = {0, -1, 32767, -32768, 1234, -4321};
    wav_begin(&file, "RIFF");
    wav_fmt(&file, 1, 2, 44100, 4, 16);
    const size_t data = wav_open_chunk(&file, "data");
    for (size_t index = 0; index < 6; index++)
    {
        wav_put_u16(&file, (uint16_t)expected[index]);
    }
    wav_close_chunk(&file, data);
    wav_finish(&file);

    tb_sound *const sound = read_built(&file);
    size_t length = 0;
    expect_spec(sound, TB_SOUND_FORMAT_S16, 44100, 2, 3);
    TB_TEST_EXPECT_BYTES(tb_sound_samples(sound, &length), expected, sizeof(expected));
    tb_sound_destroy(sound);
}

/* Each sample is read by the bytes it fills: 8 bits as U8, 12 in two bytes
 * as S16, 20 and 24 in three as S24, and 32 as S32. */
TB_TEST(media_wav, pcm_of_every_width)
{
    typedef struct row
    {
        uint32_t bits;
        uint32_t container;
        tb_sound_format format;
    } row;
    static const row rows[] = {
        {8, 1, TB_SOUND_FORMAT_U8},   {12, 2, TB_SOUND_FORMAT_S16}, {20, 3, TB_SOUND_FORMAT_S24},
        {24, 3, TB_SOUND_FORMAT_S24}, {32, 4, TB_SOUND_FORMAT_S32}, {24, 4, TB_SOUND_FORMAT_S32},
    };
    const unsigned char samples[8] = {0x10, 0x80, 0xF0, 0x7F, 0x01, 0xFF, 0x00, 0x80};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        wav_builder file = {0};
        size_t length = 0;
        TB_TEST_CONTEXT("%u bits in %u bytes", rows[index].bits, rows[index].container);
        wav_begin(&file, "RIFF");
        wav_fmt(&file, 1, 1, 8000, rows[index].container, rows[index].bits);
        wav_chunk(&file, "data", samples, rows[index].container * 2);
        wav_finish(&file);
        tb_sound *const sound = read_built(&file);
        expect_spec(sound, rows[index].format, 8000, 1, 2);
        /* Integer samples are kept in their bytes; this machine is little-endian. */
        TB_TEST_EXPECT_BYTES(tb_sound_samples(sound, &length), samples, rows[index].container * 2);
        tb_sound_destroy(sound);
    }
}

static void put_f32(wav_builder *file, float value)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    wav_put_u32(file, bits);
}

static void put_f64(wav_builder *file, uint64_t bits)
{
    wav_put_u64(file, bits);
}

TB_TEST(media_wav, float_32_bit_keeps_every_bit)
{
    wav_builder file = {0};
    const float expected[4] = {0.5f, -1.25f, 1e-30f, -0.0f};
    size_t length = 0;
    wav_begin(&file, "RIFF");
    wav_fmt(&file, 3, 1, 96000, 4, 32);
    const size_t data = wav_open_chunk(&file, "data");
    for (size_t index = 0; index < 4; index++)
    {
        put_f32(&file, expected[index]);
    }
    wav_close_chunk(&file, data);
    wav_finish(&file);
    tb_sound *const sound = read_built(&file);
    expect_spec(sound, TB_SOUND_FORMAT_F32, 96000, 1, 4);
    TB_TEST_EXPECT_BYTES(tb_sound_samples(sound, &length), expected, sizeof(expected));
    tb_sound_destroy(sound);
}

/* 64-bit floats become 32-bit, rounded; NaN becomes silence, and what is
 * past a float's range its largest value. */
TB_TEST(media_wav, float_64_bit_is_rounded_to_32)
{
    wav_builder file = {0};
    float samples[4] = {0};
    wav_begin(&file, "RIFF");
    wav_fmt(&file, 3, 2, 48000, 16, 64);
    const size_t data = wav_open_chunk(&file, "data");
    put_f64(&file, 0x3FD0000000000000u); /* 0.25 */
    put_f64(&file, 0x7FF8000000000000u); /* NaN */
    put_f64(&file, 0x7FEFFFFFFFFFFFFFu); /* DBL_MAX */
    put_f64(&file, 0xBFF0000000000001u); /* just past -1 */
    wav_close_chunk(&file, data);
    wav_finish(&file);
    tb_sound *const sound = read_built(&file);
    expect_spec(sound, TB_SOUND_FORMAT_F32, 48000, 2, 2);
    tb_sound_read_frames(sound, 0, 2, samples, 4);
    TB_TEST_EXPECT_FLOAT(samples[0], ==, 0.25f);
    TB_TEST_EXPECT_FLOAT(samples[1], ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(samples[2], ==, FLT_MAX);
    TB_TEST_EXPECT_FLOAT(samples[3], ==, -1.0f);
    tb_sound_destroy(sound);
}

/* G.711's expansions at the ends of its range and around zero, as ITU-T
 * G.711's tables give them. */
TB_TEST(media_wav, a_law_and_mu_law_expand_to_16_bits)
{
    typedef struct row
    {
        uint32_t tag;
        unsigned char codes[4];
        int16_t expected[4];
    } row;
    static const row rows[] = {
        {6, {0xD5, 0x55, 0xAA, 0x2A}, {8, -8, 32256, -32256}},
        {7, {0xFF, 0x7F, 0x80, 0x00}, {0, 0, 32124, -32124}},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        wav_builder file = {0};
        size_t length = 0;
        TB_TEST_CONTEXT("tag %u", rows[index].tag);
        wav_begin(&file, "RIFF");
        wav_fmt(&file, rows[index].tag, 1, 8000, 1, 8);
        wav_chunk(&file, "data", rows[index].codes, 4);
        wav_finish(&file);
        tb_sound *const sound = read_built(&file);
        expect_spec(sound, TB_SOUND_FORMAT_S16, 8000, 1, 4);
        TB_TEST_EXPECT_BYTES(tb_sound_samples(sound, &length), rows[index].expected, 8);
        tb_sound_destroy(sound);
    }
}

/* An IMA ADPCM block of one channel: the header's sample, then nibbles,
 * low first. Each 7 adds the step and halves and quarters of it, and moves
 * the step index up 8. */
static void put_ima_mono_block(wav_builder *file, unsigned char nibbles)
{
    wav_put_u16(file, 0);
    const unsigned char header_tail[2] = {0, 0};
    wav_put(file, header_tail, 2);
    const unsigned char words[4] = {nibbles, nibbles, nibbles, nibbles};
    wav_put(file, words, 4);
}

TB_TEST(media_wav, ima_adpcm_mono_decodes_by_its_steps)
{
    wav_builder file = {0};
    const int16_t expected[5] = {0, 11, 41, 104, 240};
    tb_sound *sound = NULL;
    size_t length = 0;
    wav_begin(&file, "RIFF");
    wav_fmt(&file, 0x11, 1, 22050, 8, 4);
    const size_t data = wav_open_chunk(&file, "data");
    put_ima_mono_block(&file, 0x77);
    put_ima_mono_block(&file, 0x77);
    wav_close_chunk(&file, data);
    wav_finish(&file);
    sound = read_built(&file);
    expect_spec(sound, TB_SOUND_FORMAT_S16, 22050, 1, 18);
    const int16_t *const samples = tb_sound_samples(sound, &length);
    TB_TEST_EXPECT_BYTES(samples, expected, sizeof(expected));
    /* The second block starts again from its own header. */
    TB_TEST_EXPECT_BYTES(samples + 9, expected, sizeof(expected));
    tb_sound_destroy(sound);
}

/* Two channels take turns by words of 4 bytes; a short last block gives
 * the frames it has, and `fact` bounds them all. */
TB_TEST(media_wav, ima_adpcm_stereo_interleaves_words)
{
    wav_builder file = {0};
    const int16_t left[8] = {107, 117, 129, 143, 161, 182, 207, 238};
    tb_sound *sound = NULL;
    float frame[2] = {0};
    wav_begin(&file, "RIFF");
    wav_fmt(&file, 0x11, 2, 22050, 16, 4);
    const size_t fact = wav_open_chunk(&file, "fact");
    wav_put_u32(&file, 12);
    wav_close_chunk(&file, fact);
    const size_t data = wav_open_chunk(&file, "data");
    for (int block = 0; block < 2; block++)
    {
        const unsigned char left_word[4] = {0x44, 0x44, 0x44, 0x44};
        const unsigned char right_word[4] = {0x11, 0x11, 0x11, 0x11};
        wav_put_u16(&file, 100);
        wav_put_u16(&file, 0);
        wav_put_u16(&file, (uint16_t)-100);
        wav_put_u16(&file, 0);
        wav_put(&file, left_word, 4);
        wav_put(&file, right_word, 4);
    }
    /* A third block of headers only: one frame more, which `fact` cuts. */
    wav_put_u32(&file, 0);
    wav_put_u32(&file, 0);
    wav_close_chunk(&file, data);
    wav_finish(&file);
    sound = read_built(&file);
    expect_spec(sound, TB_SOUND_FORMAT_S16, 22050, 2, 12);
    for (size_t index = 0; index < 8; index++)
    {
        tb_sound_read_frames(sound, 1 + index, 1, frame, 2);
        TB_TEST_EXPECT_FLOAT(frame[0] * 32768.0f, ==, (float)left[index]);
        TB_TEST_EXPECT_FLOAT(frame[1] * 32768.0f, ==, -99.0f + (float)index);
    }
    tb_sound_read_frames(sound, 9, 1, frame, 2);
    TB_TEST_EXPECT_FLOAT(frame[0] * 32768.0f, ==, 100.0f);
    tb_sound_destroy(sound);
}

TB_TEST(media_wav, extensible_gives_its_sub_format_and_mask)
{
    wav_builder file = {0};
    const unsigned char samples[18] = {1,  2,  3,  4,  5,  6,  7,  8,  9,
                                       10, 11, 12, 13, 14, 15, 16, 17, 18};
    tb_sound_spec spec = {0};
    size_t length = 0;
    tb_wav_error error = {0};
    tb_sound *sound = NULL;
    wav_begin(&file, "RIFF");
    wav_fmt_extensible(&file, 1, 6, 48000, 18, 24, 0x3F);
    wav_chunk(&file, "data", samples, sizeof(samples));
    wav_finish(&file);
    sound = read_built(&file);
    expect_spec(sound, TB_SOUND_FORMAT_S24, 48000, 6, 1);
    tb_sound_get_spec(sound, &spec);
    TB_TEST_EXPECT_HEX(spec.channel_mask, ==, 0x3F);
    TB_TEST_EXPECT_BYTES(tb_sound_samples(sound, &length), samples, sizeof(samples));
    tb_sound_destroy(sound);

    wav_begin(&file, "RIFF");
    wav_fmt_extensible(&file, 3, 2, 48000, 8, 32, 0x3);
    wav_chunk(&file, "data", samples, 16);
    wav_finish(&file);
    sound = read_built(&file);
    expect_spec(sound, TB_SOUND_FORMAT_F32, 48000, 2, 2);
    tb_sound_destroy(sound);

    /* A GUID that is not a format tag's, such as Ambisonic B-format's. */
    file.bytes[20 + 26] = 0x01;
    TB_TEST_EXPECT_UINT(tb_wav_read(file.bytes, file.length, NULL, &sound, &error), ==,
                        TB_WAV_UNSUPPORTED);
    TB_TEST_EXPECT_STRING(error.chunk, ==, "fmt ");
}

/* Chunks of unknown names are skipped, odd ones over their padding byte;
 * the last chunk's padding may be missing. */
TB_TEST(media_wav, unknown_chunks_and_padding_are_skipped)
{
    wav_builder file = {0};
    const unsigned char odd[3] = {'a', 'b', 'c'};
    const unsigned char samples[3] = {1, 2, 3};
    wav_begin(&file, "RIFF");
    wav_chunk(&file, "JUNK", odd, sizeof(odd));
    wav_chunk(&file, "LIST", odd, 1);
    wav_fmt(&file, 1, 1, 8000, 1, 8);
    wav_chunk(&file, "bext", odd, 2);
    const size_t data = wav_open_chunk(&file, "data");
    wav_put(&file, samples, sizeof(samples));
    wav_close_chunk(&file, data);
    file.length--; /* The padding of data. */
    wav_finish(&file);
    tb_sound *const sound = read_built(&file);
    expect_spec(sound, TB_SOUND_FORMAT_U8, 8000, 1, 3);
    tb_sound_destroy(sound);
}

/* A fmt chunk longer than this reader keeps is read for its fields and
 * skipped for the rest; a RIFF size of 0 means the file's own. */
TB_TEST(media_wav, long_fmt_chunks_and_placeholder_sizes)
{
    wav_builder file = {0};
    unsigned char fmt[100] = {0};
    const unsigned char samples[4] = {9, 8, 7, 6};
    fmt[0] = 1;
    fmt[2] = 2;
    fmt[4] = 0x40;
    fmt[5] = 0x1F;
    fmt[12] = 2;
    fmt[14] = 8;
    wav_begin(&file, "RIFF");
    wav_chunk(&file, "fmt ", fmt, sizeof(fmt));
    wav_chunk(&file, "data", samples, sizeof(samples));
    tb_sound *const sound = read_built(&file);
    expect_spec(sound, TB_SOUND_FORMAT_U8, 8000, 2, 2);
    tb_sound_destroy(sound);
}

/* A smpl chunk of `count` loops, each 4 values of `loops`: type, start,
 * inclusive end, and play count. */
static void put_smpl(wav_builder *file, const uint32_t *loops, uint32_t count)
{
    const size_t chunk = wav_open_chunk(file, "smpl");
    for (int field = 0; field < 7; field++)
    {
        wav_put_u32(file, 0);
    }
    wav_put_u32(file, count);
    wav_put_u32(file, 0);
    for (uint32_t index = 0; index < count; index++)
    {
        wav_put_u32(file, index);
        const uint32_t *const loop = loops + (size_t)index * 4;
        wav_put_u32(file, loop[0]);
        wav_put_u32(file, loop[1]);
        wav_put_u32(file, loop[2]);
        wav_put_u32(file, 0);
        wav_put_u32(file, loop[3]);
    }
    wav_close_chunk(file, chunk);
}

/* `smpl` loops, after the samples as they usually are: an end is
 * inclusive in the file; loops past the frames or of unknown types drop. */
TB_TEST(media_wav, smpl_gives_the_loops)
{
    static const uint32_t loops[5][4] = {
        {0, 2, 7, 0}, {1, 0, 0, 3}, {2, 5, 9, 1}, {0, 4, 10, 0}, {32, 0, 1, 0},
    };
    const unsigned char samples[10] = {0};
    wav_builder file = {0};
    tb_sound_loop loop = {0};
    wav_begin(&file, "RIFF");
    wav_fmt(&file, 1, 1, 8000, 1, 8);
    wav_chunk(&file, "data", samples, sizeof(samples));
    put_smpl(&file, &loops[0][0], 5);
    wav_finish(&file);
    tb_sound *const sound = read_built(&file);
    TB_TEST_REQUIRE_UINT(tb_sound_loop_count(sound), ==, 3);
    tb_sound_get_loop(sound, 0, &loop);
    TB_TEST_EXPECT_UINT(loop.start, ==, 2);
    TB_TEST_EXPECT_UINT(loop.end, ==, 8);
    TB_TEST_EXPECT_UINT(loop.kind, ==, TB_SOUND_LOOP_FORWARD);
    tb_sound_get_loop(sound, 1, &loop);
    TB_TEST_EXPECT_UINT(loop.end, ==, 1);
    TB_TEST_EXPECT_UINT(loop.kind, ==, TB_SOUND_LOOP_PING_PONG);
    TB_TEST_EXPECT_UINT(loop.play_count, ==, 3);
    tb_sound_get_loop(sound, 2, &loop);
    TB_TEST_EXPECT_UINT(loop.kind, ==, TB_SOUND_LOOP_BACKWARD);
    TB_TEST_EXPECT_UINT(loop.play_count, ==, 1);
    tb_sound_destroy(sound);
}

TB_TEST(media_wav, loops_past_what_a_sound_keeps_are_dropped)
{
    uint32_t loops[TB_SOUND_MAX_LOOPS + 4][4] = {{0}};
    const unsigned char samples[4] = {0};
    wav_builder file = {0};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(loops); index++)
    {
        loops[index][1] = 0;
        loops[index][2] = 3;
    }
    wav_begin(&file, "RIFF");
    put_smpl(&file, &loops[0][0], (uint32_t)TB_TEST_COUNT_OF(loops));
    wav_fmt(&file, 1, 1, 8000, 1, 8);
    wav_chunk(&file, "data", samples, sizeof(samples));
    wav_finish(&file);
    tb_sound *const sound = read_built(&file);
    TB_TEST_EXPECT_UINT(tb_sound_loop_count(sound), ==, TB_SOUND_MAX_LOOPS);
    tb_sound_destroy(sound);
}

/* RF64 keeps its sizes in ds64, where the 32-bit fields say 0xFFFFFFFF. */
TB_TEST(media_wav, rf64_takes_its_sizes_from_ds64)
{
    static const char *const forms[2] = {"RF64", "BW64"};
    for (size_t form = 0; form < 2; form++)
    {
        wav_builder file = {0};
        const unsigned char samples[6] = {1, 0, 2, 0, 3, 0};
        TB_TEST_CONTEXT("%s", forms[form]);
        wav_begin(&file, forms[form]);
        const size_t ds64 = wav_open_chunk(&file, "ds64");
        wav_put_u64(&file, 0);
        wav_put_u64(&file, sizeof(samples));
        wav_put_u64(&file, 3);
        wav_put_u32(&file, 0);
        wav_close_chunk(&file, ds64);
        wav_fmt(&file, 1, 1, 8000, 2, 16);
        wav_put(&file, "data", 4);
        wav_put_u32(&file, 0xFFFFFFFFu);
        wav_put(&file, samples, sizeof(samples));
        wav_patch_u32(&file, 4, 0xFFFFFFFFu);
        wav_patch_u32(&file, 20, (uint32_t)(file.length - 8));
        tb_sound *const sound = read_built(&file);
        expect_spec(sound, TB_SOUND_FORMAT_S16, 8000, 1, 3);
        tb_sound_destroy(sound);
    }
}

/* -------------------------------------------------------------------------
 * Streams that cannot seek.
 * ------------------------------------------------------------------------- */

typedef struct forward_stream
{
    const unsigned char *bytes;
    size_t length;
    size_t position;
} forward_stream;

static tb_stream_result forward_read(void *state, void *bytes, size_t capacity, size_t *transferred)
{
    forward_stream *const stream = state;
    const size_t left = stream->length - stream->position;
    /* At most 5 bytes at a time, to make every caller loop. */
    size_t part = capacity < left ? capacity : left;
    part = part < 5 ? part : 5;
    if (part != 0)
    {
        memcpy(bytes, stream->bytes + stream->position, part);
    }
    stream->position += part;
    *transferred = part;
    return TB_STREAM_OK;
}

static tb_stream_result forward_release(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

static const tb_stream_functions forward_functions = {.read_bytes = forward_read,
                                                      .release = forward_release};

static tb_stream open_forward(forward_stream *state, const wav_builder *file)
{
    *state = (forward_stream){.bytes = file->bytes, .length = file->length};
    return (tb_stream){.functions = &forward_functions, .state = state};
}

/* Read in order, a file is read up to its samples: loops after them are
 * not seen, and a fmt after them is not either. */
TB_TEST(media_wav, a_stream_that_cannot_seek_is_read_in_order)
{
    static const uint32_t loops[1][4] = {{0, 0, 1, 0}};
    const unsigned char samples[4] = {1, 2, 3, 4};
    wav_builder file = {0};
    forward_stream state = {0};
    tb_sound *sound = NULL;
    tb_wav_error error = {0};
    wav_begin(&file, "RIFF");
    put_smpl(&file, &loops[0][0], 1);
    wav_chunk(&file, "JUNK", samples, 3);
    wav_fmt(&file, 1, 1, 8000, 1, 8);
    wav_chunk(&file, "data", samples, sizeof(samples));
    put_smpl(&file, &loops[0][0], 1);
    wav_finish(&file);

    tb_stream stream = open_forward(&state, &file);
    TB_TEST_REQUIRE_UINT(tb_wav_read_stream(&stream, NULL, &sound, &error), ==, TB_WAV_OK);
    expect_spec(sound, TB_SOUND_FORMAT_U8, 8000, 1, 4);
    TB_TEST_EXPECT_UINT(tb_sound_loop_count(sound), ==, 1);
    tb_sound_destroy(sound);

    wav_begin(&file, "RIFF");
    wav_chunk(&file, "data", samples, sizeof(samples));
    wav_fmt(&file, 1, 1, 8000, 1, 8);
    wav_finish(&file);
    stream = open_forward(&state, &file);
    TB_TEST_EXPECT_UINT(tb_wav_read_stream(&stream, NULL, &sound, &error), ==, TB_WAV_MALFORMED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "no fmt chunk before the samples");
    /* Where the stream can seek, the order does not matter. */
    tb_sound *const seeking = read_built(&file);
    tb_sound_destroy(seeking);
}

/* A file that ends early is damage, whether its size was known or not. */
TB_TEST(media_wav, samples_cut_short_are_malformed)
{
    const unsigned char samples[8] = {0};
    wav_builder file = {0};
    forward_stream state = {0};
    tb_sound *sound = NULL;
    tb_wav_error error = {0};
    wav_begin(&file, "RIFF");
    wav_fmt(&file, 1, 1, 8000, 2, 16);
    wav_chunk(&file, "data", samples, sizeof(samples));
    wav_finish(&file);
    file.length -= 3;
    tb_stream stream = open_forward(&state, &file);
    TB_TEST_EXPECT_UINT(tb_wav_read_stream(&stream, NULL, &sound, &error), ==, TB_WAV_MALFORMED);
    TB_TEST_EXPECT_STRING(error.chunk, ==, "data");
    TB_TEST_EXPECT_STRING(error.reason, ==, "the file ends before its samples do");
    TB_TEST_EXPECT_POINTER(sound, ==, NULL);
    TB_TEST_EXPECT_UINT(tb_wav_read(file.bytes, file.length, NULL, &sound, &error), ==,
                        TB_WAV_MALFORMED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the chunk runs past the end of the file");

    /* Cut inside the smpl chunk before the samples. */
    static const uint32_t loops[1][4] = {{0, 0, 1, 0}};
    wav_begin(&file, "RIFF");
    put_smpl(&file, &loops[0][0], 1);
    file.length = 66; /* Its header, and part of its loop. */
    stream = open_forward(&state, &file);
    TB_TEST_EXPECT_UINT(tb_wav_read_stream(&stream, NULL, &sound, &error), ==, TB_WAV_MALFORMED);
    TB_TEST_EXPECT_STRING(error.chunk, ==, "smpl");
    file.length = 50; /* Part of its header. */
    stream = open_forward(&state, &file);
    TB_TEST_EXPECT_UINT(tb_wav_read_stream(&stream, NULL, &sound, &error), ==, TB_WAV_MALFORMED);
    TB_TEST_EXPECT_STRING(error.chunk, ==, "smpl");
}

/* An RF64 file read in order can claim samples past what memory addresses,
 * where decoding makes more bytes than it reads: G.711, which doubles
 * them, and ADPCM, which quadruples them. Nothing is allocated. */
TB_TEST(media_wav, samples_past_memory_are_too_large)
{
    static const uint32_t formats[2][4] = {{7, 1, 1, 8}, {0x11, 1, 8, 4}};
    for (size_t index = 0; index < 2; index++)
    {
        wav_builder file = {0};
        forward_stream state = {0};
        tb_sound *sound = NULL;
        tb_wav_error error = {0};
        TB_TEST_CONTEXT("format tag %u", formats[index][0]);
        wav_begin(&file, "RF64");
        const size_t ds64 = wav_open_chunk(&file, "ds64");
        wav_put_u64(&file, 0);
        wav_put_u64(&file, UINT64_MAX / 2 + 1);
        wav_put_u64(&file, 0);
        wav_put_u32(&file, 0);
        wav_close_chunk(&file, ds64);
        wav_fmt(&file, formats[index][0], formats[index][1], 8000, formats[index][2],
                formats[index][3]);
        wav_put(&file, "data", 4);
        wav_put_u32(&file, 0xFFFFFFFFu);
        tb_stream stream = open_forward(&state, &file);
        TB_TEST_EXPECT_UINT(tb_wav_read_stream(&stream, NULL, &sound, &error), ==,
                            TB_WAV_TOO_LARGE);
        TB_TEST_EXPECT_STRING(error.reason, ==, "more samples than memory can address");
    }
}

/* -------------------------------------------------------------------------
 * The reader.
 * ------------------------------------------------------------------------- */

static void build_ramp(wav_builder *file, uint32_t frames)
{
    wav_begin(file, "RIFF");
    wav_fmt(file, 1, 2, 8000, 4, 16);
    const size_t data = wav_open_chunk(file, "data");
    for (uint32_t frame = 0; frame < frames; frame++)
    {
        wav_put_u16(file, frame);
        wav_put_u16(file, 1000 + frame);
    }
    wav_close_chunk(file, data);
    wav_finish(file);
}

/* Blocks of any size decode the same frames as one read of them all, and
 * a seek goes back to any frame. */
TB_TEST(media_wav, the_reader_decodes_a_block_at_a_time_and_seeks)
{
    wav_builder file = {0};
    tb_stream stream = {0};
    tb_wav_reader *reader = NULL;
    tb_wav_info info = {0};
    int16_t block[15] = {0};
    size_t frames = 0;
    size_t total = 0;
    build_ramp(&file, 100);
    tb_stream_open_memory(file.bytes, file.length, NULL, &stream);
    TB_TEST_REQUIRE_UINT(tb_wav_reader_open(&stream, NULL, &reader, NULL), ==, TB_WAV_OK);
    tb_wav_reader_info(reader, &info);
    TB_TEST_EXPECT_UINT(info.frame_count, ==, 100);
    TB_TEST_EXPECT_UINT(info.format_tag, ==, 1);
    TB_TEST_EXPECT_UINT(info.bits_per_sample, ==, 16);
    do
    {
        /* 7 frames, and 2 bytes that are not a whole frame. */
        TB_TEST_REQUIRE_UINT(tb_wav_reader_read(reader, block, sizeof(block), &frames, NULL), ==,
                             TB_WAV_OK);
        for (size_t frame = 0; frame < frames; frame++)
        {
            TB_TEST_EXPECT_INT(block[frame * 2], ==, (int)(total + frame));
            TB_TEST_EXPECT_INT(block[frame * 2 + 1], ==, (int)(1000 + total + frame));
        }
        total += frames;
    } while (frames != 0);
    TB_TEST_EXPECT_UINT(total, ==, 100);
    TB_TEST_EXPECT_UINT(tb_wav_reader_position(reader), ==, 100);
    TB_TEST_REQUIRE_UINT(tb_wav_reader_seek(reader, 42, NULL), ==, TB_WAV_OK);
    TB_TEST_REQUIRE_UINT(tb_wav_reader_read(reader, block, 4, &frames, NULL), ==, TB_WAV_OK);
    TB_TEST_EXPECT_UINT(frames, ==, 1);
    TB_TEST_EXPECT_INT(block[0], ==, 42);
    TB_TEST_REQUIRE_UINT(tb_wav_reader_read(reader, block, 3, &frames, NULL), ==, TB_WAV_OK);
    TB_TEST_EXPECT_UINT(frames, ==, 0);
    tb_wav_reader_close(reader);
    tb_wav_reader_close(NULL);
    (void)tb_stream_close(&stream);
}

/* ADPCM seeks to the block that holds a frame and decodes into it. */
TB_TEST(media_wav, the_reader_seeks_inside_adpcm_blocks)
{
    wav_builder file = {0};
    tb_stream stream = {0};
    tb_wav_reader *reader = NULL;
    int16_t samples[4] = {0};
    size_t frames = 0;
    wav_begin(&file, "RIFF");
    wav_fmt(&file, 0x11, 1, 22050, 8, 4);
    const size_t data = wav_open_chunk(&file, "data");
    put_ima_mono_block(&file, 0x77);
    put_ima_mono_block(&file, 0x77);
    wav_close_chunk(&file, data);
    wav_finish(&file);
    tb_stream_open_memory(file.bytes, file.length, NULL, &stream);
    TB_TEST_REQUIRE_UINT(tb_wav_reader_open(&stream, NULL, &reader, NULL), ==, TB_WAV_OK);
    TB_TEST_REQUIRE_UINT(tb_wav_reader_seek(reader, 12, NULL), ==, TB_WAV_OK);
    TB_TEST_REQUIRE_UINT(tb_wav_reader_read(reader, samples, sizeof(samples), &frames, NULL), ==,
                         TB_WAV_OK);
    TB_TEST_EXPECT_UINT(frames, ==, 4);
    TB_TEST_EXPECT_INT(samples[0], ==, 104);
    TB_TEST_EXPECT_INT(samples[1], ==, 240);
    TB_TEST_REQUIRE_UINT(tb_wav_reader_seek(reader, 9, NULL), ==, TB_WAV_OK);
    TB_TEST_REQUIRE_UINT(tb_wav_reader_read(reader, samples, 2, &frames, NULL), ==, TB_WAV_OK);
    TB_TEST_EXPECT_INT(samples[0], ==, 0);
    tb_wav_reader_close(reader);
    (void)tb_stream_close(&stream);
}

TB_TEST(media_wav, a_reader_of_a_stream_that_cannot_seek_says_so)
{
    wav_builder file = {0};
    forward_stream state = {0};
    tb_wav_reader *reader = NULL;
    tb_wav_error error = {0};
    build_ramp(&file, 4);
    tb_stream stream = open_forward(&state, &file);
    TB_TEST_REQUIRE_UINT(tb_wav_reader_open(&stream, NULL, &reader, NULL), ==, TB_WAV_OK);
    TB_TEST_EXPECT_UINT(tb_wav_reader_seek(reader, 0, &error), ==, TB_WAV_UNSEEKABLE);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the stream cannot seek");
    tb_wav_reader_close(reader);
}

/* A file inside a stream starts at the stream's position. */
TB_TEST(media_wav, a_file_starts_where_its_stream_is)
{
    wav_builder file = {0};
    tb_stream stream = {0};
    tb_sound *sound = NULL;
    uint64_t position = 0;
    unsigned char bytes[WAV_BUILDER_CAPACITY + 7] = {0};
    build_ramp(&file, 5);
    memcpy(bytes + 7, file.bytes, file.length);
    tb_stream_open_memory(bytes, file.length + 7, NULL, &stream);
    TB_TEST_REQUIRE_UINT(tb_stream_seek(&stream, 7, TB_STREAM_FROM_START, &position), ==,
                         TB_STREAM_OK);
    TB_TEST_REQUIRE_UINT(tb_wav_read_stream(&stream, NULL, &sound, NULL), ==, TB_WAV_OK);
    TB_TEST_EXPECT_FLOAT(tb_sound_get_sample(sound, 4, 1), ==, 1004.0f / 32768.0f);
    tb_sound_destroy(sound);
    (void)tb_stream_close(&stream);
}

/* -------------------------------------------------------------------------
 * Through a VFS.
 * ------------------------------------------------------------------------- */

#define SOUND_PATH "sounds/ramp.wav"

typedef struct memory_file
{
    const unsigned char *bytes;
    size_t length;
    bool failing;
} memory_file;

static tb_vfs_result memory_open(void *state, const char *name, size_t length,
                                 const tb_allocator *allocator, tb_stream *stream)
{
    memory_file *const file = state;
    if (length != tb_str_length(SOUND_PATH) || memcmp(name, SOUND_PATH, length) != 0)
    {
        return TB_VFS_NOT_FOUND;
    }
    if (file->failing)
    {
        return TB_VFS_INPUT_FAILED;
    }
    tb_stream_open_memory(file->bytes, file->length, allocator, stream);
    return TB_VFS_OK;
}

static tb_vfs_result memory_query(void *state, const char *name, size_t length, tb_vfs_info *info)
{
    (void)state;
    (void)name;
    (void)length;
    (void)info;
    return TB_VFS_NOT_FOUND;
}

static tb_vfs_result memory_list(void *state, const char *directory, size_t length,
                                 tb_vfs_list_function *function, void *context)
{
    (void)state;
    (void)directory;
    (void)length;
    (void)function;
    (void)context;
    return TB_VFS_NOT_FOUND;
}

static void memory_release(void *state)
{
    (void)state;
}

static const tb_vfs_source_functions memory_functions = {memory_open, memory_query, memory_list,
                                                         memory_release};

TB_TEST(media_wav, files_load_through_a_vfs_and_errors_name_them)
{
    wav_builder file = {0};
    memory_file source = {0};
    tb_vfs *const vfs = tb_vfs_create(NULL);
    tb_sound *sound = NULL;
    tb_wav_error error = {0};
    build_ramp(&file, 3);
    source = (memory_file){.bytes = file.bytes, .length = file.length};
    TB_TEST_REQUIRE_UINT(tb_vfs_mount(vfs, "", 0, (tb_vfs_source){&memory_functions, &source}, 0),
                         ==, TB_VFS_OK);
    TB_TEST_REQUIRE_UINT(tb_wav_load_file(vfs, SOUND_PATH, NULL, &sound, &error), ==, TB_WAV_OK);
    TB_TEST_EXPECT_UINT(tb_sound_frame_count(sound), ==, 3);
    tb_sound_destroy(sound);

    TB_TEST_EXPECT_UINT(tb_wav_load_file(vfs, "sounds/none.wav", NULL, &sound, &error), ==,
                        TB_WAV_NOT_FOUND);
    TB_TEST_EXPECT_STRING(error.message, ==, "sounds/none.wav: no such file");
    TB_TEST_EXPECT_POINTER(sound, ==, NULL);

    file.bytes[20] = 0x55; /* MP3 in a WAV. */
    TB_TEST_EXPECT_UINT(tb_wav_load_file(vfs, SOUND_PATH, NULL, &sound, &error), ==,
                        TB_WAV_UNSUPPORTED);
    TB_TEST_EXPECT_STRING(error.message, ==,
                          SOUND_PATH ": fmt  chunk at byte 12: a format tag this reader cannot "
                                     "decode");
    TB_TEST_EXPECT_UINT(error.offset, ==, 12);

    source.failing = true;
    TB_TEST_EXPECT_UINT(tb_wav_load_file(vfs, SOUND_PATH, NULL, &sound, NULL), ==,
                        TB_WAV_INPUT_FAILED);
    tb_vfs_destroy(vfs);
}

TB_TEST(media_wav, results_have_names)
{
    static const char *const names[] = {
        "ok",        "not found",     "input failed",  "malformed",  "unsupported",
        "too large", "out of memory", "output failed", "cannot seek"};
    for (int result = TB_WAV_OK; result <= TB_WAV_UNSEEKABLE; result++)
    {
        TB_TEST_EXPECT_STRING(tb_wav_result_name((tb_wav_result)result), ==, names[result]);
    }
    TB_TEST_EXPECT_STRING(tb_wav_result_name((tb_wav_result)99), ==, "unknown result");
}
