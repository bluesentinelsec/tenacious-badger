/* DEFLATE decompression (RFC 1951) with zlib (RFC 1950) and gzip (RFC 1952)
 * framing.
 *
 * The decoder is a state machine that can stop at any byte of input or of
 * output and continue later, so a caller may feed it ranges of any size. Its
 * rules, which every state keeps:
 *
 * - Bits are pulled from the input one byte at a time, and only when the
 *   next step needs them. Between steps fewer than eight unused bits are
 *   held, so no byte past the end of the stream is ever consumed.
 * - A step either completes, or consumes nothing but the bytes it pulled
 *   into the bit buffer, which stay there for the next call.
 * - Every symbol and every distance is checked before it is used. A
 *   malformed stream is a result, kept until the inflater is reset.
 *
 * Huffman codes are decoded through a table of the first nine bits, which
 * holds every code that short; longer codes, and bit buffers too short for
 * the table, are decoded one bit at a time from the code's counts, as in
 * Mark Adler's puff.c. */
#include "badger/deflate.h"

#include "badger/assert.h"
#include "badger/checksum.h"

#include <string.h>

#define TB_INFLATE_WINDOW_SIZE ((size_t)32768)
#define TB_INFLATE_WINDOW_MASK (TB_INFLATE_WINDOW_SIZE - 1)
#define TB_HUFFMAN_FAST_BITS 9u
#define TB_HUFFMAN_FAST_SIZE ((size_t)1 << TB_HUFFMAN_FAST_BITS)
#define TB_HUFFMAN_FAST_MASK (TB_HUFFMAN_FAST_SIZE - 1)
#define TB_HUFFMAN_SYMBOL_MASK 0x1FFu
#define TB_HUFFMAN_MAX_BITS 15u
#define TB_LITERAL_SYMBOLS ((size_t)288)
#define TB_DISTANCE_SYMBOLS ((size_t)32)
#define TB_CODE_LENGTH_SYMBOLS ((size_t)19)
#define TB_END_OF_BLOCK 256u
#define TB_FIRST_LENGTH_SYMBOL 257u
#define TB_LENGTH_SYMBOLS 29u
#define TB_USED_DISTANCE_SYMBOLS 30u
#define TB_MAX_LITERAL_COUNT 286u
#define TB_GZIP_FIXED_HEADER ((size_t)10)
#define TB_GZIP_ID1 0x1Fu
#define TB_GZIP_ID2 0x8Bu
#define TB_GZIP_METHOD_DEFLATE 8u
#define TB_GZIP_FLAG_HEADER_CRC 0x02u
#define TB_GZIP_FLAG_EXTRA 0x04u
#define TB_GZIP_FLAG_NAME 0x08u
#define TB_GZIP_FLAG_COMMENT 0x10u
#define TB_GZIP_FLAG_RESERVED 0xE0u
#define TB_ZLIB_METHOD_DEFLATE 8u
#define TB_ZLIB_MAX_WINDOW_INFO 7u
#define TB_ZLIB_FLAG_DICTIONARY 0x20u
#define TB_ZLIB_CHECK_DIVISOR 31u

static const uint16_t tb_length_base[TB_LENGTH_SYMBOLS] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t tb_length_extra[TB_LENGTH_SYMBOLS] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t tb_distance_base[TB_USED_DISTANCE_SYMBOLS] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t tb_distance_extra[TB_USED_DISTANCE_SYMBOLS] = {
    0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
/* The order in which a dynamic block lists the lengths of the code-length code. */
static const uint8_t tb_code_length_order[TB_CODE_LENGTH_SYMBOLS] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/* A canonical Huffman code, ready to decode. `fast` maps the next nine bits
 * of input to (length << 9) | symbol for every code of up to nine bits, and
 * holds zero elsewhere. `count` and `symbol` decode any code a bit at a time. */
typedef struct tb_huffman
{
    uint16_t fast[TB_HUFFMAN_FAST_SIZE];
    uint16_t count[TB_HUFFMAN_MAX_BITS + 1];
    uint16_t symbol[TB_LITERAL_SYMBOLS];
    unsigned longest; /* The longest code's length; 0 for a code with none. */
} tb_huffman;

typedef enum tb_huffman_shape
{
    TB_HUFFMAN_COMPLETE, /* Every code of every length is used. */
    TB_HUFFMAN_SINGLE,   /* One code, of one bit: allowed, with the other bit pattern invalid. */
    TB_HUFFMAN_EMPTY,    /* No codes: allowed for distances, when a block has only literals. */
    TB_HUFFMAN_INVALID   /* Over-subscribed, or incomplete with more than one code. */
} tb_huffman_shape;

typedef enum tb_inflate_state
{
    TB_INFLATE_ZLIB_HEADER,
    TB_INFLATE_GZIP_HEADER,
    TB_INFLATE_GZIP_EXTRA_LENGTH,
    TB_INFLATE_GZIP_EXTRA,
    TB_INFLATE_GZIP_NAME,
    TB_INFLATE_GZIP_COMMENT,
    TB_INFLATE_GZIP_HEADER_CRC,
    TB_INFLATE_BLOCK_HEADER,
    TB_INFLATE_STORED_LENGTHS,
    TB_INFLATE_STORED_COPY,
    TB_INFLATE_TABLE_COUNTS,
    TB_INFLATE_CODE_LENGTH_LENGTHS,
    TB_INFLATE_CODE_LENGTHS,
    TB_INFLATE_LITERAL,
    TB_INFLATE_LENGTH_EXTRA,
    TB_INFLATE_DISTANCE,
    TB_INFLATE_DISTANCE_EXTRA,
    TB_INFLATE_COPY,
    TB_INFLATE_TRAILER,
    TB_INFLATE_DONE
} tb_inflate_state;

struct tb_inflater
{
    tb_allocator allocator;
    tb_deflate_format format;
    tb_inflate_state state;
    tb_deflate_result failure;

    /* Bits pulled from the input and not yet used, the next one lowest. */
    uint64_t bits;
    unsigned bit_count;

    /* The last 32 KiB of output, which matches copy from. */
    unsigned char window[TB_INFLATE_WINDOW_SIZE];
    size_t window_position;
    uint64_t total_out;
    uint32_t checksum;

    bool final_block;
    size_t stored_remaining;

    /* A dynamic block's header. */
    unsigned literal_count;
    unsigned distance_count;
    unsigned code_length_count;
    unsigned lengths_have;
    unsigned pending_symbol; /* A repeat symbol whose extra bits have not arrived; 0 when none. */
    unsigned char lengths[TB_LITERAL_SYMBOLS + TB_DISTANCE_SYMBOLS];

    /* The match being decoded or copied. */
    unsigned length_symbol;
    unsigned distance_symbol;
    unsigned match_length;
    unsigned match_distance;

    /* gzip header and trailers. */
    unsigned char header[TB_GZIP_FIXED_HEADER];
    size_t header_have;
    uint32_t header_crc;
    unsigned gzip_flags;
    size_t extra_remaining;
    unsigned char trailer[8];
    size_t trailer_have;

    const tb_huffman *literal_code;
    const tb_huffman *distance_code;
    tb_huffman fixed_literal;
    tb_huffman fixed_distance;
    tb_huffman dynamic_literal;
    tb_huffman dynamic_distance;
    tb_huffman code_length;
};

/* What one call has to work with, and how far it has got. */
typedef struct tb_inflate_io
{
    const unsigned char *input;
    size_t input_length;
    size_t input_used;
    unsigned char *output;
    size_t output_capacity;
    size_t output_written;
    size_t output_checksummed;
} tb_inflate_io;

/* -------------------------------------------------------------------------
 * Bits.
 * ------------------------------------------------------------------------- */

static bool tb_inflate_pull(tb_inflater *inflater, tb_inflate_io *io)
{
    if (io->input_used == io->input_length)
    {
        return false;
    }
    inflater->bits |= (uint64_t)io->input[io->input_used] << inflater->bit_count;
    inflater->bit_count += 8;
    io->input_used++;
    return true;
}

/* Pull bytes until `count` bits are held; false when the input runs out first. */
static bool tb_inflate_need(tb_inflater *inflater, tb_inflate_io *io, unsigned count)
{
    while (inflater->bit_count < count)
    {
        if (!tb_inflate_pull(inflater, io))
        {
            return false;
        }
    }
    return true;
}

static unsigned tb_inflate_take(tb_inflater *inflater, unsigned count)
{
    const unsigned value = (unsigned)(inflater->bits & ((UINT64_C(1) << count) - 1));
    TB_ASSERT(count <= inflater->bit_count);
    inflater->bits >>= count;
    inflater->bit_count -= count;
    return value;
}

/* Drop the bits left in the current byte. */
static void tb_inflate_align(tb_inflater *inflater)
{
    (void)tb_inflate_take(inflater, inflater->bit_count & 7u);
}

/* -------------------------------------------------------------------------
 * Huffman codes.
 * ------------------------------------------------------------------------- */

static unsigned tb_reverse_bits(unsigned code, unsigned length)
{
    unsigned reversed = 0;
    for (unsigned bit = 0; bit < length; bit++)
    {
        reversed = (reversed << 1) | ((code >> bit) & 1u);
    }
    return reversed;
}

/* Build a code from the length of each symbol's code, zero for unused. */
static tb_huffman_shape tb_huffman_build(tb_huffman *code, const unsigned char *lengths,
                                         size_t symbols)
{
    uint16_t offsets[TB_HUFFMAN_MAX_BITS + 2] = {0};
    unsigned next_code[TB_HUFFMAN_MAX_BITS + 1] = {0};
    int left = 1;
    unsigned used = 0;
    unsigned longest = 0;
    TB_ASSERT(symbols <= TB_LITERAL_SYMBOLS);
    memset(code, 0, sizeof(*code));
    for (size_t symbol = 0; symbol < symbols; symbol++)
    {
        TB_ASSERT(lengths[symbol] <= TB_HUFFMAN_MAX_BITS);
        code->count[lengths[symbol]]++;
    }
    code->count[0] = 0;
    for (unsigned length = 1; length <= TB_HUFFMAN_MAX_BITS; length++)
    {
        left <<= 1;
        left -= code->count[length];
        if (left < 0)
        {
            return TB_HUFFMAN_INVALID;
        }
        used += code->count[length];
        if (code->count[length] > 0)
        {
            longest = length;
        }
    }
    /* Symbols in canonical order: by length, then by symbol. */
    for (unsigned length = 1; length <= TB_HUFFMAN_MAX_BITS; length++)
    {
        offsets[length + 1] = (uint16_t)(offsets[length] + code->count[length]);
    }
    for (size_t symbol = 0; symbol < symbols; symbol++)
    {
        if (lengths[symbol] != 0)
        {
            code->symbol[offsets[lengths[symbol]]] = (uint16_t)symbol;
            offsets[lengths[symbol]]++;
        }
    }
    /* The first code of each length, then the fast table of the short codes. */
    {
        unsigned value = 0;
        for (unsigned length = 1; length <= TB_HUFFMAN_MAX_BITS; length++)
        {
            value = (value + code->count[length - 1]) << 1;
            next_code[length] = value;
        }
    }
    for (size_t symbol = 0; symbol < symbols; symbol++)
    {
        const unsigned length = lengths[symbol];
        if (length == 0)
        {
            continue;
        }
        if (length <= TB_HUFFMAN_FAST_BITS)
        {
            const unsigned reversed = tb_reverse_bits(next_code[length], length);
            const uint16_t entry = (uint16_t)((length << TB_HUFFMAN_FAST_BITS) | (unsigned)symbol);
            for (size_t index = reversed; index < TB_HUFFMAN_FAST_SIZE;
                 index += (size_t)1 << length)
            {
                code->fast[index] = entry;
            }
        }
        next_code[length]++;
    }
    code->longest = longest;
    if (left == 0)
    {
        return TB_HUFFMAN_COMPLETE;
    }
    if (used == 0)
    {
        return TB_HUFFMAN_EMPTY;
    }
    if (used == 1 && longest == 1)
    {
        return TB_HUFFMAN_SINGLE;
    }
    return TB_HUFFMAN_INVALID;
}

typedef enum tb_decode_outcome
{
    TB_DECODE_FOUND,
    TB_DECODE_NEED_INPUT,
    TB_DECODE_INVALID
} tb_decode_outcome;

/* Decode a bit at a time from the counts, using only the bits that are held. */
static tb_decode_outcome tb_huffman_decode_slowly(const tb_huffman *code, uint64_t bits,
                                                  unsigned available, unsigned *symbol,
                                                  unsigned *length)
{
    int value = 0;
    int first = 0;
    int index = 0;
    for (unsigned bit_length = 1; bit_length <= code->longest; bit_length++)
    {
        int count = 0;
        if (bit_length > available)
        {
            return TB_DECODE_NEED_INPUT;
        }
        value |= (int)((bits >> (bit_length - 1)) & 1u);
        count = code->count[bit_length];
        if (value - count < first)
        {
            *symbol = code->symbol[index + (value - first)];
            *length = bit_length;
            return TB_DECODE_FOUND;
        }
        index += count;
        first += count;
        first <<= 1;
        value <<= 1;
    }
    return TB_DECODE_INVALID;
}

/* Find the next symbol and its length without consuming it, pulling input as
 * the code needs. A short bit buffer is looked up as if padded with zeros:
 * when the code is no longer than the bits held, the padding cannot change it. */
static tb_decode_outcome tb_huffman_peek(tb_inflater *inflater, tb_inflate_io *io,
                                         const tb_huffman *code, unsigned *symbol, unsigned *length)
{
    for (;;)
    {
        const unsigned entry = code->fast[inflater->bits & TB_HUFFMAN_FAST_MASK];
        if (entry != 0)
        {
            const unsigned entry_length = entry >> TB_HUFFMAN_FAST_BITS;
            if (entry_length <= inflater->bit_count)
            {
                *symbol = entry & TB_HUFFMAN_SYMBOL_MASK;
                *length = entry_length;
                return TB_DECODE_FOUND;
            }
        }
        else
        {
            const tb_decode_outcome outcome =
                tb_huffman_decode_slowly(code, inflater->bits, inflater->bit_count, symbol, length);
            if (outcome != TB_DECODE_NEED_INPUT)
            {
                return outcome;
            }
        }
        if (!tb_inflate_pull(inflater, io))
        {
            return TB_DECODE_NEED_INPUT;
        }
    }
}

/* -------------------------------------------------------------------------
 * Output.
 * ------------------------------------------------------------------------- */

static bool tb_inflate_has_room(const tb_inflate_io *io)
{
    return io->output_written < io->output_capacity;
}

static void tb_inflate_emit(tb_inflater *inflater, tb_inflate_io *io, unsigned char byte)
{
    io->output[io->output_written] = byte;
    io->output_written++;
    inflater->window[inflater->window_position] = byte;
    inflater->window_position = (inflater->window_position + 1) & TB_INFLATE_WINDOW_MASK;
    inflater->total_out++;
}

/* Preserve the newest window of a contiguous output run. The write
 * position is relative to the stream even when the run wraps several times. */
static void tb_inflate_remember(tb_inflater *inflater, const unsigned char *bytes, size_t length)
{
    if (length > TB_INFLATE_WINDOW_SIZE)
    {
        const size_t skip = length - TB_INFLATE_WINDOW_SIZE;
        bytes += skip;
        inflater->window_position =
            (inflater->window_position + (skip & TB_INFLATE_WINDOW_MASK)) & TB_INFLATE_WINDOW_MASK;
        length = TB_INFLATE_WINDOW_SIZE;
    }
    size_t first = TB_INFLATE_WINDOW_SIZE - inflater->window_position;
    if (first > length)
    {
        first = length;
    }
    memcpy(inflater->window + inflater->window_position, bytes, first);
    memcpy(inflater->window, bytes + first, length - first);
    inflater->window_position = (inflater->window_position + length) & TB_INFLATE_WINDOW_MASK;
}

/* Copy bytes of a stored block straight from the input. */
static void tb_inflate_emit_run(tb_inflater *inflater, tb_inflate_io *io,
                                const unsigned char *bytes, size_t length)
{
    memcpy(io->output + io->output_written, bytes, length);
    io->output_written += length;
    inflater->total_out += length;
    tb_inflate_remember(inflater, bytes, length);
}

/* Fold the output written since the last time into the running checksum. */
static void tb_inflate_settle_checksum(tb_inflater *inflater, tb_inflate_io *io)
{
    const unsigned char *const start = io->output + io->output_checksummed;
    const size_t length = io->output_written - io->output_checksummed;
    if (length == 0)
    {
        return;
    }
    if (inflater->format == TB_DEFLATE_ZLIB)
    {
        inflater->checksum = tb_adler32_update(inflater->checksum, start, length);
    }
    else if (inflater->format == TB_DEFLATE_GZIP)
    {
        inflater->checksum = tb_crc32_update(inflater->checksum, start, length);
    }
    io->output_checksummed = io->output_written;
}

/* -------------------------------------------------------------------------
 * The states. Each returns true to go on, false to stop: for input, for
 * room, at the end, or on a failure it has recorded.
 * ------------------------------------------------------------------------- */

static bool tb_inflate_fail(tb_inflater *inflater, tb_deflate_result failure)
{
    inflater->failure = failure;
    return false;
}

static bool tb_inflate_zlib_header(tb_inflater *inflater, tb_inflate_io *io)
{
    unsigned method_and_info = 0;
    unsigned flags = 0;
    if (!tb_inflate_need(inflater, io, 16))
    {
        return false;
    }
    method_and_info = tb_inflate_take(inflater, 8);
    flags = tb_inflate_take(inflater, 8);
    if ((method_and_info & 0x0Fu) != TB_ZLIB_METHOD_DEFLATE ||
        (method_and_info >> 4) > TB_ZLIB_MAX_WINDOW_INFO ||
        ((method_and_info << 8) | flags) % TB_ZLIB_CHECK_DIVISOR != 0)
    {
        return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
    }
    if ((flags & TB_ZLIB_FLAG_DICTIONARY) != 0)
    {
        return tb_inflate_fail(inflater, TB_DEFLATE_UNSUPPORTED);
    }
    inflater->state = TB_INFLATE_BLOCK_HEADER;
    return true;
}

/* One byte of a gzip header, folded into the header's CRC. */
static bool tb_inflate_header_byte(tb_inflater *inflater, tb_inflate_io *io, unsigned char *byte)
{
    if (!tb_inflate_need(inflater, io, 8))
    {
        return false;
    }
    *byte = (unsigned char)tb_inflate_take(inflater, 8);
    inflater->header_crc = tb_crc32_update(inflater->header_crc, byte, 1);
    return true;
}

/* The optional parts of a gzip header come in a fixed order; skip those absent. */
static void tb_inflate_next_gzip_part(tb_inflater *inflater, tb_inflate_state after)
{
    tb_inflate_state next = after;
    if (next == TB_INFLATE_GZIP_EXTRA_LENGTH && (inflater->gzip_flags & TB_GZIP_FLAG_EXTRA) == 0)
    {
        next = TB_INFLATE_GZIP_NAME;
    }
    if (next == TB_INFLATE_GZIP_NAME && (inflater->gzip_flags & TB_GZIP_FLAG_NAME) == 0)
    {
        next = TB_INFLATE_GZIP_COMMENT;
    }
    if (next == TB_INFLATE_GZIP_COMMENT && (inflater->gzip_flags & TB_GZIP_FLAG_COMMENT) == 0)
    {
        next = TB_INFLATE_GZIP_HEADER_CRC;
    }
    if (next == TB_INFLATE_GZIP_HEADER_CRC && (inflater->gzip_flags & TB_GZIP_FLAG_HEADER_CRC) == 0)
    {
        next = TB_INFLATE_BLOCK_HEADER;
    }
    inflater->header_have = 0;
    inflater->state = next;
}

static bool tb_inflate_gzip_header(tb_inflater *inflater, tb_inflate_io *io)
{
    while (inflater->header_have < TB_GZIP_FIXED_HEADER)
    {
        if (!tb_inflate_header_byte(inflater, io, &inflater->header[inflater->header_have]))
        {
            return false;
        }
        inflater->header_have++;
    }
    if (inflater->header[0] != TB_GZIP_ID1 || inflater->header[1] != TB_GZIP_ID2 ||
        inflater->header[2] != TB_GZIP_METHOD_DEFLATE ||
        (inflater->header[3] & TB_GZIP_FLAG_RESERVED) != 0)
    {
        return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
    }
    inflater->gzip_flags = inflater->header[3];
    tb_inflate_next_gzip_part(inflater, TB_INFLATE_GZIP_EXTRA_LENGTH);
    return true;
}

static bool tb_inflate_gzip_extra_length(tb_inflater *inflater, tb_inflate_io *io)
{
    while (inflater->header_have < 2)
    {
        if (!tb_inflate_header_byte(inflater, io, &inflater->header[inflater->header_have]))
        {
            return false;
        }
        inflater->header_have++;
    }
    inflater->extra_remaining = (size_t)inflater->header[0] | ((size_t)inflater->header[1] << 8);
    inflater->state = TB_INFLATE_GZIP_EXTRA;
    return true;
}

static bool tb_inflate_gzip_extra(tb_inflater *inflater, tb_inflate_io *io)
{
    while (inflater->extra_remaining > 0)
    {
        unsigned char byte = 0;
        if (!tb_inflate_header_byte(inflater, io, &byte))
        {
            return false;
        }
        inflater->extra_remaining--;
    }
    tb_inflate_next_gzip_part(inflater, TB_INFLATE_GZIP_NAME);
    return true;
}

/* A file name or a comment: bytes up to a zero, of any length, kept nowhere. */
static bool tb_inflate_gzip_text(tb_inflater *inflater, tb_inflate_io *io, tb_inflate_state after)
{
    for (;;)
    {
        unsigned char byte = 0;
        if (!tb_inflate_header_byte(inflater, io, &byte))
        {
            return false;
        }
        if (byte == 0)
        {
            tb_inflate_next_gzip_part(inflater, after);
            return true;
        }
    }
}

static bool tb_inflate_gzip_header_crc(tb_inflater *inflater, tb_inflate_io *io)
{
    unsigned stored = 0;
    if (!tb_inflate_need(inflater, io, 16))
    {
        return false;
    }
    stored = tb_inflate_take(inflater, 16);
    if (stored != (inflater->header_crc & 0xFFFFu))
    {
        return tb_inflate_fail(inflater, TB_DEFLATE_CHECKSUM);
    }
    inflater->state = TB_INFLATE_BLOCK_HEADER;
    return true;
}

static bool tb_inflate_block_header(tb_inflater *inflater, tb_inflate_io *io)
{
    unsigned type = 0;
    if (!tb_inflate_need(inflater, io, 3))
    {
        return false;
    }
    inflater->final_block = tb_inflate_take(inflater, 1) != 0;
    type = tb_inflate_take(inflater, 2);
    switch (type)
    {
    case 0:
        tb_inflate_align(inflater);
        inflater->state = TB_INFLATE_STORED_LENGTHS;
        return true;
    case 1:
        inflater->literal_code = &inflater->fixed_literal;
        inflater->distance_code = &inflater->fixed_distance;
        inflater->state = TB_INFLATE_LITERAL;
        return true;
    case 2:
        inflater->state = TB_INFLATE_TABLE_COUNTS;
        return true;
    default:
        return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
    }
}

static void tb_inflate_end_block(tb_inflater *inflater)
{
    if (!inflater->final_block)
    {
        inflater->state = TB_INFLATE_BLOCK_HEADER;
        return;
    }
    tb_inflate_align(inflater);
    inflater->trailer_have = 0;
    if (inflater->format == TB_DEFLATE_RAW)
    {
        inflater->state = TB_INFLATE_DONE;
    }
    else
    {
        inflater->state = TB_INFLATE_TRAILER;
    }
}

static bool tb_inflate_stored_lengths(tb_inflater *inflater, tb_inflate_io *io)
{
    unsigned length = 0;
    unsigned complement = 0;
    if (!tb_inflate_need(inflater, io, 32))
    {
        return false;
    }
    length = tb_inflate_take(inflater, 16);
    complement = tb_inflate_take(inflater, 16);
    if (length != (~complement & 0xFFFFu))
    {
        return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
    }
    inflater->stored_remaining = length;
    inflater->state = TB_INFLATE_STORED_COPY;
    return true;
}

static bool tb_inflate_stored_copy(tb_inflater *inflater, tb_inflate_io *io)
{
    while (inflater->stored_remaining > 0)
    {
        size_t piece = 0;
        if (!tb_inflate_has_room(io))
        {
            return false;
        }
        /* The bit buffer is byte-aligned here; its bytes come first. */
        if (inflater->bit_count >= 8)
        {
            tb_inflate_emit(inflater, io, (unsigned char)tb_inflate_take(inflater, 8));
            inflater->stored_remaining--;
            continue;
        }
        piece = inflater->stored_remaining;
        if (piece > io->input_length - io->input_used)
        {
            piece = io->input_length - io->input_used;
        }
        if (piece > io->output_capacity - io->output_written)
        {
            piece = io->output_capacity - io->output_written;
        }
        if (piece == 0)
        {
            return false;
        }
        tb_inflate_emit_run(inflater, io, io->input + io->input_used, piece);
        io->input_used += piece;
        inflater->stored_remaining -= piece;
    }
    tb_inflate_end_block(inflater);
    return true;
}

static bool tb_inflate_table_counts(tb_inflater *inflater, tb_inflate_io *io)
{
    if (!tb_inflate_need(inflater, io, 14))
    {
        return false;
    }
    inflater->literal_count = tb_inflate_take(inflater, 5) + TB_FIRST_LENGTH_SYMBOL;
    inflater->distance_count = tb_inflate_take(inflater, 5) + 1;
    inflater->code_length_count = tb_inflate_take(inflater, 4) + 4;
    if (inflater->literal_count > TB_MAX_LITERAL_COUNT ||
        inflater->distance_count > TB_USED_DISTANCE_SYMBOLS)
    {
        return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
    }
    memset(inflater->lengths, 0, sizeof(inflater->lengths));
    inflater->lengths_have = 0;
    inflater->state = TB_INFLATE_CODE_LENGTH_LENGTHS;
    return true;
}

static bool tb_inflate_code_length_lengths(tb_inflater *inflater, tb_inflate_io *io)
{
    while (inflater->lengths_have < inflater->code_length_count)
    {
        if (!tb_inflate_need(inflater, io, 3))
        {
            return false;
        }
        inflater->lengths[tb_code_length_order[inflater->lengths_have]] =
            (unsigned char)tb_inflate_take(inflater, 3);
        inflater->lengths_have++;
    }
    /* The code-length code must be complete. */
    if (tb_huffman_build(&inflater->code_length, inflater->lengths, TB_CODE_LENGTH_SYMBOLS) !=
        TB_HUFFMAN_COMPLETE)
    {
        return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
    }
    memset(inflater->lengths, 0, sizeof(inflater->lengths));
    inflater->lengths_have = 0;
    inflater->pending_symbol = 0;
    inflater->state = TB_INFLATE_CODE_LENGTHS;
    return true;
}

/* Apply a repeat symbol of the code lengths, once its extra bits are held. */
static bool tb_inflate_repeat_lengths(tb_inflater *inflater, tb_inflate_io *io, unsigned total)
{
    unsigned value = 0;
    unsigned repeat = 0;
    switch (inflater->pending_symbol)
    {
    case 16:
        if (!tb_inflate_need(inflater, io, 2))
        {
            return false;
        }
        if (inflater->lengths_have == 0)
        {
            return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
        }
        value = inflater->lengths[inflater->lengths_have - 1];
        repeat = 3 + tb_inflate_take(inflater, 2);
        break;
    case 17:
        if (!tb_inflate_need(inflater, io, 3))
        {
            return false;
        }
        repeat = 3 + tb_inflate_take(inflater, 3);
        break;
    default:
        TB_ASSERT(inflater->pending_symbol == 18);
        if (!tb_inflate_need(inflater, io, 7))
        {
            return false;
        }
        repeat = 11 + tb_inflate_take(inflater, 7);
        break;
    }
    if (repeat > total - inflater->lengths_have)
    {
        return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
    }
    memset(inflater->lengths + inflater->lengths_have, (int)value, repeat);
    inflater->lengths_have += repeat;
    inflater->pending_symbol = 0;
    return true;
}

static bool tb_inflate_build_block_codes(tb_inflater *inflater)
{
    tb_huffman_shape shape = TB_HUFFMAN_INVALID;
    /* A block must be able to end. */
    if (inflater->lengths[TB_END_OF_BLOCK] == 0)
    {
        return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
    }
    shape =
        tb_huffman_build(&inflater->dynamic_literal, inflater->lengths, inflater->literal_count);
    if (shape == TB_HUFFMAN_INVALID)
    {
        return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
    }
    shape = tb_huffman_build(&inflater->dynamic_distance,
                             inflater->lengths + inflater->literal_count, inflater->distance_count);
    if (shape == TB_HUFFMAN_INVALID)
    {
        return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
    }
    inflater->literal_code = &inflater->dynamic_literal;
    inflater->distance_code = &inflater->dynamic_distance;
    inflater->state = TB_INFLATE_LITERAL;
    return true;
}

static bool tb_inflate_code_lengths(tb_inflater *inflater, tb_inflate_io *io)
{
    const unsigned total = inflater->literal_count + inflater->distance_count;
    while (inflater->lengths_have < total)
    {
        unsigned symbol = 0;
        unsigned length = 0;
        tb_decode_outcome outcome = TB_DECODE_INVALID;
        if (inflater->pending_symbol != 0)
        {
            if (!tb_inflate_repeat_lengths(inflater, io, total))
            {
                return false;
            }
            continue;
        }
        outcome = tb_huffman_peek(inflater, io, &inflater->code_length, &symbol, &length);
        if (outcome == TB_DECODE_NEED_INPUT)
        {
            return false;
        }
        if (outcome == TB_DECODE_INVALID)
        {
            return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
        }
        (void)tb_inflate_take(inflater, length);
        if (symbol < 16)
        {
            inflater->lengths[inflater->lengths_have] = (unsigned char)symbol;
            inflater->lengths_have++;
        }
        else
        {
            inflater->pending_symbol = symbol;
        }
    }
    return tb_inflate_build_block_codes(inflater);
}

/* -------------------------------------------------------------------------
 * The fast loop: literals and matches while the input and the room are
 * plentiful.
 *
 * With at least TB_INFLATE_FAST_INPUT bytes of input and TB_INFLATE_FAST_ROOM
 * of room, a whole symbol, its match and their extra bits can be decoded
 * without stopping. A word fetch supplies at least TB_INFLATE_FAST_BITS.
 * The loop writes literals and copies matches
 * in the output itself, and brings the window up to date once, when it
 * stops. It gives back the whole bytes it pulled and did not use, so the
 * input is consumed exactly, as everywhere else.
 * ------------------------------------------------------------------------- */

/* The most bits one symbol of literal or length and its match need: a code
 * of 15 bits, 5 extra, a distance code of 15 and 13 extra. */
#define TB_INFLATE_FAST_BITS 48u
#define TB_INFLATE_FAST_INPUT ((size_t)8)
#define TB_INFLATE_FAST_ROOM ((size_t)258)

static bool tb_inflate_fast_ready(const tb_inflate_io *io)
{
    return io->input_length - io->input_used >= TB_INFLATE_FAST_INPUT &&
           io->output_capacity - io->output_written >= TB_INFLATE_FAST_ROOM;
}

/* A symbol of a code; the bit buffer holds enough for any. */
static bool tb_inflate_fast_symbol(tb_inflater *inflater, const tb_huffman *code, unsigned *symbol)
{
    const unsigned entry = code->fast[inflater->bits & TB_HUFFMAN_FAST_MASK];
    unsigned length = 0;

    if (entry != 0)
    {
        *symbol = entry & TB_HUFFMAN_SYMBOL_MASK;
        length = entry >> TB_HUFFMAN_FAST_BITS;
    }
    else if (tb_huffman_decode_slowly(code, inflater->bits, inflater->bit_count, symbol, &length) !=
             TB_DECODE_FOUND)
    {
        return false;
    }
    (void)tb_inflate_take(inflater, length);
    return true;
}

/* Copy a match. What is in this call's output is copied from there; what
 * is older, from the window, which holds the stream up to `fast_start`. */
static void tb_inflate_fast_copy(const tb_inflater *inflater, tb_inflate_io *io, size_t fast_start,
                                 size_t distance, size_t length)
{
    unsigned char *const output = io->output;
    size_t written = io->output_written;

    if (distance <= written && distance >= length)
    {
        memcpy(output + written, output + written - distance, length);
        io->output_written = written + length;
        return;
    }
    for (size_t index = 0; index < length; index++)
    {
        if (distance <= written)
        {
            output[written] = output[written - distance];
        }
        else
        {
            /* Before this call's output: `distance - written` bytes back
             * from its start, which the window's position marks once
             * the bytes since `fast_start` are counted. */
            const size_t back = distance - written + fast_start;
            output[written] =
                inflater->window[(inflater->window_position - back) & TB_INFLATE_WINDOW_MASK];
        }
        written++;
    }
    io->output_written = written;
}

static uint32_t tb_inflate_load32_little(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/* Decode symbols of a block while the input and room allow. Returns false
 * on a failure it has recorded; otherwise true, in the literal state still
 * or at the block's end. */
static bool tb_inflate_fast(tb_inflater *inflater, tb_inflate_io *io)
{
    const size_t fast_start = io->output_written;
    const size_t input_start = io->input_used;
    bool ok = true;

    while (tb_inflate_fast_ready(io))
    {
        if (inflater->bit_count < TB_INFLATE_FAST_BITS)
        {
            /* At least eight bytes are readable. Keep only whole bytes
             * that fit below bit 63, so the shift and mask are defined. */
            const unsigned char *const bytes = io->input + io->input_used;
            const uint64_t word = (uint64_t)tb_inflate_load32_little(bytes) |
                                  (uint64_t)tb_inflate_load32_little(bytes + 4u) << 32;
            const unsigned take = (63u - inflater->bit_count) / 8u;
            const unsigned bits = take * 8u;
            const uint64_t mask = (UINT64_C(1) << bits) - 1u;
            inflater->bits |= (word & mask) << inflater->bit_count;
            inflater->bit_count += bits;
            io->input_used += take;
        }
        unsigned symbol = 0;
        if (!tb_inflate_fast_symbol(inflater, inflater->literal_code, &symbol))
        {
            ok = tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
            break;
        }
        if (symbol < TB_END_OF_BLOCK)
        {
            io->output[io->output_written] = (unsigned char)symbol;
            io->output_written++;
            inflater->total_out++;
            continue;
        }
        if (symbol == TB_END_OF_BLOCK)
        {
            tb_inflate_end_block(inflater);
            break;
        }
        const unsigned length_symbol = symbol - TB_FIRST_LENGTH_SYMBOL;
        if (length_symbol >= TB_LENGTH_SYMBOLS)
        {
            ok = tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
            break;
        }
        const size_t length = tb_length_base[length_symbol] +
                              tb_inflate_take(inflater, tb_length_extra[length_symbol]);
        unsigned distance_symbol = 0;
        if (!tb_inflate_fast_symbol(inflater, inflater->distance_code, &distance_symbol) ||
            distance_symbol >= TB_USED_DISTANCE_SYMBOLS)
        {
            ok = tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
            break;
        }
        const size_t distance = tb_distance_base[distance_symbol] +
                                tb_inflate_take(inflater, tb_distance_extra[distance_symbol]);
        /* A match may reach back only over output that exists. */
        if ((uint64_t)distance > inflater->total_out)
        {
            ok = tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
            break;
        }
        tb_inflate_fast_copy(inflater, io, fast_start, distance, length);
        inflater->total_out += length;
    }
    /* Give back the whole bytes pulled here and not used. */
    while (inflater->bit_count >= 8u && io->input_used > input_start)
    {
        inflater->bit_count -= 8u;
        inflater->bits &= (UINT64_C(1) << inflater->bit_count) - 1u;
        io->input_used--;
    }
    tb_inflate_remember(inflater, io->output + fast_start, io->output_written - fast_start);
    return ok;
}

static bool tb_inflate_literal(tb_inflater *inflater, tb_inflate_io *io)
{
    if (tb_inflate_fast_ready(io))
    {
        return tb_inflate_fast(inflater, io);
    }
    for (;;)
    {
        unsigned symbol = 0;
        unsigned length = 0;
        const tb_decode_outcome outcome =
            tb_huffman_peek(inflater, io, inflater->literal_code, &symbol, &length);
        if (outcome == TB_DECODE_NEED_INPUT)
        {
            return false;
        }
        if (outcome == TB_DECODE_INVALID)
        {
            return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
        }
        if (symbol < TB_END_OF_BLOCK)
        {
            /* Leave a literal unconsumed until there is room for it. */
            if (!tb_inflate_has_room(io))
            {
                return false;
            }
            (void)tb_inflate_take(inflater, length);
            tb_inflate_emit(inflater, io, (unsigned char)symbol);
            continue;
        }
        (void)tb_inflate_take(inflater, length);
        if (symbol == TB_END_OF_BLOCK)
        {
            tb_inflate_end_block(inflater);
            return true;
        }
        if (symbol - TB_FIRST_LENGTH_SYMBOL >= TB_LENGTH_SYMBOLS)
        {
            return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
        }
        inflater->length_symbol = symbol - TB_FIRST_LENGTH_SYMBOL;
        inflater->state = TB_INFLATE_LENGTH_EXTRA;
        return true;
    }
}

static bool tb_inflate_length_extra(tb_inflater *inflater, tb_inflate_io *io)
{
    const unsigned extra = tb_length_extra[inflater->length_symbol];
    if (!tb_inflate_need(inflater, io, extra))
    {
        return false;
    }
    inflater->match_length =
        tb_length_base[inflater->length_symbol] + tb_inflate_take(inflater, extra);
    inflater->state = TB_INFLATE_DISTANCE;
    return true;
}

static bool tb_inflate_distance(tb_inflater *inflater, tb_inflate_io *io)
{
    unsigned symbol = 0;
    unsigned length = 0;
    const tb_decode_outcome outcome =
        tb_huffman_peek(inflater, io, inflater->distance_code, &symbol, &length);
    if (outcome == TB_DECODE_NEED_INPUT)
    {
        return false;
    }
    if (outcome == TB_DECODE_INVALID || symbol >= TB_USED_DISTANCE_SYMBOLS)
    {
        return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
    }
    (void)tb_inflate_take(inflater, length);
    inflater->distance_symbol = symbol;
    inflater->state = TB_INFLATE_DISTANCE_EXTRA;
    return true;
}

static bool tb_inflate_distance_extra(tb_inflater *inflater, tb_inflate_io *io)
{
    const unsigned extra = tb_distance_extra[inflater->distance_symbol];
    unsigned distance = 0;
    if (!tb_inflate_need(inflater, io, extra))
    {
        return false;
    }
    distance = tb_distance_base[inflater->distance_symbol] + tb_inflate_take(inflater, extra);
    /* A match may reach back only over output that exists. */
    if ((uint64_t)distance > inflater->total_out)
    {
        return tb_inflate_fail(inflater, TB_DEFLATE_MALFORMED);
    }
    inflater->match_distance = distance;
    inflater->state = TB_INFLATE_COPY;
    return true;
}

static bool tb_inflate_copy(tb_inflater *inflater, tb_inflate_io *io)
{
    while (inflater->match_length > 0)
    {
        size_t source = 0;
        if (!tb_inflate_has_room(io))
        {
            return false;
        }
        source = (inflater->window_position - inflater->match_distance) & TB_INFLATE_WINDOW_MASK;
        tb_inflate_emit(inflater, io, inflater->window[source]);
        inflater->match_length--;
    }
    inflater->state = TB_INFLATE_LITERAL;
    return true;
}

static uint32_t tb_inflate_load32_big(const unsigned char *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static bool tb_inflate_trailer(tb_inflater *inflater, tb_inflate_io *io)
{
    const size_t size = inflater->format == TB_DEFLATE_GZIP ? 8 : 4;
    tb_inflate_settle_checksum(inflater, io);
    while (inflater->trailer_have < size)
    {
        if (!tb_inflate_need(inflater, io, 8))
        {
            return false;
        }
        inflater->trailer[inflater->trailer_have] = (unsigned char)tb_inflate_take(inflater, 8);
        inflater->trailer_have++;
    }
    if (inflater->format == TB_DEFLATE_ZLIB)
    {
        if (tb_inflate_load32_big(inflater->trailer) != inflater->checksum)
        {
            return tb_inflate_fail(inflater, TB_DEFLATE_CHECKSUM);
        }
    }
    else
    {
        /* gzip records the length modulo 2^32. */
        if (tb_inflate_load32_little(inflater->trailer) != inflater->checksum ||
            tb_inflate_load32_little(inflater->trailer + 4) != (uint32_t)inflater->total_out)
        {
            return tb_inflate_fail(inflater, TB_DEFLATE_CHECKSUM);
        }
    }
    inflater->state = TB_INFLATE_DONE;
    return true;
}

static bool tb_inflate_step(tb_inflater *inflater, tb_inflate_io *io)
{
    switch (inflater->state)
    {
    case TB_INFLATE_ZLIB_HEADER:
        return tb_inflate_zlib_header(inflater, io);
    case TB_INFLATE_GZIP_HEADER:
        return tb_inflate_gzip_header(inflater, io);
    case TB_INFLATE_GZIP_EXTRA_LENGTH:
        return tb_inflate_gzip_extra_length(inflater, io);
    case TB_INFLATE_GZIP_EXTRA:
        return tb_inflate_gzip_extra(inflater, io);
    case TB_INFLATE_GZIP_NAME:
        return tb_inflate_gzip_text(inflater, io, TB_INFLATE_GZIP_COMMENT);
    case TB_INFLATE_GZIP_COMMENT:
        return tb_inflate_gzip_text(inflater, io, TB_INFLATE_GZIP_HEADER_CRC);
    case TB_INFLATE_GZIP_HEADER_CRC:
        return tb_inflate_gzip_header_crc(inflater, io);
    case TB_INFLATE_BLOCK_HEADER:
        return tb_inflate_block_header(inflater, io);
    case TB_INFLATE_STORED_LENGTHS:
        return tb_inflate_stored_lengths(inflater, io);
    case TB_INFLATE_STORED_COPY:
        return tb_inflate_stored_copy(inflater, io);
    case TB_INFLATE_TABLE_COUNTS:
        return tb_inflate_table_counts(inflater, io);
    case TB_INFLATE_CODE_LENGTH_LENGTHS:
        return tb_inflate_code_length_lengths(inflater, io);
    case TB_INFLATE_CODE_LENGTHS:
        return tb_inflate_code_lengths(inflater, io);
    case TB_INFLATE_LITERAL:
        return tb_inflate_literal(inflater, io);
    case TB_INFLATE_LENGTH_EXTRA:
        return tb_inflate_length_extra(inflater, io);
    case TB_INFLATE_DISTANCE:
        return tb_inflate_distance(inflater, io);
    case TB_INFLATE_DISTANCE_EXTRA:
        return tb_inflate_distance_extra(inflater, io);
    case TB_INFLATE_COPY:
        return tb_inflate_copy(inflater, io);
    case TB_INFLATE_TRAILER:
        return tb_inflate_trailer(inflater, io);
    case TB_INFLATE_DONE:
        return false;
    default:
        /* No state: memory that is not an inflater's. */
        return tb_inflate_fail(inflater, TB_DEFLATE_INVALID);
    }
}

/* -------------------------------------------------------------------------
 * The interface.
 * ------------------------------------------------------------------------- */

static void tb_inflate_build_fixed_codes(tb_inflater *inflater)
{
    unsigned char lengths[TB_LITERAL_SYMBOLS] = {0};
    tb_huffman_shape shape = TB_HUFFMAN_INVALID;
    memset(lengths, 8, 144);
    memset(lengths + 144, 9, 112);
    memset(lengths + 256, 7, 24);
    memset(lengths + 280, 8, 8);
    shape = tb_huffman_build(&inflater->fixed_literal, lengths, TB_LITERAL_SYMBOLS);
    TB_ASSERT(shape == TB_HUFFMAN_COMPLETE);
    memset(lengths, 5, TB_DISTANCE_SYMBOLS);
    shape = tb_huffman_build(&inflater->fixed_distance, lengths, TB_DISTANCE_SYMBOLS);
    TB_ASSERT(shape == TB_HUFFMAN_COMPLETE);
    (void)shape;
}

void tb_inflater_reset(tb_inflater *inflater)
{
    TB_ASSERT(inflater != NULL);
    inflater->failure = TB_DEFLATE_OK;
    inflater->bits = 0;
    inflater->bit_count = 0;
    inflater->window_position = 0;
    inflater->total_out = 0;
    inflater->final_block = false;
    inflater->stored_remaining = 0;
    inflater->pending_symbol = 0;
    inflater->match_length = 0;
    inflater->header_have = 0;
    inflater->header_crc = TB_CRC32_INITIAL;
    inflater->gzip_flags = 0;
    inflater->extra_remaining = 0;
    inflater->trailer_have = 0;
    switch (inflater->format)
    {
    case TB_DEFLATE_ZLIB:
        inflater->checksum = TB_ADLER32_INITIAL;
        inflater->state = TB_INFLATE_ZLIB_HEADER;
        break;
    case TB_DEFLATE_GZIP:
        inflater->checksum = TB_CRC32_INITIAL;
        inflater->state = TB_INFLATE_GZIP_HEADER;
        break;
    case TB_DEFLATE_RAW:
    default:
        inflater->checksum = 0;
        inflater->state = TB_INFLATE_BLOCK_HEADER;
        break;
    }
}

tb_inflater *tb_inflater_create(tb_deflate_format format, const tb_allocator *allocator)
{
    tb_allocator resolved = {0};
    tb_inflater *inflater = NULL;
    TB_ASSERT_MSG(format == TB_DEFLATE_RAW || format == TB_DEFLATE_ZLIB ||
                      format == TB_DEFLATE_GZIP,
                  "unknown DEFLATE format");
    resolved = tb_allocator_resolve(allocator);
    inflater = TB_ALLOCATOR_NEW(&resolved, tb_inflater);
    inflater->allocator = resolved;
    inflater->format = format;
    tb_inflate_build_fixed_codes(inflater);
    tb_inflater_reset(inflater);
    return inflater;
}

void tb_inflater_destroy(tb_inflater *inflater)
{
    tb_allocator allocator = {0};
    if (inflater == NULL)
    {
        return;
    }
    allocator = inflater->allocator;
    TB_ALLOCATOR_DELETE(&allocator, tb_inflater, inflater);
}

bool tb_inflater_finished(const tb_inflater *inflater)
{
    TB_ASSERT(inflater != NULL);
    return inflater->state == TB_INFLATE_DONE && inflater->failure == TB_DEFLATE_OK;
}

tb_deflate_result tb_inflater_run(tb_inflater *inflater, const void *input, size_t input_length,
                                  size_t *input_used, void *output, size_t output_capacity,
                                  size_t *output_written)
{
    tb_inflate_io io = {0};
    TB_ASSERT(inflater != NULL);
    TB_ASSERT(input != NULL || input_length == 0);
    TB_ASSERT(output != NULL || output_capacity == 0);
    TB_ASSERT(input_used != NULL);
    TB_ASSERT(output_written != NULL);
    *input_used = 0;
    *output_written = 0;
    if (inflater->failure != TB_DEFLATE_OK)
    {
        return inflater->failure;
    }
    io.input = input;
    io.input_length = input_length;
    io.output = output;
    io.output_capacity = output_capacity;
    while (tb_inflate_step(inflater, &io))
    {
    }
    tb_inflate_settle_checksum(inflater, &io);
    *input_used = io.input_used;
    *output_written = io.output_written;
    return inflater->failure;
}

tb_deflate_result tb_inflate(tb_deflate_format format, const void *input, size_t input_length,
                             void *output, size_t capacity, size_t *written)
{
    tb_inflater *inflater = NULL;
    size_t used = 0;
    tb_deflate_result result = TB_DEFLATE_OK;
    TB_ASSERT(input != NULL || input_length == 0);
    TB_ASSERT(output != NULL || capacity == 0);
    TB_ASSERT(written != NULL);
    *written = 0;
    if (format != TB_DEFLATE_RAW && format != TB_DEFLATE_ZLIB && format != TB_DEFLATE_GZIP)
    {
        return TB_DEFLATE_INVALID;
    }
    inflater = tb_inflater_create(format, NULL);
    result = tb_inflater_run(inflater, input, input_length, &used, output, capacity, written);
    if (result != TB_DEFLATE_OK)
    {
        goto cleanup;
    }
    if (tb_inflater_finished(inflater))
    {
        if (used < input_length)
        {
            result = TB_DEFLATE_MALFORMED;
        }
        goto cleanup;
    }
    /* Stopped short: for room, or for input. */
    if (*written == capacity && used < input_length)
    {
        result = TB_DEFLATE_TOO_LARGE;
    }
    else if (*written == capacity)
    {
        /* Out of both at once: one more byte of room tells which ran short. */
        unsigned char probe = 0;
        size_t probe_used = 0;
        size_t probe_written = 0;
        result = tb_inflater_run(inflater, NULL, 0, &probe_used, &probe, 1, &probe_written);
        /* A byte written to the probe is a byte that did not fit. */
        if (result == TB_DEFLATE_OK)
        {
            result = probe_written > 0 ? TB_DEFLATE_TOO_LARGE : TB_DEFLATE_TRUNCATED;
        }
    }
    else
    {
        result = TB_DEFLATE_TRUNCATED;
    }
cleanup:
    tb_inflater_destroy(inflater);
    return result;
}

const char *tb_deflate_result_name(tb_deflate_result result)
{
    switch (result)
    {
    case TB_DEFLATE_OK:
        return "ok";
    case TB_DEFLATE_INVALID:
        return "invalid argument";
    case TB_DEFLATE_TOO_LARGE:
        return "too large";
    case TB_DEFLATE_MALFORMED:
        return "malformed data";
    case TB_DEFLATE_CHECKSUM:
        return "checksum mismatch";
    case TB_DEFLATE_TRUNCATED:
        return "unexpected end";
    case TB_DEFLATE_UNSUPPORTED:
        return "not supported";
    default:
        return "unknown result";
    }
}
