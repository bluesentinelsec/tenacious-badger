/* WAV files that are damaged or not WAV at all: each is refused with the
 * result, chunk, and reason that say what is wrong, and no file, however
 * mutated or cut, reads out of bounds (run under AddressSanitizer). */
#include "badger/allocator.h"
#include "badger/sound.h"
#include "badger/stream.h"
#include "badger/wav.h"
#include "tb_test.h"
#include "wav_builder.h"

#include <stdint.h>
#include <string.h>

typedef void build_function(wav_builder *file);

typedef struct malformed_case
{
    const char *name;
    build_function *build;
    tb_wav_result result;
    const char *chunk;
    const char *reason;
} malformed_case;

static void put_pcm16_mono(wav_builder *file)
{
    wav_begin(file, "RIFF");
    wav_fmt(file, 1, 1, 8000, 2, 16);
    const unsigned char samples[4] = {1, 2, 3, 4};
    wav_chunk(file, "data", samples, sizeof(samples));
    wav_finish(file);
}

static void build_empty(wav_builder *file)
{
    file->length = 0;
}

static void build_short(wav_builder *file)
{
    wav_begin(file, "RIFF");
    file->length = 11;
}

static void build_not_riff(wav_builder *file)
{
    put_pcm16_mono(file);
    memcpy(file->bytes, "OggS", 4);
}

static void build_rifx(wav_builder *file)
{
    put_pcm16_mono(file);
    memcpy(file->bytes, "RIFX", 4);
}

static void build_not_wave(wav_builder *file)
{
    put_pcm16_mono(file);
    memcpy(file->bytes + 8, "AVI ", 4);
}

static void build_no_fmt(wav_builder *file)
{
    const unsigned char samples[2] = {0};
    wav_begin(file, "RIFF");
    wav_chunk(file, "data", samples, sizeof(samples));
    wav_finish(file);
}

static void build_no_data(wav_builder *file)
{
    wav_begin(file, "RIFF");
    wav_fmt(file, 1, 1, 8000, 2, 16);
    wav_finish(file);
}

static void build_short_fmt(wav_builder *file)
{
    const unsigned char fmt[14] = {1, 0, 1, 0};
    wav_begin(file, "RIFF");
    wav_chunk(file, "fmt ", fmt, sizeof(fmt));
    wav_finish(file);
}

static void build_two_fmts(wav_builder *file)
{
    wav_begin(file, "RIFF");
    wav_fmt(file, 1, 1, 8000, 2, 16);
    wav_fmt(file, 1, 1, 8000, 2, 16);
    wav_finish(file);
}

static void build_two_datas(wav_builder *file)
{
    put_pcm16_mono(file);
    const unsigned char samples[2] = {0};
    wav_chunk(file, "data", samples, sizeof(samples));
    wav_finish(file);
}

/* A file of PCM whose fmt fields are these. */
static void build_fmt(wav_builder *file, uint32_t tag, uint32_t channels, uint32_t rate,
                      uint32_t block_align, uint32_t bits)
{
    const unsigned char samples[64] = {0};
    wav_begin(file, "RIFF");
    wav_fmt(file, tag, channels, rate, block_align, bits);
    wav_chunk(file, "data", samples, sizeof(samples));
    wav_finish(file);
}

static void build_no_channels(wav_builder *file)
{
    build_fmt(file, 1, 0, 8000, 2, 16);
}

static void build_no_rate(wav_builder *file)
{
    build_fmt(file, 1, 1, 0, 2, 16);
}

static void build_no_block_align(wav_builder *file)
{
    build_fmt(file, 1, 1, 8000, 0, 16);
}

static void build_too_many_channels(wav_builder *file)
{
    build_fmt(file, 1, 33, 8000, 66, 16);
}

static void build_absurd_rate(wav_builder *file)
{
    build_fmt(file, 1, 1, 1000000000u, 2, 16);
}

static void build_zero_bits(wav_builder *file)
{
    build_fmt(file, 1, 1, 8000, 2, 0);
}

static void build_wide_bits(wav_builder *file)
{
    build_fmt(file, 1, 1, 8000, 8, 64);
}

static void build_wrong_block_align(wav_builder *file)
{
    build_fmt(file, 1, 2, 8000, 3, 16);
}

static void build_narrow_container(wav_builder *file)
{
    build_fmt(file, 1, 1, 8000, 1, 16);
}

static void build_float_16(wav_builder *file)
{
    build_fmt(file, 3, 1, 8000, 2, 16);
}

static void build_float_block_align(wav_builder *file)
{
    build_fmt(file, 3, 2, 8000, 4, 32);
}

static void build_law_16(wav_builder *file)
{
    build_fmt(file, 7, 1, 8000, 2, 16);
}

static void build_ima_3_bits(wav_builder *file)
{
    build_fmt(file, 0x11, 1, 8000, 8, 3);
}

static void build_ima_bad_block(wav_builder *file)
{
    build_fmt(file, 0x11, 2, 8000, 12, 4);
}

static void build_mp3(wav_builder *file)
{
    build_fmt(file, 0x55, 1, 8000, 1, 0);
}

static void build_short_extensible(wav_builder *file)
{
    const unsigned char samples[2] = {0};
    wav_begin(file, "RIFF");
    wav_fmt(file, 0xFFFE, 1, 8000, 2, 16);
    wav_chunk(file, "data", samples, sizeof(samples));
    wav_finish(file);
}

static void build_chunk_past_end(wav_builder *file)
{
    put_pcm16_mono(file);
    const unsigned char junk[4] = {0};
    const size_t chunk = wav_open_chunk(file, "JUNK");
    wav_put(file, junk, sizeof(junk));
    wav_close_chunk(file, chunk);
    wav_patch_u32(file, chunk + 4, 1000);
    wav_finish(file);
}

static void build_data_past_end(wav_builder *file)
{
    put_pcm16_mono(file);
    wav_patch_u32(file, 40, 0x7FFFFFF0u);
}

static void build_short_fact(wav_builder *file)
{
    const unsigned char fact[2] = {0};
    wav_begin(file, "RIFF");
    wav_chunk(file, "fact", fact, sizeof(fact));
    wav_finish(file);
}

static void build_short_smpl(wav_builder *file)
{
    const unsigned char smpl[20] = {0};
    wav_begin(file, "RIFF");
    wav_chunk(file, "smpl", smpl, sizeof(smpl));
    wav_finish(file);
}

static void build_smpl_overflowing_loops(wav_builder *file)
{
    unsigned char smpl[36 + 24] = {0};
    smpl[28] = 2; /* Two loops, room for one. */
    wav_begin(file, "RIFF");
    wav_chunk(file, "smpl", smpl, sizeof(smpl));
    wav_finish(file);
}

static void build_rf64_without_ds64(wav_builder *file)
{
    put_pcm16_mono(file);
    memcpy(file->bytes, "RF64", 4);
}

static void build_short_ds64(wav_builder *file)
{
    const unsigned char ds64[8] = {0};
    wav_begin(file, "RF64");
    wav_chunk(file, "ds64", ds64, sizeof(ds64));
    wav_finish(file);
}

static const malformed_case cases[] = {
    {"empty", build_empty, TB_WAV_MALFORMED, "", "too short to be a WAV file"},
    {"short", build_short, TB_WAV_MALFORMED, "", "too short to be a WAV file"},
    {"not RIFF", build_not_riff, TB_WAV_MALFORMED, "", "not a RIFF file"},
    {"RIFX", build_rifx, TB_WAV_UNSUPPORTED, "", "a big-endian RIFX file"},
    {"not WAVE", build_not_wave, TB_WAV_MALFORMED, "", "a RIFF file not of WAVE"},
    {"no fmt", build_no_fmt, TB_WAV_MALFORMED, "", "no fmt chunk before the samples"},
    {"no data", build_no_data, TB_WAV_MALFORMED, "", "no data chunk"},
    {"short fmt", build_short_fmt, TB_WAV_MALFORMED, "fmt ", "a fmt chunk shorter than 16 bytes"},
    {"two fmts", build_two_fmts, TB_WAV_MALFORMED, "fmt ", "a second fmt chunk"},
    {"two datas", build_two_datas, TB_WAV_MALFORMED, "data", "a second data chunk"},
    {"no channels", build_no_channels, TB_WAV_MALFORMED, "fmt ", "no channels"},
    {"no rate", build_no_rate, TB_WAV_MALFORMED, "fmt ", "a sample rate of 0"},
    {"no block align", build_no_block_align, TB_WAV_MALFORMED, "fmt ", "a block align of 0"},
    {"33 channels", build_too_many_channels, TB_WAV_TOO_LARGE, "fmt ", "more than 32 channels"},
    {"1 GHz", build_absurd_rate, TB_WAV_TOO_LARGE, "fmt ", "a sample rate above 768 kHz"},
    {"0 bits", build_zero_bits, TB_WAV_UNSUPPORTED, "fmt ",
     "PCM samples of 0 or more than 32 bits"},
    {"64 bits", build_wide_bits, TB_WAV_UNSUPPORTED, "fmt ",
     "PCM samples of 0 or more than 32 bits"},
    {"block align", build_wrong_block_align, TB_WAV_MALFORMED, "fmt ",
     "a block align that does not fit the channels and bits"},
    {"narrow container", build_narrow_container, TB_WAV_MALFORMED, "fmt ",
     "a block align that does not fit the channels and bits"},
    {"float 16", build_float_16, TB_WAV_UNSUPPORTED, "fmt ",
     "float samples of other than 32 or 64 bits"},
    {"float block align", build_float_block_align, TB_WAV_MALFORMED, "fmt ",
     "a block align that does not fit the channels and bits"},
    {"law 16", build_law_16, TB_WAV_MALFORMED, "fmt ", "G.711 samples of other than 8 bits"},
    {"IMA 3 bits", build_ima_3_bits, TB_WAV_UNSUPPORTED, "fmt ", "IMA ADPCM of other than 4 bits"},
    {"IMA block", build_ima_bad_block, TB_WAV_MALFORMED, "fmt ",
     "an IMA ADPCM block that is not whole words of its channels"},
    {"MP3", build_mp3, TB_WAV_UNSUPPORTED, "fmt ", "a format tag this reader cannot decode"},
    {"short EXTENSIBLE", build_short_extensible, TB_WAV_MALFORMED, "fmt ",
     "a WAVE_FORMAT_EXTENSIBLE fmt chunk shorter than 40 bytes"},
    {"chunk past end", build_chunk_past_end, TB_WAV_MALFORMED, "JUNK",
     "the chunk runs past the end of the file"},
    {"data past end", build_data_past_end, TB_WAV_MALFORMED, "data",
     "the chunk runs past the end of the file"},
    {"short fact", build_short_fact, TB_WAV_MALFORMED, "fact", "a fact chunk shorter than 4 bytes"},
    {"short smpl", build_short_smpl, TB_WAV_MALFORMED, "smpl",
     "a smpl chunk shorter than 36 bytes"},
    {"smpl loops", build_smpl_overflowing_loops, TB_WAV_MALFORMED, "smpl",
     "more loops than the smpl chunk holds"},
    {"RF64 without ds64", build_rf64_without_ds64, TB_WAV_MALFORMED, "fmt ",
     "an RF64 file whose first chunk is not ds64"},
    {"short ds64", build_short_ds64, TB_WAV_MALFORMED, "ds64",
     "a ds64 chunk too short for its sizes"},
};

TB_TEST(media_wav_malformed, each_fault_is_named_with_its_chunk)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        static wav_builder file;
        tb_sound *sound = NULL;
        tb_wav_error error = {0};
        TB_TEST_CONTEXT("%s", cases[index].name);
        memset(&file, 0, sizeof(file));
        cases[index].build(&file);
        TB_TEST_EXPECT_UINT(tb_wav_read(file.bytes, file.length, NULL, &sound, &error), ==,
                            cases[index].result);
        TB_TEST_EXPECT_POINTER(sound, ==, NULL);
        TB_TEST_EXPECT_UINT(error.result, ==, cases[index].result);
        TB_TEST_EXPECT_STRING(error.chunk, ==, cases[index].chunk);
        TB_TEST_EXPECT_STRING(error.reason, ==, cases[index].reason);
        TB_TEST_EXPECT(tb_test_case_has_failed() || error.message[0] != '\0');
        /* Without an error to fill, the result is the same. */
        TB_TEST_EXPECT_UINT(tb_wav_read(file.bytes, file.length, NULL, &sound, NULL), ==,
                            cases[index].result);
    }
}

TB_TEST(media_wav_malformed, the_message_says_where)
{
    wav_builder file = {0};
    tb_sound *sound = NULL;
    tb_wav_error error = {0};
    build_no_channels(&file);
    TB_TEST_EXPECT_UINT(tb_wav_read(file.bytes, file.length, NULL, &sound, &error), ==,
                        TB_WAV_MALFORMED);
    TB_TEST_EXPECT_UINT(error.offset, ==, 12);
    TB_TEST_EXPECT_STRING(error.message, ==, "fmt  chunk at byte 12: no channels");
    build_not_wave(&file);
    TB_TEST_EXPECT_UINT(tb_wav_read(file.bytes, file.length, NULL, &sound, &error), ==,
                        TB_WAV_MALFORMED);
    TB_TEST_EXPECT_STRING(error.message, ==, "byte 8: a RIFF file not of WAVE");
}

/* A stream of a file's bytes that fails, as a disk does, after `left` of them. */
typedef struct failing_stream
{
    const unsigned char *bytes;
    size_t left;
} failing_stream;

static tb_stream_result failing_read(void *state, void *bytes, size_t capacity, size_t *transferred)
{
    failing_stream *const stream = state;
    *transferred = 0;
    if (stream->left == 0)
    {
        return TB_STREAM_OS_ERROR;
    }
    const size_t part = capacity < stream->left ? capacity : stream->left;
    memcpy(bytes, stream->bytes, part);
    stream->bytes += part;
    stream->left -= part;
    *transferred = part;
    return TB_STREAM_OK;
}

static tb_stream_result failing_release(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

static const tb_stream_functions failing_functions = {.read_bytes = failing_read,
                                                      .release = failing_release};

/* A stream that fails to read, in the header, among the chunks, or in the
 * samples, is input that failed, not damage. */
TB_TEST(media_wav_malformed, a_stream_that_fails_is_input_that_failed)
{
    static const size_t failures[] = {0, 12, 20, 44, 45};
    static const char *const chunks[] = {"", "", "fmt ", "data", "data"};
    wav_builder file = {0};
    put_pcm16_mono(&file);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(failures); index++)
    {
        failing_stream state = {.bytes = file.bytes, .left = failures[index]};
        tb_stream stream = {.functions = &failing_functions, .state = &state};
        tb_sound *sound = NULL;
        tb_wav_error error = {0};
        TB_TEST_CONTEXT("after %zu bytes", failures[index]);
        TB_TEST_EXPECT_UINT(tb_wav_read_stream(&stream, NULL, &sound, &error), ==,
                            TB_WAV_INPUT_FAILED);
        TB_TEST_EXPECT_STRING(error.reason, ==, "operating system error");
        TB_TEST_EXPECT_STRING(error.chunk, ==, chunks[index]);
    }
}

/* A refusing allocator: samples it cannot hold are out of memory, which a
 * caller survives. */
static void *refusing_allocate(void *context, void *memory, size_t old_size, size_t new_size,
                               size_t alignment)
{
    const tb_allocator heap = tb_allocator_heap();
    const size_t *const limit = context;
    if (new_size > *limit)
    {
        return NULL;
    }
    return heap.function(heap.context, memory, old_size, new_size, alignment);
}

TB_TEST(media_wav_malformed, samples_without_memory_are_reported)
{
    wav_builder file = {0};
    size_t limit = 1024;
    const tb_allocator allocator = {.function = refusing_allocate, .context = &limit};
    tb_sound *sound = NULL;
    tb_wav_error error = {0};
    const unsigned char samples[4000] = {0};
    wav_begin(&file, "RIFF");
    wav_fmt(&file, 1, 1, 8000, 1, 8);
    wav_chunk(&file, "data", samples, sizeof(samples));
    wav_finish(&file);
    TB_TEST_EXPECT_UINT(tb_wav_read(file.bytes, file.length, &allocator, &sound, &error), ==,
                        TB_WAV_NO_MEMORY);
    TB_TEST_EXPECT_STRING(error.reason, ==, "no memory for the samples");
    TB_TEST_EXPECT_POINTER(sound, ==, NULL);
}

/* Valid files of every kind the reader decodes, to mutate. */
static void build_seed(wav_builder *file, size_t kind)
{
    const unsigned char samples[96] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                                       13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
    wav_begin(file, kind == 5 ? "RF64" : "RIFF");
    if (kind == 5)
    {
        const size_t ds64 = wav_open_chunk(file, "ds64");
        wav_put_u64(file, 0);
        wav_put_u64(file, sizeof(samples));
        wav_put_u64(file, 0);
        wav_put_u32(file, 0);
        wav_close_chunk(file, ds64);
    }
    switch (kind)
    {
    case 0:
        wav_fmt(file, 1, 2, 8000, 4, 16);
        break;
    case 1:
        wav_fmt(file, 3, 1, 8000, 8, 64);
        break;
    case 2:
        wav_fmt(file, 7, 2, 8000, 2, 8);
        break;
    case 3:
        wav_fmt(file, 0x11, 2, 8000, 24, 4);
        break;
    case 4:
        wav_fmt_extensible(file, 1, 3, 8000, 9, 24, 0x7);
        break;
    default:
        wav_fmt(file, 1, 1, 8000, 3, 24);
        break;
    }
    const size_t fact = wav_open_chunk(file, "fact");
    wav_put_u32(file, 20);
    wav_close_chunk(file, fact);
    wav_chunk(file, "data", samples, sizeof(samples));
    unsigned char smpl[36 + 48] = {0};
    smpl[28] = 2;
    smpl[36 + 12] = 3;
    smpl[60 + 4] = 1;
    smpl[60 + 12] = 9;
    wav_chunk(file, "smpl", smpl, sizeof(smpl));
    wav_finish(file);
    if (kind == 5)
    {
        wav_patch_u32(file, 4, 0xFFFFFFFFu);
    }
}

/* Read a file whole and a block at a time; whatever it is, nothing reads
 * out of bounds and whatever is made is released. */
static void read_every_way(const unsigned char *bytes, size_t length)
{
    tb_sound *sound = NULL;
    tb_stream stream = {0};
    tb_wav_reader *reader = NULL;
    if (tb_wav_read(bytes, length, NULL, &sound, NULL) == TB_WAV_OK)
    {
        (void)tb_sound_peak(sound);
        tb_sound_destroy(sound);
    }
    tb_stream_open_memory(bytes, length, NULL, &stream);
    if (tb_wav_reader_open(&stream, NULL, &reader, NULL) == TB_WAV_OK)
    {
        unsigned char block[100] = {0};
        size_t frames = 0;
        tb_wav_info info = {0};
        tb_wav_reader_info(reader, &info);
        while (tb_wav_reader_read(reader, block, sizeof(block), &frames, NULL) == TB_WAV_OK &&
               frames != 0)
        {
        }
        if (info.frame_count != 0)
        {
            (void)tb_wav_reader_seek(reader, info.frame_count / 2, NULL);
            (void)tb_wav_reader_read(reader, block, sizeof(block), &frames, NULL);
        }
        tb_wav_reader_close(reader);
    }
    (void)tb_stream_close(&stream);
}

TB_TEST(media_wav_malformed, the_seeds_read)
{
    for (size_t kind = 0; kind < 6; kind++)
    {
        static wav_builder file;
        tb_sound *sound = NULL;
        tb_wav_error error = {0};
        TB_TEST_CONTEXT("seed %zu", kind);
        memset(&file, 0, sizeof(file));
        build_seed(&file, kind);
        TB_TEST_EXPECT_UINT(tb_wav_read(file.bytes, file.length, NULL, &sound, &error), ==,
                            TB_WAV_OK);
        TB_TEST_EXPECT_STRING(error.message, ==, "");
        tb_sound_destroy(sound);
        read_every_way(file.bytes, file.length);
    }
}

/* Every cut of every seed, and thousands of random mutations: bytes
 * changed, sizes made large, bytes removed. */
TB_TEST(media_wav_malformed, mutations_never_read_out_of_bounds)
{
    static wav_builder seed;
    static unsigned char mutated[WAV_BUILDER_CAPACITY];
    for (size_t kind = 0; kind < 6; kind++)
    {
        memset(&seed, 0, sizeof(seed));
        build_seed(&seed, kind);
        for (size_t length = 0; length < seed.length; length++)
        {
            /* A copy of exactly the length, so a read past it is caught. */
            unsigned char *const cut = tb_allocator_alloc(NULL, length + 1, 1);
            memcpy(cut, seed.bytes, length);
            read_every_way(cut, length);
            tb_allocator_free(NULL, cut, length + 1, 1);
        }
        for (size_t round = 0; round < 1500; round++)
        {
            size_t length = seed.length;
            memcpy(mutated, seed.bytes, seed.length);
            const size_t changes = 1 + (size_t)tb_test_random_below(4);
            for (size_t change = 0; change < changes; change++)
            {
                const size_t at = (size_t)tb_test_random_below(length);
                switch (tb_test_random_below(4))
                {
                case 0:
                    mutated[at] = (unsigned char)tb_test_random_below(256);
                    break;
                case 1:
                    mutated[at] = 0xFF;
                    break;
                case 2:
                    mutated[at] = 0;
                    break;
                default:
                    length = at + 1;
                    break;
                }
            }
            unsigned char *const copy = tb_allocator_alloc(NULL, length, 1);
            memcpy(copy, mutated, length);
            read_every_way(copy, length);
            tb_allocator_free(NULL, copy, length, 1);
        }
    }
}
