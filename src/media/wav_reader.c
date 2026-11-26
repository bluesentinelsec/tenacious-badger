/* Reading WAV files: the chunks of the header, found by walking the RIFF
 * list, and the frames of the `data` chunk, decoded a block at a time. A
 * stream that can seek is walked to its end, so chunks after `data` are
 * found; one that cannot is walked up to `data`, skipping by reading. */
#include "badger/wav.h"

#include "badger/assert.h"
#include "badger/str.h"
#include "badger/vfs.h"
#include "sound_internal.h"
#include "wav_internal.h"

#include <stdbool.h>
#include <string.h>

/* Bytes of the RIFF header: its id, its size, and the form `WAVE`. */
#define RIFF_HEADER 12u
/* Bytes of the fields of ds64 that this reader uses: the RIFF and data sizes. */
#define DS64_SIZE 16u
/* The most bytes of a `fmt ` chunk that are read; the rest is skipped. */
#define FMT_MOST 64u
/* The size of a 32-bit chunk that RF64 gives in ds64 instead. */
#define RF64_SIZE_IN_DS64 0xFFFFFFFFu
/* Bytes of raw samples decoded at a time. */
#define SCRATCH_SIZE 4096u
/* The largest frame of a file read without a codec: 32 channels of 8 bytes. */
#define MOST_RAW_FRAME (TB_SOUND_MAX_CHANNELS * 8u)

typedef enum wav_encoding
{
    WAV_INTEGER, /* PCM, kept in its bytes. */
    WAV_FLOAT32,
    WAV_FLOAT64,
    WAV_ALAW,
    WAV_MULAW,
    WAV_IMA_ADPCM
} wav_encoding;

struct tb_wav_reader
{
    tb_allocator allocator;
    tb_stream *stream;
    const char *name; /* For errors' messages; NULL for none. */
    bool seekable;
    uint64_t base;   /* The stream's position at the file's first byte. */
    uint64_t cursor; /* The stream's position, from the file's first byte. */
    uint64_t end;    /* Bytes the stream has from `base`; UINT64_MAX when unknown. */
    uint64_t data_start;
    uint64_t data_size;
    uint64_t data_read; /* Bytes of `data` consumed. */
    wav_encoding encoding;
    uint32_t container;   /* Bytes of a sample in the file. */
    uint32_t block_align; /* Bytes of a frame, or of an ADPCM block. */
    size_t frames_per_block;
    size_t output_frame_size;
    tb_wav_info info;
    size_t position;
    /* ADPCM: the raw block, and its frames decoded, of which `decoded_cursor` is next. */
    unsigned char *block;
    int16_t *decoded;
    size_t decoded_frames;
    size_t decoded_cursor;
};

/* What the walk of the chunks found. */
typedef struct wav_header
{
    bool rf64;
    uint64_t rf64_data_size;
    bool has_fmt;
    uint64_t fmt_offset;
    size_t fmt_size;
    unsigned char fmt[FMT_MOST];
    bool has_fact;
    uint32_t fact_frames;
    bool has_data;
    /* The loops of `smpl`, as many as a sound keeps, read as the walk
     * passes them and checked once the frames are counted. */
    size_t smpl_loops;
    unsigned char smpl[TB_WAV_SMPL_LOOP * TB_SOUND_MAX_LOOPS];
} wav_header;

static tb_wav_result reader_fail(const tb_wav_reader *reader, tb_wav_error *error,
                                 tb_wav_result result, const char *chunk, uint64_t offset,
                                 const char *reason)
{
    return tb_wav_fail(error, result, reader->name, chunk, offset, reason);
}

/* A failure of the stream: its end is damage to the file, anything else is input. */
static tb_wav_result stream_fail(const tb_wav_reader *reader, tb_wav_error *error,
                                 tb_stream_result result, const char *chunk, uint64_t offset)
{
    if (result == TB_STREAM_END)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, chunk, offset,
                           "the file ends inside the chunk");
    }
    return reader_fail(reader, error, TB_WAV_INPUT_FAILED, chunk, offset,
                       tb_stream_result_name(result));
}

/* Move the stream to `offset` of the file: by seeking, or by reading forward. */
static tb_stream_result move_to(tb_wav_reader *reader, uint64_t offset)
{
    unsigned char discarded[256] = {0};

    if (offset == reader->cursor)
    {
        return TB_STREAM_OK;
    }
    if (reader->seekable)
    {
        uint64_t position = 0;
        if (offset > (uint64_t)INT64_MAX - reader->base)
        {
            return TB_STREAM_TOO_LARGE;
        }
        const tb_stream_result sought = tb_stream_seek(
            reader->stream, (int64_t)(reader->base + offset), TB_STREAM_FROM_START, &position);
        if (sought == TB_STREAM_OK)
        {
            reader->cursor = offset;
        }
        return sought;
    }
    TB_ASSERT(offset > reader->cursor);
    while (reader->cursor < offset)
    {
        const uint64_t left = offset - reader->cursor;
        const size_t part = left < sizeof(discarded) ? (size_t)left : sizeof(discarded);
        const tb_stream_result read = tb_stream_read_exact(reader->stream, discarded, part);
        if (read != TB_STREAM_OK)
        {
            return read;
        }
        reader->cursor += part;
    }
    return TB_STREAM_OK;
}

/* Read `length` bytes at `offset` of the file. */
static tb_stream_result read_at(tb_wav_reader *reader, uint64_t offset, void *bytes, size_t length)
{
    tb_stream_result result = move_to(reader, offset);
    if (result != TB_STREAM_OK)
    {
        return result;
    }
    result = tb_stream_read_exact(reader->stream, bytes, length);
    if (result == TB_STREAM_OK)
    {
        reader->cursor += length;
    }
    return result;
}

/* -------------------------------------------------------------------------
 * The walk of the chunks.
 * ------------------------------------------------------------------------- */

/* The chunks' end: what the RIFF header says, unless the stream says less,
 * or the header's size is a placeholder of a writer that never went back. */
static uint64_t chunks_end(const tb_wav_reader *reader, uint64_t riff_size)
{
    const bool placeholder = riff_size == 0 || riff_size == RF64_SIZE_IN_DS64;
    if (placeholder || riff_size > reader->end - TB_WAV_CHUNK_HEADER)
    {
        return reader->end;
    }
    return riff_size + TB_WAV_CHUNK_HEADER;
}

static tb_wav_result read_riff(tb_wav_reader *reader, wav_header *header, uint64_t *limit,
                               tb_wav_error *error)
{
    unsigned char bytes[RIFF_HEADER] = {0};
    const tb_stream_result read = read_at(reader, 0, bytes, sizeof(bytes));

    if (read == TB_STREAM_END)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "", 0, "too short to be a WAV file");
    }
    if (read != TB_STREAM_OK)
    {
        return stream_fail(reader, error, read, "", 0);
    }
    if (memcmp(bytes, "RIFX", 4) == 0)
    {
        return reader_fail(reader, error, TB_WAV_UNSUPPORTED, "", 0, "a big-endian RIFX file");
    }
    header->rf64 = memcmp(bytes, "RF64", 4) == 0 || memcmp(bytes, "BW64", 4) == 0;
    if (memcmp(bytes, "RIFF", 4) != 0 && !header->rf64)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "", 0, "not a RIFF file");
    }
    if (memcmp(bytes + 8, "WAVE", 4) != 0)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "", 8, "a RIFF file not of WAVE");
    }
    *limit = chunks_end(reader, tb_wav_u32(bytes + 4));
    return TB_WAV_OK;
}

/* ds64, the first chunk of an RF64 file: the 64-bit sizes of RIFF and data. */
static tb_wav_result read_ds64(tb_wav_reader *reader, wav_header *header, uint64_t offset,
                               uint64_t size, uint64_t *limit, tb_wav_error *error)
{
    unsigned char bytes[DS64_SIZE] = {0};

    if (size < DS64_SIZE)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "ds64", offset,
                           "a ds64 chunk too short for its sizes");
    }
    const tb_stream_result read =
        read_at(reader, offset + TB_WAV_CHUNK_HEADER, bytes, sizeof(bytes));
    if (read != TB_STREAM_OK)
    {
        return stream_fail(reader, error, read, "ds64", offset);
    }
    *limit = chunks_end(reader, tb_wav_u64(bytes));
    header->rf64_data_size = tb_wav_u64(bytes + 8);
    return TB_WAV_OK;
}

static tb_wav_result read_fmt(tb_wav_reader *reader, wav_header *header, uint64_t offset,
                              uint64_t size, tb_wav_error *error)
{
    if (header->has_fmt)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "fmt ", offset, "a second fmt chunk");
    }
    if (size < TB_WAV_FMT_SIZE)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "fmt ", offset,
                           "a fmt chunk shorter than 16 bytes");
    }
    header->fmt_size = size < FMT_MOST ? (size_t)size : FMT_MOST;
    const tb_stream_result read =
        read_at(reader, offset + TB_WAV_CHUNK_HEADER, header->fmt, header->fmt_size);
    if (read != TB_STREAM_OK)
    {
        return stream_fail(reader, error, read, "fmt ", offset);
    }
    header->has_fmt = true;
    header->fmt_offset = offset;
    return TB_WAV_OK;
}

static tb_wav_result read_fact(tb_wav_reader *reader, wav_header *header, uint64_t offset,
                               uint64_t size, tb_wav_error *error)
{
    unsigned char bytes[4] = {0};

    if (size < sizeof(bytes))
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "fact", offset,
                           "a fact chunk shorter than 4 bytes");
    }
    const tb_stream_result read =
        read_at(reader, offset + TB_WAV_CHUNK_HEADER, bytes, sizeof(bytes));
    if (read != TB_STREAM_OK)
    {
        return stream_fail(reader, error, read, "fact", offset);
    }
    header->has_fact = true;
    header->fact_frames = tb_wav_u32(bytes);
    return TB_WAV_OK;
}

static tb_wav_result read_smpl(tb_wav_reader *reader, wav_header *header, uint64_t offset,
                               uint64_t size, tb_wav_error *error)
{
    unsigned char bytes[TB_WAV_SMPL_HEADER] = {0};

    if (size < TB_WAV_SMPL_HEADER)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "smpl", offset,
                           "a smpl chunk shorter than 36 bytes");
    }
    tb_stream_result read = read_at(reader, offset + TB_WAV_CHUNK_HEADER, bytes, sizeof(bytes));
    if (read != TB_STREAM_OK)
    {
        return stream_fail(reader, error, read, "smpl", offset);
    }
    const uint64_t declared = tb_wav_u32(bytes + 28);
    if (declared > (size - TB_WAV_SMPL_HEADER) / TB_WAV_SMPL_LOOP)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "smpl", offset,
                           "more loops than the smpl chunk holds");
    }
    header->smpl_loops = declared < TB_SOUND_MAX_LOOPS ? (size_t)declared : TB_SOUND_MAX_LOOPS;
    read = read_at(reader, reader->cursor, header->smpl, header->smpl_loops * TB_WAV_SMPL_LOOP);
    if (read != TB_STREAM_OK)
    {
        return stream_fail(reader, error, read, "smpl", offset);
    }
    return TB_WAV_OK;
}

/* One chunk of the walk, whose header is at `offset` and whose body ends
 * by `limit`; `next` is where the following one starts. */
static tb_wav_result read_chunk(tb_wav_reader *reader, wav_header *header, uint64_t offset,
                                const unsigned char *bytes, uint64_t *limit, uint64_t *next,
                                tb_wav_error *error)
{
    char id[5] = {0};

    memcpy(id, bytes, 4);
    uint64_t size = tb_wav_u32(bytes + 4);
    const bool is_data = memcmp(id, "data", 4) == 0;
    if (is_data && header->rf64 && size == RF64_SIZE_IN_DS64)
    {
        size = header->rf64_data_size;
    }
    const uint64_t body = offset + TB_WAV_CHUNK_HEADER;
    if (size > reader->end - body)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, id, offset,
                           "the chunk runs past the end of the file");
    }
    /* A chunk of odd size is padded to an even one; the last one's padding may be missing. */
    *next = body + size + (size & 1u);
    if (offset == RIFF_HEADER && header->rf64)
    {
        if (memcmp(id, "ds64", 4) != 0)
        {
            return reader_fail(reader, error, TB_WAV_MALFORMED, id, offset,
                               "an RF64 file whose first chunk is not ds64");
        }
        return read_ds64(reader, header, offset, size, limit, error);
    }
    if (memcmp(id, "fmt ", 4) == 0)
    {
        return read_fmt(reader, header, offset, size, error);
    }
    if (memcmp(id, "fact", 4) == 0)
    {
        return read_fact(reader, header, offset, size, error);
    }
    if (memcmp(id, "smpl", 4) == 0)
    {
        return read_smpl(reader, header, offset, size, error);
    }
    if (is_data)
    {
        if (header->has_data)
        {
            return reader_fail(reader, error, TB_WAV_MALFORMED, id, offset, "a second data chunk");
        }
        header->has_data = true;
        reader->data_start = body;
        reader->data_size = size;
    }
    return TB_WAV_OK;
}

static tb_wav_result walk_chunks(tb_wav_reader *reader, wav_header *header, tb_wav_error *error)
{
    uint64_t limit = 0;
    uint64_t offset = RIFF_HEADER;
    tb_wav_result result = read_riff(reader, header, &limit, error);

    while (result == TB_WAV_OK && limit >= TB_WAV_CHUNK_HEADER &&
           offset <= limit - TB_WAV_CHUNK_HEADER)
    {
        unsigned char bytes[TB_WAV_CHUNK_HEADER] = {0};
        uint64_t next = 0;
        const tb_stream_result read = read_at(reader, offset, bytes, sizeof(bytes));
        if (read == TB_STREAM_END)
        {
            /* A stream of unknown size ends where its chunks do. */
            break;
        }
        if (read != TB_STREAM_OK)
        {
            return stream_fail(reader, error, read, "", offset);
        }
        result = read_chunk(reader, header, offset, bytes, &limit, &next, error);
        /* A stream that cannot seek stops at the samples, which come next. */
        if (header->has_data && !reader->seekable)
        {
            break;
        }
        offset = next;
    }
    return result;
}

/* -------------------------------------------------------------------------
 * What the header says.
 * ------------------------------------------------------------------------- */

/* The sub-format of WAVE_FORMAT_EXTENSIBLE, and its channel mask. */
static tb_wav_result read_extensible(tb_wav_reader *reader, const wav_header *header, uint16_t *tag,
                                     tb_wav_error *error)
{
    const unsigned char *const fmt = header->fmt;

    if (header->fmt_size < TB_WAV_FMT_EXTENSIBLE_SIZE ||
        tb_wav_u16(fmt + 16) < TB_WAV_EXTENSIBLE_EXTRA)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "fmt ", header->fmt_offset,
                           "a WAVE_FORMAT_EXTENSIBLE fmt chunk shorter than 40 bytes");
    }
    if (memcmp(fmt + 26, tb_wav_subformat_tail, sizeof(tb_wav_subformat_tail)) != 0)
    {
        return reader_fail(reader, error, TB_WAV_UNSUPPORTED, "fmt ", header->fmt_offset,
                           "a sub-format that is not a format tag");
    }
    reader->info.spec.channel_mask = tb_wav_u32(fmt + 20);
    *tag = tb_wav_u16(fmt + 24);
    return TB_WAV_OK;
}

/* PCM: whatever its bits, a sample is read by the bytes it occupies. */
static tb_wav_result choose_integer(tb_wav_reader *reader, uint32_t bits, uint32_t channels,
                                    const wav_header *header, tb_wav_error *error)
{
    static const tb_sound_format formats[] = {TB_SOUND_FORMAT_U8, TB_SOUND_FORMAT_S16,
                                              TB_SOUND_FORMAT_S24, TB_SOUND_FORMAT_S32};
    const uint32_t container = reader->block_align / channels;

    if (bits == 0 || bits > 32)
    {
        return reader_fail(reader, error, TB_WAV_UNSUPPORTED, "fmt ", header->fmt_offset,
                           "PCM samples of 0 or more than 32 bits");
    }
    if (reader->block_align % channels != 0 || container == 0 || container > 4 ||
        container * 8 < bits)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "fmt ", header->fmt_offset,
                           "a block align that does not fit the channels and bits");
    }
    reader->encoding = WAV_INTEGER;
    reader->container = container;
    reader->info.spec.format = formats[container - 1];
    return TB_WAV_OK;
}

static tb_wav_result choose_float(tb_wav_reader *reader, uint32_t bits, uint32_t channels,
                                  const wav_header *header, tb_wav_error *error)
{
    if (bits != 32 && bits != 64)
    {
        return reader_fail(reader, error, TB_WAV_UNSUPPORTED, "fmt ", header->fmt_offset,
                           "float samples of other than 32 or 64 bits");
    }
    reader->container = bits / 8;
    if (reader->block_align != reader->container * channels)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "fmt ", header->fmt_offset,
                           "a block align that does not fit the channels and bits");
    }
    reader->encoding = bits == 32 ? WAV_FLOAT32 : WAV_FLOAT64;
    reader->info.spec.format = TB_SOUND_FORMAT_F32;
    return TB_WAV_OK;
}

static tb_wav_result choose_law(tb_wav_reader *reader, uint16_t tag, uint32_t bits,
                                uint32_t channels, const wav_header *header, tb_wav_error *error)
{
    if (bits != 8 || reader->block_align != channels)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "fmt ", header->fmt_offset,
                           "G.711 samples of other than 8 bits");
    }
    reader->encoding = tag == TB_WAV_FORMAT_ALAW ? WAV_ALAW : WAV_MULAW;
    reader->container = 1;
    reader->info.spec.format = TB_SOUND_FORMAT_S16;
    return TB_WAV_OK;
}

static tb_wav_result choose_ima(tb_wav_reader *reader, uint32_t bits, uint32_t channels,
                                const wav_header *header, tb_wav_error *error)
{
    const uint32_t group = 4 * channels;

    if (bits != 4)
    {
        return reader_fail(reader, error, TB_WAV_UNSUPPORTED, "fmt ", header->fmt_offset,
                           "IMA ADPCM of other than 4 bits");
    }
    if (reader->block_align < group || (reader->block_align - group) % group != 0)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "fmt ", header->fmt_offset,
                           "an IMA ADPCM block that is not whole words of its channels");
    }
    reader->encoding = WAV_IMA_ADPCM;
    reader->frames_per_block = 1 + (size_t)(reader->block_align - group) / group * 8;
    reader->info.spec.format = TB_SOUND_FORMAT_S16;
    return TB_WAV_OK;
}

static tb_wav_result read_format(tb_wav_reader *reader, const wav_header *header,
                                 tb_wav_error *error)
{
    const unsigned char *const fmt = header->fmt;
    uint16_t tag = tb_wav_u16(fmt);
    const uint32_t channels = tb_wav_u16(fmt + 2);
    const uint32_t rate = tb_wav_u32(fmt + 4);
    const uint32_t bits = tb_wav_u16(fmt + 14);
    tb_wav_result result = TB_WAV_OK;

    reader->block_align = tb_wav_u16(fmt + 12);
    reader->info.bits_per_sample = (uint16_t)bits;
    if (channels == 0)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "fmt ", header->fmt_offset,
                           "no channels");
    }
    if (rate == 0)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "fmt ", header->fmt_offset,
                           "a sample rate of 0");
    }
    if (reader->block_align == 0)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "fmt ", header->fmt_offset,
                           "a block align of 0");
    }
    if (channels > TB_SOUND_MAX_CHANNELS)
    {
        return reader_fail(reader, error, TB_WAV_TOO_LARGE, "fmt ", header->fmt_offset,
                           "more than 32 channels");
    }
    if (rate > TB_SOUND_MAX_RATE)
    {
        return reader_fail(reader, error, TB_WAV_TOO_LARGE, "fmt ", header->fmt_offset,
                           "a sample rate above 768 kHz");
    }
    reader->info.spec.channels = channels;
    reader->info.spec.sample_rate = rate;
    if (tag == TB_WAV_FORMAT_EXTENSIBLE)
    {
        result = read_extensible(reader, header, &tag, error);
        if (result != TB_WAV_OK)
        {
            return result;
        }
    }
    reader->info.format_tag = tag;
    switch (tag)
    {
    case TB_WAV_FORMAT_PCM:
        return choose_integer(reader, bits, channels, header, error);
    case TB_WAV_FORMAT_IEEE_FLOAT:
        return choose_float(reader, bits, channels, header, error);
    case TB_WAV_FORMAT_ALAW:
    case TB_WAV_FORMAT_MULAW:
        return choose_law(reader, tag, bits, channels, header, error);
    case TB_WAV_FORMAT_IMA_ADPCM:
        return choose_ima(reader, bits, channels, header, error);
    default:
        break;
    }
    return reader_fail(reader, error, TB_WAV_UNSUPPORTED, "fmt ", header->fmt_offset,
                       "a format tag this reader cannot decode");
}

/* Frames in the data chunk; for ADPCM, its last block may be cut short. */
static tb_wav_result count_frames(tb_wav_reader *reader, const wav_header *header,
                                  tb_wav_error *error)
{
    uint64_t frames = reader->data_size / reader->block_align;
    size_t bytes = 0;

    if (reader->encoding == WAV_IMA_ADPCM)
    {
        const uint64_t remainder = reader->data_size % reader->block_align;
        const uint64_t group = 4u * (uint64_t)reader->info.spec.channels;
        const uint64_t partial = remainder >= group ? 1 + (remainder - group) / group * 8 : 0;
        if (frames > (UINT64_MAX - partial) / reader->frames_per_block)
        {
            return reader_fail(reader, error, TB_WAV_TOO_LARGE, "data", reader->data_start - 8,
                               "more samples than memory can address");
        }
        frames = frames * reader->frames_per_block + partial;
        if (header->has_fact && header->fact_frames < frames)
        {
            frames = header->fact_frames;
        }
    }
    if (frames > SIZE_MAX || !tb_sound_bytes_of(&reader->info.spec, (size_t)frames, &bytes))
    {
        return reader_fail(reader, error, TB_WAV_TOO_LARGE, "data", reader->data_start - 8,
                           "more samples than memory can address");
    }
    reader->info.frame_count = (size_t)frames;
    return TB_WAV_OK;
}

static tb_sound_loop_kind loop_kind(uint32_t type, bool *known)
{
    *known = type <= 2;
    if (type == 1)
    {
        return TB_SOUND_LOOP_PING_PONG;
    }
    if (type == 2)
    {
        return TB_SOUND_LOOP_BACKWARD;
    }
    return TB_SOUND_LOOP_FORWARD;
}

/* The loops of `smpl` that lie within the frames, once they are counted. */
static void read_loops(tb_wav_reader *reader, const wav_header *header)
{
    for (size_t index = 0; index < header->smpl_loops; index++)
    {
        const unsigned char *const loop = header->smpl + index * TB_WAV_SMPL_LOOP;
        const uint64_t start = tb_wav_u32(loop + 8);
        const uint64_t last = tb_wav_u32(loop + 12);
        bool known = false;
        const tb_sound_loop_kind kind = loop_kind(tb_wav_u32(loop + 4), &known);
        if (known && start <= last && last < reader->info.frame_count)
        {
            reader->info.loops[reader->info.loop_count] =
                (tb_sound_loop){.start = (size_t)start,
                                .end = (size_t)last + 1,
                                .play_count = tb_wav_u32(loop + 20),
                                .kind = kind};
            reader->info.loop_count++;
        }
    }
}

static tb_wav_result read_header(tb_wav_reader *reader, tb_wav_error *error)
{
    wav_header header = {0};
    tb_wav_result result = walk_chunks(reader, &header, error);

    if (result != TB_WAV_OK)
    {
        return result;
    }
    if (!header.has_fmt)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "", RIFF_HEADER,
                           "no fmt chunk before the samples");
    }
    if (!header.has_data)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "", RIFF_HEADER, "no data chunk");
    }
    result = read_format(reader, &header, error);
    if (result == TB_WAV_OK)
    {
        result = count_frames(reader, &header, error);
    }
    if (result == TB_WAV_OK)
    {
        read_loops(reader, &header);
    }
    return result;
}

/* -------------------------------------------------------------------------
 * The reader.
 * ------------------------------------------------------------------------- */

static tb_wav_result open_reader(tb_stream *stream, const tb_allocator *allocator, const char *name,
                                 tb_wav_reader **reader, tb_wav_error *error)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    uint64_t size = 0;

    TB_ASSERT(stream != NULL);
    TB_ASSERT(reader != NULL);
    *reader = NULL;
    tb_wav_reader *const made = TB_ALLOCATOR_NEW(&resolved, tb_wav_reader);
    made->allocator = resolved;
    made->stream = stream;
    made->name = name;
    made->end = UINT64_MAX;
    made->seekable = tb_stream_seek(stream, 0, TB_STREAM_FROM_CURRENT, &made->base) == TB_STREAM_OK;
    if (made->seekable && tb_stream_size(stream, &size) == TB_STREAM_OK)
    {
        made->end = size > made->base ? size - made->base : 0;
    }
    tb_wav_result result = read_header(made, error);
    if (result == TB_WAV_OK && made->seekable && move_to(made, made->data_start) != TB_STREAM_OK)
    {
        /* coverage-off: a stream that seeks and reads the header then fails to seek back */
        result = reader_fail(made, error, TB_WAV_INPUT_FAILED, "data", made->data_start - 8,
                             "the stream cannot return to the samples");
        /* coverage-on */
    }
    if (result != TB_WAV_OK)
    {
        TB_ALLOCATOR_DELETE(&resolved, tb_wav_reader, made);
        return result;
    }
    made->output_frame_size =
        tb_sound_format_size(made->info.spec.format) * made->info.spec.channels;
    if (made->encoding == WAV_IMA_ADPCM)
    {
        made->block = tb_allocator_alloc(&resolved, made->block_align, 1);
        made->decoded = TB_ALLOCATOR_NEW_ARRAY(&resolved, int16_t,
                                               made->frames_per_block * made->info.spec.channels);
    }
    *reader = made;
    return TB_WAV_OK;
}

tb_wav_result tb_wav_reader_open(tb_stream *stream, const tb_allocator *allocator,
                                 tb_wav_reader **reader, tb_wav_error *error)
{
    return open_reader(stream, allocator, NULL, reader, error);
}

void tb_wav_reader_close(tb_wav_reader *reader)
{
    if (reader == NULL)
    {
        return;
    }
    const tb_allocator allocator = reader->allocator;
    if (reader->encoding == WAV_IMA_ADPCM)
    {
        tb_allocator_free(&allocator, reader->block, reader->block_align, 1);
        TB_ALLOCATOR_DELETE_ARRAY(&allocator, int16_t, reader->decoded,
                                  reader->frames_per_block * reader->info.spec.channels);
    }
    TB_ALLOCATOR_DELETE(&allocator, tb_wav_reader, reader);
}

void tb_wav_reader_info(const tb_wav_reader *reader, tb_wav_info *info)
{
    TB_ASSERT(reader != NULL);
    TB_ASSERT(info != NULL);
    *info = reader->info;
}

size_t tb_wav_reader_position(const tb_wav_reader *reader)
{
    TB_ASSERT(reader != NULL);
    return reader->position;
}

/* Read the next `length` bytes of the data chunk. */
static tb_wav_result read_data(tb_wav_reader *reader, void *bytes, size_t length,
                               tb_wav_error *error)
{
    const uint64_t offset = reader->data_start + reader->data_read;
    const tb_stream_result read = read_at(reader, offset, bytes, length);

    if (read == TB_STREAM_END)
    {
        return reader_fail(reader, error, TB_WAV_MALFORMED, "data", reader->data_start - 8,
                           "the file ends before its samples do");
    }
    if (read != TB_STREAM_OK)
    {
        return stream_fail(reader, error, read, "data", reader->data_start - 8);
    }
    reader->data_read += length;
    return TB_WAV_OK;
}

static void store_s16(unsigned char *target, int16_t value)
{
    memcpy(target, &value, sizeof(value));
}

/* Decode `count` samples of the file's bytes into the output format. */
static void decode_samples(const tb_wav_reader *reader, const unsigned char *source, size_t count,
                           unsigned char *target)
{
    for (size_t index = 0; index < count; index++)
    {
        const unsigned char *const sample = source + index * reader->container;
        switch (reader->encoding)
        {
        case WAV_INTEGER:
            if (reader->container == 2)
            {
                store_s16(target + index * 2, (int16_t)tb_wav_u16(sample));
            }
            else if (reader->container == 4)
            {
                const int32_t value = (int32_t)tb_wav_u32(sample);
                memcpy(target + index * 4, &value, sizeof(value));
            }
            else
            {
                /* U8 and S24 are stored as the file has them. */
                memcpy(target + index * reader->container, sample, reader->container);
            }
            break;
        case WAV_FLOAT32: {
            const uint32_t bits = tb_wav_u32(sample);
            memcpy(target + index * 4, &bits, sizeof(bits));
            break;
        }
        case WAV_FLOAT64: {
            const uint64_t bits = tb_wav_u64(sample);
            double value = 0.0;
            memcpy(&value, &bits, sizeof(value));
            tb_sound_store(TB_SOUND_FORMAT_F32, target + index * 4, value);
            break;
        }
        case WAV_ALAW:
            store_s16(target + index * 2, tb_wav_alaw(sample[0]));
            break;
        case WAV_MULAW:
            store_s16(target + index * 2, tb_wav_mulaw(sample[0]));
            break;
        case WAV_IMA_ADPCM:
            /* coverage-off: ADPCM is decoded a block at a time, never here */
            TB_ASSERT_MSG(false, "ADPCM is decoded a block at a time");
            break;
            /* coverage-on */
        }
    }
}

static tb_wav_result read_raw(tb_wav_reader *reader, unsigned char *target, size_t count,
                              tb_wav_error *error)
{
    unsigned char scratch[SCRATCH_SIZE] = {0};
    const size_t per_part = SCRATCH_SIZE / reader->block_align;

    while (count > 0)
    {
        const size_t part = count < per_part ? count : per_part;
        const tb_wav_result result = read_data(reader, scratch, part * reader->block_align, error);
        if (result != TB_WAV_OK)
        {
            return result;
        }
        decode_samples(reader, scratch, part * reader->info.spec.channels, target);
        target += part * reader->output_frame_size;
        reader->position += part;
        count -= part;
    }
    return TB_WAV_OK;
}

/* Decode the next block of ADPCM into `decoded`. */
static tb_wav_result load_block(tb_wav_reader *reader, tb_wav_error *error)
{
    const uint64_t left = reader->data_size - reader->data_read;
    const size_t length = left < reader->block_align ? (size_t)left : reader->block_align;
    const tb_wav_result result = read_data(reader, reader->block, length, error);

    if (result != TB_WAV_OK)
    {
        return result;
    }
    reader->decoded_frames =
        tb_wav_decode_ima_block(reader->block, length, reader->info.spec.channels, reader->decoded);
    reader->decoded_cursor = 0;
    return TB_WAV_OK;
}

static tb_wav_result read_ima(tb_wav_reader *reader, unsigned char *target, size_t count,
                              tb_wav_error *error)
{
    const size_t channels = reader->info.spec.channels;

    while (count > 0)
    {
        if (reader->decoded_cursor == reader->decoded_frames)
        {
            const tb_wav_result result = load_block(reader, error);
            if (result != TB_WAV_OK)
            {
                return result;
            }
        }
        const size_t available = reader->decoded_frames - reader->decoded_cursor;
        const size_t part = count < available ? count : available;
        memcpy(target, reader->decoded + reader->decoded_cursor * channels,
               part * reader->output_frame_size);
        reader->decoded_cursor += part;
        reader->position += part;
        target += part * reader->output_frame_size;
        count -= part;
    }
    return TB_WAV_OK;
}

tb_wav_result tb_wav_reader_read(tb_wav_reader *reader, void *samples, size_t length,
                                 size_t *frames, tb_wav_error *error)
{
    TB_ASSERT(reader != NULL);
    TB_ASSERT(frames != NULL);
    TB_ASSERT(samples != NULL || length == 0);
    const size_t left = reader->info.frame_count - reader->position;
    const size_t fit = length / reader->output_frame_size;
    const size_t count = fit < left ? fit : left;
    const size_t start = reader->position;
    tb_wav_result result = TB_WAV_OK;

    if (count != 0)
    {
        if (reader->encoding == WAV_IMA_ADPCM)
        {
            result = read_ima(reader, samples, count, error);
        }
        else
        {
            result = read_raw(reader, samples, count, error);
        }
    }
    *frames = reader->position - start;
    return result;
}

tb_wav_result tb_wav_reader_seek(tb_wav_reader *reader, size_t frame, tb_wav_error *error)
{
    TB_ASSERT(reader != NULL);
    TB_ASSERT(frame <= reader->info.frame_count);
    if (!reader->seekable)
    {
        return reader_fail(reader, error, TB_WAV_UNSEEKABLE, "data", reader->data_start - 8,
                           "the stream cannot seek");
    }
    if (reader->encoding != WAV_IMA_ADPCM)
    {
        reader->data_read = (uint64_t)frame * reader->block_align;
        reader->position = frame;
        return TB_WAV_OK;
    }
    const size_t block = frame / reader->frames_per_block;
    const size_t within = frame % reader->frames_per_block;
    reader->data_read = (uint64_t)block * reader->block_align;
    reader->decoded_frames = 0;
    reader->decoded_cursor = 0;
    reader->position = frame;
    if (within != 0)
    {
        const tb_wav_result result = load_block(reader, error);
        if (result != TB_WAV_OK)
        {
            return result;
        }
        reader->decoded_cursor = within;
    }
    return TB_WAV_OK;
}

/* -------------------------------------------------------------------------
 * Reading everything.
 * ------------------------------------------------------------------------- */

static tb_wav_result read_all(tb_stream *stream, const tb_allocator *allocator, const char *name,
                              tb_sound **sound, tb_wav_error *error)
{
    tb_wav_reader *reader = NULL;
    tb_sound *made = NULL;
    size_t frames = 0;
    tb_wav_result result = TB_WAV_OK;

    TB_ASSERT(sound != NULL);
    *sound = NULL;
    result = open_reader(stream, allocator, name, &reader, error);
    if (result != TB_WAV_OK)
    {
        return result;
    }
    made = tb_sound_try_create(&reader->info.spec, reader->info.frame_count, allocator);
    if (made == NULL)
    {
        result = reader_fail(reader, error, TB_WAV_NO_MEMORY, "data", reader->data_start - 8,
                             "no memory for the samples");
        goto cleanup;
    }
    result = tb_wav_reader_read(reader, made->owned, made->owned_size, &frames, error);
    if (result != TB_WAV_OK)
    {
        goto cleanup;
    }
    for (size_t index = 0; index < reader->info.loop_count; index++)
    {
        (void)tb_sound_add_loop(made, &reader->info.loops[index]);
    }
    *sound = made;
    made = NULL;
cleanup:
    tb_sound_destroy(made);
    tb_wav_reader_close(reader);
    return result;
}

tb_wav_result tb_wav_read_stream(tb_stream *stream, const tb_allocator *allocator, tb_sound **sound,
                                 tb_wav_error *error)
{
    return read_all(stream, allocator, NULL, sound, error);
}

tb_wav_result tb_wav_read(const void *bytes, size_t length, const tb_allocator *allocator,
                          tb_sound **sound, tb_wav_error *error)
{
    tb_stream stream = {0};

    TB_ASSERT(bytes != NULL || length == 0);
    tb_stream_open_memory(bytes, length, allocator, &stream);
    const tb_wav_result result = read_all(&stream, allocator, NULL, sound, error);
    (void)tb_stream_close(&stream);
    return result;
}

tb_wav_result tb_wav_load_file(struct tb_vfs *vfs, const char *path, const tb_allocator *allocator,
                               tb_sound **sound, tb_wav_error *error)
{
    tb_stream stream = {0};

    TB_ASSERT(vfs != NULL);
    TB_ASSERT(path != NULL);
    TB_ASSERT(sound != NULL);
    *sound = NULL;
    const tb_vfs_result opened = tb_vfs_open(vfs, path, tb_str_length(path), allocator, &stream);
    if (opened == TB_VFS_NOT_FOUND || opened == TB_VFS_NOT_A_FILE || opened == TB_VFS_INVALID)
    {
        return tb_wav_fail(error, TB_WAV_NOT_FOUND, path, "", TB_WAV_NOWHERE, "no such file");
    }
    if (opened != TB_VFS_OK)
    {
        return tb_wav_fail(error, TB_WAV_INPUT_FAILED, path, "", TB_WAV_NOWHERE,
                           tb_vfs_result_name(opened));
    }
    const tb_wav_result result = read_all(&stream, allocator, path, sound, error);
    (void)tb_stream_close(&stream);
    return result;
}
