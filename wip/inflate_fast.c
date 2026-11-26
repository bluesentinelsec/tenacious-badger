/* -------------------------------------------------------------------------
 * The fast loop: literals and matches while the input and the room are
 * plentiful.
 *
 * With at least TB_INFLATE_FAST_INPUT bytes of input and TB_INFLATE_FAST_ROOM
 * of room, a whole symbol, its match and their extra bits can be decoded
 * without stopping, so the loop checks neither. It fills the bit buffer a
 * byte at a time to TB_INFLATE_FAST_BITS, writes literals and copies matches
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
static bool tb_inflate_fast_symbol(tb_inflater *inflater, const tb_huffman *code,
                                   unsigned *symbol)
{
    const unsigned entry = code->fast[inflater->bits & TB_HUFFMAN_FAST_MASK];
    unsigned length = 0;

    if (entry != 0)
    {
        *symbol = entry & TB_HUFFMAN_SYMBOL_MASK;
        length = entry >> TB_HUFFMAN_FAST_BITS;
    }
    else if (tb_huffman_decode_slowly(code, inflater->bits, inflater->bit_count, symbol,
                                      &length) != TB_DECODE_FOUND)
    {
        return false;
    }
    (void)tb_inflate_take(inflater, length);
    return true;
}

/* Copy a match. What is in this call's output is copied from there; what
 * is older, from the window, which holds the stream up to `fast_start`. */
static void tb_inflate_fast_copy(const tb_inflater *inflater, tb_inflate_io *io,
                                 size_t fast_start, size_t distance, size_t length)
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
            output[written] = inflater->window[(inflater->window_position - back) &
                                               TB_INFLATE_WINDOW_MASK];
        }
        written++;
    }
    io->output_written = written;
}

/* Put what the loop wrote, from `fast_start`, in the window: its last 32 KiB. */
static void tb_inflate_fast_window(tb_inflater *inflater, const tb_inflate_io *io,
                                   size_t fast_start)
{
    size_t length = io->output_written - fast_start;
    const unsigned char *bytes = io->output + fast_start;

    if (length > TB_INFLATE_WINDOW_SIZE)
    {
        bytes += length - TB_INFLATE_WINDOW_SIZE;
        length = TB_INFLATE_WINDOW_SIZE;
    }
    while (length > 0)
    {
        size_t piece = TB_INFLATE_WINDOW_SIZE - inflater->window_position;
        if (piece > length)
        {
            piece = length;
        }
        memcpy(inflater->window + inflater->window_position, bytes, piece);
        inflater->window_position = (inflater->window_position + piece) & TB_INFLATE_WINDOW_MASK;
        bytes += piece;
        length -= piece;
    }
}

/* Decode symbols of a block while the input and room allow. Returns false
 * on a failure it has recorded; otherwise true, in the literal state still
 * or at the block's end. */
static bool tb_inflate_fast(tb_inflater *inflater, tb_inflate_io *io)
{
    const size_t fast_start = io->output_written;
    size_t pulled = 0;
    bool ok = true;

    while (tb_inflate_fast_ready(io))
    {
        while (inflater->bit_count < TB_INFLATE_FAST_BITS)
        {
            inflater->bits |= (uint64_t)io->input[io->input_used] << inflater->bit_count;
            inflater->bit_count += 8;
            io->input_used++;
            pulled++;
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
    while (inflater->bit_count >= 8u && pulled > 0)
    {
        inflater->bit_count -= 8u;
        inflater->bits &= (UINT64_C(1) << inflater->bit_count) - 1u;
        io->input_used--;
        pulled--;
    }
    tb_inflate_fast_window(inflater, io, fast_start);
    return ok;
}
