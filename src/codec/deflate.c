/* DEFLATE compression (RFC 1951), with zlib (RFC 1950) and gzip (RFC 1952)
 * framing.
 *
 * Input is gathered into a window of 64 KiB, the last 32 KiB of which is
 * what matches may reach back over. A hash of the next three bytes chains
 * each position to earlier ones with the same hash; the matcher walks the
 * chain for the longest match, as far as the level allows. Levels 1 to 3
 * take the first good match ("greedy"); levels 4 to 9 look one byte ahead
 * for a better one ("lazy"), as zlib does, with zlib's table of effort.
 *
 * Matches and literals are collected into a block of at most 16383 symbols.
 * A block is written in whichever of the three forms is smallest: stored,
 * with the fixed code, or with codes built for it, limited to 15 bits.
 *
 * Every decision depends only on the data, never on how it was split into
 * calls: the matcher runs only with a full lookahead (or at a flush), the
 * window slides at a fixed position, and blocks end at fixed points. So the
 * same input gives the same bytes however it arrives. */
#include "badger/deflate.h"

#include "badger/assert.h"
#include "badger/checksum.h"

#include <stddef.h>
#include <string.h>

#define TB_DEFLATE_WINDOW ((size_t)32768)
#define TB_DEFLATE_BUFFER (2 * TB_DEFLATE_WINDOW)
#define TB_DEFLATE_MIN_MATCH 3u
#define TB_DEFLATE_MAX_MATCH 258u
/* Bytes the matcher wants ahead of it: a whole match, and the next hash. */
#define TB_DEFLATE_LOOKAHEAD ((size_t)(TB_DEFLATE_MAX_MATCH + TB_DEFLATE_MIN_MATCH + 1))
/* Matches reach no further back than this, so a slide never strands one. */
#define TB_DEFLATE_MAX_DISTANCE (TB_DEFLATE_WINDOW - TB_DEFLATE_LOOKAHEAD)
/* The window slides when the position reaches this. */
#define TB_DEFLATE_SLIDE_AT (TB_DEFLATE_BUFFER - TB_DEFLATE_LOOKAHEAD)
#define TB_DEFLATE_HASH_BITS 15u
#define TB_DEFLATE_HASH_SIZE ((size_t)1 << TB_DEFLATE_HASH_BITS)
/* A match of three bytes this far back costs more than three literals. */
#define TB_DEFLATE_TOO_FAR 4096u
/* The hash chains hold a position plus one, so that 0 can mean none and
 * every position of the 64 KiB window fits in 16 bits. */
#define TB_DEFLATE_NO_LINK 0u
#define TB_DEFLATE_SYMBOLS ((size_t)16383)
#define TB_DEFLATE_LITERALS 286u
#define TB_DEFLATE_DISTANCES 30u
#define TB_DEFLATE_CODE_LENGTHS 19u
#define TB_DEFLATE_END_OF_BLOCK 256u
#define TB_DEFLATE_MAX_BITS 15u
#define TB_DEFLATE_MAX_CODE_LENGTH_BITS 7u
#define TB_DEFLATE_STORED_MAX ((size_t)65535)
/* Room for the largest block in any form, its framing, and a trailer. The
 * form chosen is never larger than storing the block, which is at most the
 * window's 64 KiB in two stored blocks. */
#define TB_DEFLATE_PENDING ((size_t)TB_DEFLATE_BUFFER + 1024)
#define TB_DEFLATE_MAX_LEVEL 9

static const uint16_t tb_deflate_length_base[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                                    15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                                    67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t tb_deflate_length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t tb_deflate_distance_base[30] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t tb_deflate_distance_extra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                                      4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                                      9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t tb_deflate_code_length_order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                         11, 4,  12, 3, 13, 2, 14, 1, 15};

/* How hard each level looks for matches, as zlib's table has it: a match this
 * good shortens the search; a match this long is not bettered lazily; a match
 * this long ends the search; and the most chain links to follow. */
typedef struct tb_deflate_effort
{
    uint16_t good_length;
    uint16_t lazy_length;
    uint16_t nice_length;
    uint16_t max_chain;
    bool lazy;
} tb_deflate_effort;

static const tb_deflate_effort tb_deflate_efforts[TB_DEFLATE_MAX_LEVEL + 1] = {
    {0, 0, 0, 0, false},        {4, 4, 8, 4, false},     {4, 5, 16, 8, false},
    {4, 6, 32, 32, false},      {4, 4, 16, 16, true},    {8, 16, 32, 32, true},
    {8, 16, 128, 128, true},    {8, 32, 128, 256, true}, {32, 128, 258, 1024, true},
    {32, 258, 258, 4096, true},
};

typedef enum tb_deflate_phase
{
    TB_DEFLATE_PHASE_HEADER,
    TB_DEFLATE_PHASE_DATA,
    TB_DEFLATE_PHASE_TRAILER,
    TB_DEFLATE_PHASE_DONE
} tb_deflate_phase;

/* Room for all 288 fixed literal codes, though 286 and 287 are never written. */
#define TB_DEFLATE_FIXED_LITERALS ((size_t)288)

typedef struct tb_deflate_code
{
    uint8_t length[TB_DEFLATE_FIXED_LITERALS];
    uint16_t bits[TB_DEFLATE_FIXED_LITERALS]; /* Reversed, ready to write lowest bit first. */
} tb_deflate_code;

/* Distances up to this have a symbol each in the table; longer ones share a
 * symbol by 128s, as every distance code past 256 spans a multiple of 128. */
#define TB_DEFLATE_NEAR_DISTANCES ((size_t)256)
#define TB_DEFLATE_FAR_SHIFT 7u

/* One symbol of a block: a literal (distance 0) or a match. */
typedef struct tb_deflate_symbol
{
    uint16_t literal_or_length;
    uint16_t distance;
} tb_deflate_symbol;

struct tb_deflater
{
    tb_allocator allocator;
    tb_deflate_format format;
    int level;
    tb_deflate_effort effort;

    /* Made once, when the deflater is created: the fixed code, and the
     * symbol of every match length and distance, which each match needs. */
    tb_deflate_code fixed_literal;
    tb_deflate_code fixed_distance;
    uint8_t length_symbol[TB_DEFLATE_MAX_MATCH + 1];
    uint8_t distance_symbol[2 * TB_DEFLATE_NEAR_DISTANCES];
    tb_deflate_phase phase;
    uint32_t checksum;
    uint64_t total_in;

    /* The window: [0, window_end) holds input; matching is at `position`. */
    unsigned char window[TB_DEFLATE_BUFFER];
    size_t window_end;
    size_t position;
    uint16_t head[TB_DEFLATE_HASH_SIZE];
    uint16_t previous[TB_DEFLATE_BUFFER];

    /* The lazy matcher's state between positions. */
    unsigned previous_length;
    size_t previous_match;
    bool literal_waiting;

    /* The block being collected. */
    tb_deflate_symbol symbols[TB_DEFLATE_SYMBOLS];
    size_t symbol_count;
    size_t block_start; /* Window position of the block's first byte. */
    size_t covered;     /* Window position after the last byte its symbols cover. */
    uint32_t literal_frequency[TB_DEFLATE_LITERALS];
    uint32_t distance_frequency[TB_DEFLATE_DISTANCES];

    /* Encoded bytes not yet given to the caller. */
    unsigned char pending[TB_DEFLATE_PENDING];
    size_t pending_start;
    size_t pending_end;
    uint64_t bits;
    unsigned bit_count;
    /* Input total at the last sync flush, so a repeated flush adds nothing. */
    uint64_t synced_at;
    bool synced;
};

/* -------------------------------------------------------------------------
 * Bits.
 * ------------------------------------------------------------------------- */

static void tb_deflate_put_bits(tb_deflater *deflater, uint32_t value, unsigned count)
{
    TB_ASSERT(count <= 32);
    deflater->bits |= (uint64_t)value << deflater->bit_count;
    deflater->bit_count += count;
    while (deflater->bit_count >= 8)
    {
        TB_ASSERT(deflater->pending_end < TB_DEFLATE_PENDING);
        deflater->pending[deflater->pending_end] = (unsigned char)deflater->bits;
        deflater->pending_end++;
        deflater->bits >>= 8;
        deflater->bit_count -= 8;
    }
}

/* Fill the current byte with zero bits. */
static void tb_deflate_align(tb_deflater *deflater)
{
    if (deflater->bit_count > 0)
    {
        tb_deflate_put_bits(deflater, 0, 8 - deflater->bit_count);
    }
}

static void tb_deflate_put_byte(tb_deflater *deflater, unsigned value)
{
    TB_ASSERT(deflater->bit_count == 0);
    tb_deflate_put_bits(deflater, value & 0xFFu, 8);
}

static void tb_deflate_put32_big(tb_deflater *deflater, uint32_t value)
{
    tb_deflate_put_byte(deflater, value >> 24);
    tb_deflate_put_byte(deflater, value >> 16);
    tb_deflate_put_byte(deflater, value >> 8);
    tb_deflate_put_byte(deflater, value);
}

static void tb_deflate_put32_little(tb_deflater *deflater, uint32_t value)
{
    tb_deflate_put_byte(deflater, value);
    tb_deflate_put_byte(deflater, value >> 8);
    tb_deflate_put_byte(deflater, value >> 16);
    tb_deflate_put_byte(deflater, value >> 24);
}

/* -------------------------------------------------------------------------
 * Huffman codes for a block.
 * ------------------------------------------------------------------------- */

typedef struct tb_deflate_leaf
{
    uint32_t key; /* The frequency, then the parent, then the depth. */
    uint16_t symbol;
} tb_deflate_leaf;

static bool tb_deflate_leaf_before(tb_deflate_leaf left, tb_deflate_leaf right)
{
    return left.key < right.key || (left.key == right.key && left.symbol < right.symbol);
}

/* Sort leaves by frequency, rising; equal frequencies by symbol. That order
 * is total, so the result never depends on how it is sorted. A merge sort,
 * bottom up, since a block has up to 286 leaves and an insertion sort's cost
 * grows with their square. */
static void tb_deflate_sort_leaves(tb_deflate_leaf *leaves, size_t count)
{
    tb_deflate_leaf scratch[TB_DEFLATE_LITERALS] = {{0}};
    tb_deflate_leaf *from = leaves;
    tb_deflate_leaf *to = scratch;
    TB_ASSERT(count <= TB_DEFLATE_LITERALS);
    for (size_t width = 1; width < count; width *= 2)
    {
        for (size_t start = 0; start < count; start += 2 * width)
        {
            const size_t middle = start + width < count ? start + width : count;
            const size_t end = middle + width < count ? middle + width : count;
            size_t left = start;
            size_t right = middle;
            for (size_t out = start; out < end; out++)
            {
                if (left < middle &&
                    (right >= end || !tb_deflate_leaf_before(from[right], from[left])))
                {
                    to[out] = from[left];
                    left++;
                }
                else
                {
                    to[out] = from[right];
                    right++;
                }
            }
        }
        {
            tb_deflate_leaf *const swap = from;
            from = to;
            to = swap;
        }
    }
    if (from != leaves)
    {
        memcpy(leaves, from, count * sizeof(*leaves));
    }
}

/* Optimal code lengths for leaves sorted by rising frequency, computed in
 * place: the algorithm of Moffat and Katajainen, "In-place calculation of
 * minimum-redundancy codes" (1995). On return each key is a length, the
 * longest first. */
static void tb_deflate_minimum_redundancy(tb_deflate_leaf *leaves, size_t count)
{
    size_t root = 0;
    size_t leaf = 2;
    size_t available = 1;
    size_t used = 0;
    uint32_t depth = 0;
    size_t next = 0;
    size_t node = 0;
    if (count == 1)
    {
        leaves[0].key = 1;
        return;
    }
    /* Phase one: combine, leaving each internal node's parent in its key. */
    leaves[0].key += leaves[1].key;
    for (next = 1; next < count - 1; next++)
    {
        if (leaf >= count || leaves[root].key < leaves[leaf].key)
        {
            leaves[next].key = leaves[root].key;
            leaves[root].key = (uint32_t)next;
            root++;
        }
        else
        {
            leaves[next].key = leaves[leaf].key;
            leaf++;
        }
        if (leaf >= count || (root < next && leaves[root].key < leaves[leaf].key))
        {
            leaves[next].key += leaves[root].key;
            leaves[root].key = (uint32_t)next;
            root++;
        }
        else
        {
            leaves[next].key += leaves[leaf].key;
            leaf++;
        }
    }
    /* Phase two: each internal node's depth from its parent's. */
    leaves[count - 2].key = 0;
    for (node = count - 2; node > 0; node--)
    {
        leaves[node - 1].key = leaves[leaves[node - 1].key].key + 1;
    }
    /* Phase three: leaf depths, from the counts of internal nodes at each depth. */
    {
        ptrdiff_t internal = (ptrdiff_t)count - 2;
        next = count;
        while (available > 0)
        {
            while (internal >= 0 && leaves[internal].key == depth)
            {
                used++;
                internal--;
            }
            while (available > used)
            {
                next--;
                leaves[next].key = depth;
                available--;
            }
            available = 2 * used;
            depth++;
            used = 0;
        }
    }
}

/* Lengths for the symbols of a block, none longer than `max_bits`, at least
 * two codes so that every decoder accepts the code. */
static void tb_deflate_build_lengths(const uint32_t *frequency, size_t symbols, unsigned max_bits,
                                     uint8_t *lengths)
{
    tb_deflate_leaf leaves[TB_DEFLATE_LITERALS] = {{0}};
    unsigned count_of_length[TB_DEFLATE_MAX_BITS + 2] = {0};
    size_t count = 0;
    uint32_t kraft = 0;
    size_t leaf = 0;
    memset(lengths, 0, symbols);
    for (size_t symbol = 0; symbol < symbols; symbol++)
    {
        if (frequency[symbol] > 0)
        {
            leaves[count].key = frequency[symbol];
            leaves[count].symbol = (uint16_t)symbol;
            count++;
        }
    }
    /* Fewer than two codes: give the missing ones a frequency of one. */
    for (size_t symbol = 0; count < 2 && symbol < symbols; symbol++)
    {
        if (frequency[symbol] == 0)
        {
            leaves[count].key = 1;
            leaves[count].symbol = (uint16_t)symbol;
            count++;
        }
    }
    tb_deflate_sort_leaves(leaves, count);
    tb_deflate_minimum_redundancy(leaves, count);
    /* Count lengths, folding any too long into the longest allowed... */
    for (size_t index = 0; index < count; index++)
    {
        unsigned length = leaves[index].key;
        if (length > max_bits)
        {
            length = max_bits;
        }
        count_of_length[length]++;
    }
    /* ...then lengthen shorter codes until the lengths fit a prefix code. */
    for (unsigned length = 1; length <= max_bits; length++)
    {
        kraft += count_of_length[length] << (max_bits - length);
    }
    while (kraft > (1u << max_bits))
    {
        count_of_length[max_bits]--;
        for (unsigned length = max_bits - 1; length > 0; length--)
        {
            if (count_of_length[length] > 0)
            {
                count_of_length[length]--;
                count_of_length[length + 1] += 2;
                break;
            }
        }
        kraft--;
    }
    /* The rarest symbols, first in `leaves`, take the longest codes. */
    for (unsigned length = max_bits; length > 0; length--)
    {
        for (unsigned remaining = count_of_length[length]; remaining > 0; remaining--)
        {
            lengths[leaves[leaf].symbol] = (uint8_t)length;
            leaf++;
        }
    }
}

static unsigned tb_deflate_reverse(unsigned code, unsigned length)
{
    unsigned reversed = 0;
    for (unsigned bit = 0; bit < length; bit++)
    {
        reversed = (reversed << 1) | ((code >> bit) & 1u);
    }
    return reversed;
}

/* Canonical codes from lengths, as RFC 1951 section 3.2.2 assigns them. */
static void tb_deflate_assign_codes(tb_deflate_code *code, size_t symbols)
{
    unsigned count[TB_DEFLATE_MAX_BITS + 1] = {0};
    unsigned next[TB_DEFLATE_MAX_BITS + 1] = {0};
    unsigned value = 0;
    for (size_t symbol = 0; symbol < symbols; symbol++)
    {
        count[code->length[symbol]]++;
    }
    count[0] = 0;
    for (unsigned length = 1; length <= TB_DEFLATE_MAX_BITS; length++)
    {
        value = (value + count[length - 1]) << 1;
        next[length] = value;
    }
    for (size_t symbol = 0; symbol < symbols; symbol++)
    {
        const unsigned length = code->length[symbol];
        if (length > 0)
        {
            code->bits[symbol] = (uint16_t)tb_deflate_reverse(next[length], length);
            next[length]++;
        }
    }
}

static void tb_deflate_fixed_codes(tb_deflate_code *literal, tb_deflate_code *distance)
{
    memset(literal, 0, sizeof(*literal));
    memset(distance, 0, sizeof(*distance));
    memset(literal->length, 8, 144);
    memset(literal->length + 144, 9, 112);
    memset(literal->length + 256, 7, 24);
    memset(literal->length + 280, 8, TB_DEFLATE_FIXED_LITERALS - 280);
    memset(distance->length, 5, TB_DEFLATE_DISTANCES);
    tb_deflate_assign_codes(literal, TB_DEFLATE_FIXED_LITERALS);
    tb_deflate_assign_codes(distance, TB_DEFLATE_DISTANCES);
}

/* The symbol of each length and distance, by search of the base tables. */
static void tb_deflate_build_symbols(tb_deflater *deflater)
{
    unsigned symbol = 0;
    for (unsigned length = TB_DEFLATE_MIN_MATCH; length <= TB_DEFLATE_MAX_MATCH; length++)
    {
        while (symbol + 1 < 29 && tb_deflate_length_base[symbol + 1] <= length)
        {
            symbol++;
        }
        deflater->length_symbol[length] = (uint8_t)symbol;
    }
    symbol = 0;
    for (unsigned distance = 1; distance <= TB_DEFLATE_NEAR_DISTANCES; distance++)
    {
        while (symbol + 1 < 30 && tb_deflate_distance_base[symbol + 1] <= distance)
        {
            symbol++;
        }
        deflater->distance_symbol[distance - 1] = (uint8_t)symbol;
    }
    for (size_t slot = 0; slot < TB_DEFLATE_NEAR_DISTANCES; slot++)
    {
        /* The first distance of each 128 past 256; each 128 has one symbol. */
        const size_t distance = ((slot + 2) << TB_DEFLATE_FAR_SHIFT) + 1;
        symbol = 0;
        while (symbol + 1 < 30 && tb_deflate_distance_base[symbol + 1] <= distance)
        {
            symbol++;
        }
        deflater->distance_symbol[TB_DEFLATE_NEAR_DISTANCES + slot] = (uint8_t)symbol;
    }
}

static unsigned tb_deflate_length_symbol(const tb_deflater *deflater, unsigned length)
{
    TB_ASSERT(length >= TB_DEFLATE_MIN_MATCH && length <= TB_DEFLATE_MAX_MATCH);
    return deflater->length_symbol[length];
}

static unsigned tb_deflate_distance_symbol(const tb_deflater *deflater, unsigned distance)
{
    TB_ASSERT(distance >= 1 && distance <= TB_DEFLATE_WINDOW);
    if (distance <= TB_DEFLATE_NEAR_DISTANCES)
    {
        return deflater->distance_symbol[distance - 1];
    }
    return deflater
        ->distance_symbol[TB_DEFLATE_NEAR_DISTANCES - 2 + ((distance - 1) >> TB_DEFLATE_FAR_SHIFT)];
}

/* -------------------------------------------------------------------------
 * Writing a block.
 * ------------------------------------------------------------------------- */

/* The code-length symbols that describe a dynamic block's code lengths:
 * runs of zeros as 17 and 18, runs of a repeated length as 16. */
typedef struct tb_deflate_length_run
{
    uint8_t symbol;
    uint8_t extra;
} tb_deflate_length_run;

static size_t tb_deflate_encode_lengths(const uint8_t *lengths, size_t count,
                                        tb_deflate_length_run *runs, uint32_t *frequency)
{
    size_t produced = 0;
    size_t index = 0;
    while (index < count)
    {
        const uint8_t value = lengths[index];
        size_t run = 1;
        while (index + run < count && lengths[index + run] == value)
        {
            run++;
        }
        index += run;
        if (value == 0)
        {
            while (run >= 11)
            {
                const size_t piece = run > 138 ? 138 : run;
                runs[produced] = (tb_deflate_length_run){18, (uint8_t)(piece - 11)};
                produced++;
                run -= piece;
            }
            if (run >= 3)
            {
                runs[produced] = (tb_deflate_length_run){17, (uint8_t)(run - 3)};
                produced++;
                run = 0;
            }
        }
        else
        {
            runs[produced] = (tb_deflate_length_run){value, 0};
            produced++;
            run--;
            while (run >= 3)
            {
                const size_t piece = run > 6 ? 6 : run;
                runs[produced] = (tb_deflate_length_run){16, (uint8_t)(piece - 3)};
                produced++;
                run -= piece;
            }
        }
        while (run > 0)
        {
            runs[produced] = (tb_deflate_length_run){value, 0};
            produced++;
            run--;
        }
    }
    for (size_t at = 0; at < produced; at++)
    {
        frequency[runs[at].symbol]++;
    }
    return produced;
}

static unsigned tb_deflate_run_extra_bits(unsigned symbol)
{
    switch (symbol)
    {
    case 16:
        return 2;
    case 17:
        return 3;
    case 18:
        return 7;
    default:
        return 0;
    }
}

/* What a dynamic block's header costs and says. */
typedef struct tb_deflate_dynamic
{
    tb_deflate_code literal;
    tb_deflate_code distance;
    tb_deflate_code code_length;
    tb_deflate_length_run runs[TB_DEFLATE_LITERALS + TB_DEFLATE_DISTANCES];
    size_t run_count;
    unsigned literal_count;
    unsigned distance_count;
    unsigned code_length_count;
    uint64_t header_bits;
} tb_deflate_dynamic;

static void tb_deflate_plan_dynamic(const tb_deflater *deflater, tb_deflate_dynamic *plan)
{
    uint8_t lengths[TB_DEFLATE_LITERALS + TB_DEFLATE_DISTANCES] = {0};
    uint32_t frequency[TB_DEFLATE_CODE_LENGTHS] = {0};
    memset(plan, 0, sizeof(*plan));
    tb_deflate_build_lengths(deflater->literal_frequency, TB_DEFLATE_LITERALS, TB_DEFLATE_MAX_BITS,
                             plan->literal.length);
    tb_deflate_build_lengths(deflater->distance_frequency, TB_DEFLATE_DISTANCES,
                             TB_DEFLATE_MAX_BITS, plan->distance.length);
    tb_deflate_assign_codes(&plan->literal, TB_DEFLATE_LITERALS);
    tb_deflate_assign_codes(&plan->distance, TB_DEFLATE_DISTANCES);
    plan->literal_count = TB_DEFLATE_LITERALS;
    while (plan->literal_count > 257 && plan->literal.length[plan->literal_count - 1] == 0)
    {
        plan->literal_count--;
    }
    plan->distance_count = TB_DEFLATE_DISTANCES;
    while (plan->distance_count > 1 && plan->distance.length[plan->distance_count - 1] == 0)
    {
        plan->distance_count--;
    }
    memcpy(lengths, plan->literal.length, plan->literal_count);
    memcpy(lengths + plan->literal_count, plan->distance.length, plan->distance_count);
    plan->run_count = tb_deflate_encode_lengths(lengths, plan->literal_count + plan->distance_count,
                                                plan->runs, frequency);
    tb_deflate_build_lengths(frequency, TB_DEFLATE_CODE_LENGTHS, TB_DEFLATE_MAX_CODE_LENGTH_BITS,
                             plan->code_length.length);
    tb_deflate_assign_codes(&plan->code_length, TB_DEFLATE_CODE_LENGTHS);
    plan->code_length_count = TB_DEFLATE_CODE_LENGTHS;
    while (plan->code_length_count > 4 &&
           plan->code_length.length[tb_deflate_code_length_order[plan->code_length_count - 1]] == 0)
    {
        plan->code_length_count--;
    }
    plan->header_bits = 5 + 5 + 4 + 3 * (uint64_t)plan->code_length_count;
    for (size_t at = 0; at < plan->run_count; at++)
    {
        const unsigned symbol = plan->runs[at].symbol;
        plan->header_bits += plan->code_length.length[symbol] + tb_deflate_run_extra_bits(symbol);
    }
}

/* Bits the block's symbols take with the given codes. */
static uint64_t tb_deflate_data_bits(const tb_deflater *deflater, const tb_deflate_code *literal,
                                     const tb_deflate_code *distance)
{
    uint64_t bits = 0;
    for (size_t symbol = 0; symbol < TB_DEFLATE_LITERALS; symbol++)
    {
        uint64_t extra = 0;
        if (symbol > TB_DEFLATE_END_OF_BLOCK)
        {
            extra = tb_deflate_length_extra[symbol - 257];
        }
        bits += (uint64_t)deflater->literal_frequency[symbol] * (literal->length[symbol] + extra);
    }
    for (size_t symbol = 0; symbol < TB_DEFLATE_DISTANCES; symbol++)
    {
        bits += (uint64_t)deflater->distance_frequency[symbol] *
                (distance->length[symbol] + tb_deflate_distance_extra[symbol]);
    }
    return bits;
}

static void tb_deflate_write_symbols(tb_deflater *deflater, const tb_deflate_code *literal,
                                     const tb_deflate_code *distance)
{
    for (size_t index = 0; index < deflater->symbol_count; index++)
    {
        const tb_deflate_symbol symbol = deflater->symbols[index];
        if (symbol.distance == 0)
        {
            tb_deflate_put_bits(deflater, literal->bits[symbol.literal_or_length],
                                literal->length[symbol.literal_or_length]);
            continue;
        }
        {
            const unsigned length_symbol =
                tb_deflate_length_symbol(deflater, symbol.literal_or_length);
            const unsigned distance_symbol = tb_deflate_distance_symbol(deflater, symbol.distance);
            tb_deflate_put_bits(deflater, literal->bits[257 + length_symbol],
                                literal->length[257 + length_symbol]);
            tb_deflate_put_bits(deflater,
                                symbol.literal_or_length - tb_deflate_length_base[length_symbol],
                                tb_deflate_length_extra[length_symbol]);
            tb_deflate_put_bits(deflater, distance->bits[distance_symbol],
                                distance->length[distance_symbol]);
            tb_deflate_put_bits(deflater,
                                symbol.distance - tb_deflate_distance_base[distance_symbol],
                                tb_deflate_distance_extra[distance_symbol]);
        }
    }
    tb_deflate_put_bits(deflater, literal->bits[TB_DEFLATE_END_OF_BLOCK],
                        literal->length[TB_DEFLATE_END_OF_BLOCK]);
}

static void tb_deflate_write_dynamic_header(tb_deflater *deflater, const tb_deflate_dynamic *plan)
{
    tb_deflate_put_bits(deflater, plan->literal_count - 257, 5);
    tb_deflate_put_bits(deflater, plan->distance_count - 1, 5);
    tb_deflate_put_bits(deflater, plan->code_length_count - 4, 4);
    for (unsigned index = 0; index < plan->code_length_count; index++)
    {
        tb_deflate_put_bits(deflater, plan->code_length.length[tb_deflate_code_length_order[index]],
                            3);
    }
    for (size_t at = 0; at < plan->run_count; at++)
    {
        const unsigned symbol = plan->runs[at].symbol;
        tb_deflate_put_bits(deflater, plan->code_length.bits[symbol],
                            plan->code_length.length[symbol]);
        tb_deflate_put_bits(deflater, plan->runs[at].extra, tb_deflate_run_extra_bits(symbol));
    }
}

/* Store bytes of the window in blocks of at most 65535. */
static void tb_deflate_write_stored(tb_deflater *deflater, size_t start, size_t length, bool final)
{
    do
    {
        const size_t piece = length > TB_DEFLATE_STORED_MAX ? TB_DEFLATE_STORED_MAX : length;
        const bool last = final && piece == length;
        tb_deflate_put_bits(deflater, last ? 1u : 0u, 1);
        tb_deflate_put_bits(deflater, 0, 2);
        tb_deflate_align(deflater);
        tb_deflate_put_bits(deflater, (uint32_t)piece, 16);
        tb_deflate_put_bits(deflater, (uint32_t)(~piece & 0xFFFFu), 16);
        TB_ASSERT(deflater->pending_end + piece <= TB_DEFLATE_PENDING);
        memcpy(deflater->pending + deflater->pending_end, deflater->window + start, piece);
        deflater->pending_end += piece;
        start += piece;
        length -= piece;
    } while (length > 0);
}

/* Write the block collected, in its smallest form, and start the next. */
static void tb_deflate_end_block(tb_deflater *deflater, bool final)
{
    tb_deflate_dynamic plan = {0};
    const size_t raw = deflater->covered - deflater->block_start;
    uint64_t stored_bits = 0;
    uint64_t fixed_bits = 0;
    uint64_t dynamic_bits = 0;
    deflater->literal_frequency[TB_DEFLATE_END_OF_BLOCK]++;
    /* A stored block: up to 7 bits to the byte, then four bytes of lengths. */
    stored_bits = (uint64_t)(raw / TB_DEFLATE_STORED_MAX + 1) * (3 + 7 + 32) + 8 * (uint64_t)raw;
    if (deflater->level > 0)
    {
        tb_deflate_plan_dynamic(deflater, &plan);
        fixed_bits =
            3 + tb_deflate_data_bits(deflater, &deflater->fixed_literal, &deflater->fixed_distance);
        dynamic_bits =
            3 + plan.header_bits + tb_deflate_data_bits(deflater, &plan.literal, &plan.distance);
    }
    if (deflater->level == 0 || (stored_bits <= fixed_bits && stored_bits <= dynamic_bits))
    {
        tb_deflate_write_stored(deflater, deflater->block_start, raw, final);
    }
    else if (fixed_bits <= dynamic_bits)
    {
        tb_deflate_put_bits(deflater, final ? 1u : 0u, 1);
        tb_deflate_put_bits(deflater, 1, 2);
        tb_deflate_write_symbols(deflater, &deflater->fixed_literal, &deflater->fixed_distance);
    }
    else
    {
        tb_deflate_put_bits(deflater, final ? 1u : 0u, 1);
        tb_deflate_put_bits(deflater, 2, 2);
        tb_deflate_write_dynamic_header(deflater, &plan);
        tb_deflate_write_symbols(deflater, &plan.literal, &plan.distance);
    }
    deflater->symbol_count = 0;
    deflater->block_start = deflater->covered;
    memset(deflater->literal_frequency, 0, sizeof(deflater->literal_frequency));
    memset(deflater->distance_frequency, 0, sizeof(deflater->distance_frequency));
}

/* -------------------------------------------------------------------------
 * Matching.
 * ------------------------------------------------------------------------- */

static uint32_t tb_deflate_hash(const unsigned char *bytes)
{
    const uint32_t value =
        (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16);
    return (value * 2654435761u) >> (32 - TB_DEFLATE_HASH_BITS);
}

/* Link a position into the chain of its hash; return the previous head. */
static unsigned tb_deflate_insert(tb_deflater *deflater, size_t position)
{
    uint32_t hash = 0;
    unsigned previous_head = 0;
    if (position + TB_DEFLATE_MIN_MATCH > deflater->window_end)
    {
        return TB_DEFLATE_NO_LINK;
    }
    /* A position with three bytes after it is below the window's last two. */
    TB_ASSERT(position + 1 < TB_DEFLATE_BUFFER);
    hash = tb_deflate_hash(deflater->window + position);
    previous_head = deflater->head[hash];
    deflater->previous[position] = (uint16_t)previous_head;
    deflater->head[hash] = (uint16_t)(position + 1);
    return previous_head;
}

/* How many of the first `limit` bytes of two strings are equal: eight
 * bytes at a time while they all are, then one at a time. */
static unsigned tb_deflate_common_length(const unsigned char *here, const unsigned char *there,
                                         unsigned limit)
{
    unsigned length = 0;
    while (length + sizeof(uint64_t) <= limit)
    {
        uint64_t mine = 0;
        uint64_t theirs = 0;
        memcpy(&mine, here + length, sizeof(mine));
        memcpy(&theirs, there + length, sizeof(theirs));
        if (mine != theirs)
        {
            break;
        }
        length += (unsigned)sizeof(uint64_t);
    }
    while (length < limit && there[length] == here[length])
    {
        length++;
    }
    return length;
}

/* The longest match at `position` along the chain from `candidate`. */
static unsigned tb_deflate_longest_match(const tb_deflater *deflater, size_t position,
                                         unsigned link, unsigned previous_length, size_t *match)
{
    const size_t available = deflater->window_end - position;
    const unsigned limit =
        available < TB_DEFLATE_MAX_MATCH ? (unsigned)available : TB_DEFLATE_MAX_MATCH;
    const size_t lowest =
        position > TB_DEFLATE_MAX_DISTANCE ? position - TB_DEFLATE_MAX_DISTANCE : 0;
    unsigned chain = deflater->effort.max_chain;
    unsigned best = previous_length;
    const unsigned char *const here = deflater->window + position;
    TB_ASSERT(previous_length >= TB_DEFLATE_MIN_MATCH - 1);
    if (previous_length >= deflater->effort.good_length)
    {
        chain >>= 2;
    }
    /* A link is its position plus one, and none is 0: links above `lowest`
     * are positions within reach. */
    while (link > lowest && link <= position && chain > 0)
    {
        const size_t candidate = (size_t)link - 1;
        const unsigned char *const there = deflater->window + candidate;
        unsigned length = 0;
        /* Only a candidate whose first best + 1 bytes all match can do
         * better; test the bytes most likely to differ before the rest. */
        if (best < limit && there[best] == here[best] && there[best - 1] == here[best - 1] &&
            there[0] == here[0] && there[1] == here[1])
        {
            length = tb_deflate_common_length(here, there, limit);
            if (length > best)
            {
                best = length;
                *match = candidate;
                if (length >= deflater->effort.nice_length || length >= limit)
                {
                    break;
                }
            }
        }
        link = deflater->previous[candidate];
        chain--;
    }
    return best;
}

static bool tb_deflate_block_full(const tb_deflater *deflater)
{
    return deflater->symbol_count == TB_DEFLATE_SYMBOLS;
}

static void tb_deflate_emit_literal(tb_deflater *deflater, size_t position)
{
    const unsigned value = deflater->window[position];
    deflater->symbols[deflater->symbol_count] = (tb_deflate_symbol){(uint16_t)value, 0};
    deflater->symbol_count++;
    deflater->literal_frequency[value]++;
    deflater->covered = position + 1;
}

static void tb_deflate_emit_match(tb_deflater *deflater, size_t position, unsigned length,
                                  size_t distance)
{
    deflater->symbols[deflater->symbol_count] =
        (tb_deflate_symbol){(uint16_t)length, (uint16_t)distance};
    deflater->symbol_count++;
    deflater->literal_frequency[257 + tb_deflate_length_symbol(deflater, length)]++;
    deflater->distance_frequency[tb_deflate_distance_symbol(deflater, (unsigned)distance)]++;
    deflater->covered = position + length;
}

/* Insert the positions after the first of a match, which the matcher skips. */
static void tb_deflate_insert_run(tb_deflater *deflater, size_t from, size_t to)
{
    for (size_t position = from; position < to; position++)
    {
        (void)tb_deflate_insert(deflater, position);
    }
}

/* Is a match worth taking? A short one far back costs more than literals. */
static bool tb_deflate_worth(unsigned length, size_t distance)
{
    return length > TB_DEFLATE_MIN_MATCH ||
           (length == TB_DEFLATE_MIN_MATCH && distance <= TB_DEFLATE_TOO_FAR);
}

/* One step of the greedy matcher at `position`. */
static void tb_deflate_step_greedy(tb_deflater *deflater)
{
    const size_t position = deflater->position;
    const unsigned link = tb_deflate_insert(deflater, position);
    size_t match = 0;
    unsigned length = 0;
    if (link != TB_DEFLATE_NO_LINK)
    {
        length =
            tb_deflate_longest_match(deflater, position, link, TB_DEFLATE_MIN_MATCH - 1, &match);
    }
    if (length >= TB_DEFLATE_MIN_MATCH && tb_deflate_worth(length, position - match))
    {
        tb_deflate_emit_match(deflater, position, length, position - match);
        tb_deflate_insert_run(deflater, position + 1, position + length);
        deflater->position += length;
        return;
    }
    tb_deflate_emit_literal(deflater, position);
    deflater->position++;
}

/* One step of the lazy matcher: a match found at the previous position is
 * taken only if the one found here is no longer. */
static void tb_deflate_step_lazy(tb_deflater *deflater)
{
    const size_t position = deflater->position;
    const unsigned link = tb_deflate_insert(deflater, position);
    const unsigned previous_length = deflater->previous_length;
    const size_t previous_match = deflater->previous_match;
    size_t match = 0;
    unsigned length = TB_DEFLATE_MIN_MATCH - 1;
    if (link != TB_DEFLATE_NO_LINK && previous_length < deflater->effort.lazy_length)
    {
        length =
            tb_deflate_longest_match(deflater, position, link, TB_DEFLATE_MIN_MATCH - 1, &match);
        if (length >= TB_DEFLATE_MIN_MATCH && !tb_deflate_worth(length, position - match))
        {
            length = TB_DEFLATE_MIN_MATCH - 1;
        }
    }
    if (previous_length >= TB_DEFLATE_MIN_MATCH && length <= previous_length)
    {
        /* The match at the previous position wins. */
        const size_t start = position - 1;
        tb_deflate_emit_match(deflater, start, previous_length, start - previous_match);
        tb_deflate_insert_run(deflater, position + 1, start + previous_length);
        deflater->position = start + previous_length;
        deflater->previous_length = TB_DEFLATE_MIN_MATCH - 1;
        deflater->literal_waiting = false;
        return;
    }
    if (deflater->literal_waiting)
    {
        tb_deflate_emit_literal(deflater, position - 1);
    }
    deflater->literal_waiting = true;
    deflater->previous_length = length;
    deflater->previous_match = match;
    deflater->position++;
}

/* Move the upper half of the window down, and every position with it. */
static void tb_deflate_slide(tb_deflater *deflater)
{
    TB_ASSERT(deflater->block_start >= TB_DEFLATE_WINDOW);
    memmove(deflater->window, deflater->window + TB_DEFLATE_WINDOW,
            deflater->window_end - TB_DEFLATE_WINDOW);
    deflater->window_end -= TB_DEFLATE_WINDOW;
    deflater->position -= TB_DEFLATE_WINDOW;
    deflater->block_start -= TB_DEFLATE_WINDOW;
    deflater->covered -= TB_DEFLATE_WINDOW;
    deflater->previous_match = deflater->previous_match >= TB_DEFLATE_WINDOW
                                   ? deflater->previous_match - TB_DEFLATE_WINDOW
                                   : 0;
    /* A link to the lower half, which is gone, becomes none. */
    for (size_t index = 0; index < TB_DEFLATE_HASH_SIZE; index++)
    {
        const unsigned link = deflater->head[index];
        deflater->head[index] =
            (uint16_t)(link > TB_DEFLATE_WINDOW ? link - TB_DEFLATE_WINDOW : TB_DEFLATE_NO_LINK);
    }
    for (size_t index = 0; index < TB_DEFLATE_WINDOW; index++)
    {
        const unsigned link = deflater->previous[index + TB_DEFLATE_WINDOW];
        deflater->previous[index] =
            (uint16_t)(link > TB_DEFLATE_WINDOW ? link - TB_DEFLATE_WINDOW : TB_DEFLATE_NO_LINK);
    }
}

/* -------------------------------------------------------------------------
 * The interface.
 * ------------------------------------------------------------------------- */

static void tb_deflate_write_header(tb_deflater *deflater)
{
    if (deflater->format == TB_DEFLATE_ZLIB)
    {
        /* Method 8 with a 32 KiB window, and the level in two bits. */
        unsigned level_bits = 2;
        unsigned flags = 0;
        if (deflater->level <= 1)
        {
            level_bits = 0;
        }
        else if (deflater->level <= 5)
        {
            level_bits = 1;
        }
        else if (deflater->level >= 7)
        {
            level_bits = 3;
        }
        flags = level_bits << 6;
        flags += 31 - ((0x78u << 8) | flags) % 31;
        tb_deflate_put_byte(deflater, 0x78);
        tb_deflate_put_byte(deflater, flags);
    }
    else if (deflater->format == TB_DEFLATE_GZIP)
    {
        /* No name, no time, made on an unknown system. */
        static const unsigned char header[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};
        for (size_t index = 0; index < sizeof(header); index++)
        {
            unsigned value = header[index];
            if (index == 8 && deflater->level == TB_DEFLATE_MAX_LEVEL)
            {
                value = 2;
            }
            else if (index == 8 && deflater->level == 1)
            {
                value = 4;
            }
            tb_deflate_put_byte(deflater, value);
        }
    }
}

static void tb_deflate_write_trailer(tb_deflater *deflater)
{
    tb_deflate_align(deflater);
    if (deflater->format == TB_DEFLATE_ZLIB)
    {
        tb_deflate_put32_big(deflater, deflater->checksum);
    }
    else if (deflater->format == TB_DEFLATE_GZIP)
    {
        tb_deflate_put32_little(deflater, deflater->checksum);
        tb_deflate_put32_little(deflater, (uint32_t)deflater->total_in);
    }
}

void tb_deflater_reset(tb_deflater *deflater)
{
    TB_ASSERT(deflater != NULL);
    deflater->phase = TB_DEFLATE_PHASE_HEADER;
    deflater->checksum =
        deflater->format == TB_DEFLATE_ZLIB ? TB_ADLER32_INITIAL : TB_CRC32_INITIAL;
    deflater->total_in = 0;
    deflater->window_end = 0;
    deflater->position = 0;
    deflater->previous_length = TB_DEFLATE_MIN_MATCH - 1;
    deflater->previous_match = 0;
    deflater->literal_waiting = false;
    deflater->symbol_count = 0;
    deflater->block_start = 0;
    deflater->covered = 0;
    deflater->pending_start = 0;
    deflater->pending_end = 0;
    deflater->bits = 0;
    deflater->bit_count = 0;
    deflater->synced_at = 0;
    deflater->synced = false;
    memset(deflater->literal_frequency, 0, sizeof(deflater->literal_frequency));
    memset(deflater->distance_frequency, 0, sizeof(deflater->distance_frequency));
    for (size_t index = 0; index < TB_DEFLATE_HASH_SIZE; index++)
    {
        deflater->head[index] = TB_DEFLATE_NO_LINK;
    }
}

tb_deflater *tb_deflater_create(tb_deflate_format format, int level, const tb_allocator *allocator)
{
    tb_allocator resolved = {0};
    tb_deflater *deflater = NULL;
    TB_ASSERT_MSG(format == TB_DEFLATE_RAW || format == TB_DEFLATE_ZLIB ||
                      format == TB_DEFLATE_GZIP,
                  "unknown DEFLATE format");
    TB_ASSERT_MSG(level >= 0 && level <= TB_DEFLATE_MAX_LEVEL, "levels run from 0 to 9");
    resolved = tb_allocator_resolve(allocator);
    deflater = TB_ALLOCATOR_NEW(&resolved, tb_deflater);
    deflater->allocator = resolved;
    deflater->format = format;
    deflater->level = level;
    deflater->effort = tb_deflate_efforts[level];
    tb_deflate_fixed_codes(&deflater->fixed_literal, &deflater->fixed_distance);
    tb_deflate_build_symbols(deflater);
    tb_deflater_reset(deflater);
    return deflater;
}

void tb_deflater_destroy(tb_deflater *deflater)
{
    tb_allocator allocator = {0};
    if (deflater == NULL)
    {
        return;
    }
    allocator = deflater->allocator;
    TB_ALLOCATOR_DELETE(&allocator, tb_deflater, deflater);
}

bool tb_deflater_finished(const tb_deflater *deflater)
{
    TB_ASSERT(deflater != NULL);
    return deflater->phase == TB_DEFLATE_PHASE_DONE &&
           deflater->pending_start == deflater->pending_end;
}

/* Give pending bytes to the caller; true when none are left. */
static bool tb_deflate_drain(tb_deflater *deflater, unsigned char *output, size_t capacity,
                             size_t *written)
{
    size_t piece = deflater->pending_end - deflater->pending_start;
    if (piece > capacity - *written)
    {
        piece = capacity - *written;
    }
    if (piece > 0)
    {
        memcpy(output + *written, deflater->pending + deflater->pending_start, piece);
        *written += piece;
        deflater->pending_start += piece;
    }
    if (deflater->pending_start < deflater->pending_end)
    {
        return false;
    }
    deflater->pending_start = 0;
    deflater->pending_end = 0;
    return true;
}

/* Copy input into the free end of the window. */
static size_t tb_deflate_fill(tb_deflater *deflater, const unsigned char *input, size_t length)
{
    size_t piece = TB_DEFLATE_BUFFER - deflater->window_end;
    if (piece > length)
    {
        piece = length;
    }
    if (piece == 0)
    {
        return 0;
    }
    memcpy(deflater->window + deflater->window_end, input, piece);
    if (deflater->format == TB_DEFLATE_ZLIB)
    {
        deflater->checksum = tb_adler32_update(deflater->checksum, input, piece);
    }
    else if (deflater->format == TB_DEFLATE_GZIP)
    {
        deflater->checksum = tb_crc32_update(deflater->checksum, input, piece);
    }
    deflater->window_end += piece;
    deflater->total_in += piece;
    return piece;
}

/* Match until a block is written, the lookahead runs short, or the window
 * must slide. `flushing` lets the matcher use a short lookahead, at the end. */
typedef enum tb_deflate_progress
{
    TB_DEFLATE_PROGRESS_BLOCK_WRITTEN,
    TB_DEFLATE_PROGRESS_NEED_INPUT,
    TB_DEFLATE_PROGRESS_ALL_MATCHED
} tb_deflate_progress;

static tb_deflate_progress tb_deflate_match(tb_deflater *deflater, bool flushing)
{
    for (;;)
    {
        const size_t lookahead = deflater->window_end - deflater->position;
        if (tb_deflate_block_full(deflater))
        {
            tb_deflate_end_block(deflater, false);
            return TB_DEFLATE_PROGRESS_BLOCK_WRITTEN;
        }
        if (deflater->position >= TB_DEFLATE_SLIDE_AT)
        {
            /* A block never spans a slide, so its bytes stay in the window. */
            if (deflater->covered > deflater->block_start)
            {
                tb_deflate_end_block(deflater, false);
                return TB_DEFLATE_PROGRESS_BLOCK_WRITTEN;
            }
            tb_deflate_slide(deflater);
            continue;
        }
        if (lookahead < TB_DEFLATE_LOOKAHEAD && !flushing)
        {
            return TB_DEFLATE_PROGRESS_NEED_INPUT;
        }
        if (lookahead == 0)
        {
            return TB_DEFLATE_PROGRESS_ALL_MATCHED;
        }
        if (deflater->effort.lazy)
        {
            tb_deflate_step_lazy(deflater);
        }
        else
        {
            tb_deflate_step_greedy(deflater);
        }
    }
}

/* At a flush, the lazy matcher's waiting literal is all that is left. */
static void tb_deflate_settle_lazy(tb_deflater *deflater)
{
    if (deflater->literal_waiting)
    {
        tb_deflate_emit_literal(deflater, deflater->position - 1);
        deflater->literal_waiting = false;
        deflater->previous_length = TB_DEFLATE_MIN_MATCH - 1;
    }
}

tb_deflate_result tb_deflater_run(tb_deflater *deflater, const void *input, size_t input_length,
                                  size_t *input_used, void *output, size_t output_capacity,
                                  size_t *output_written, tb_deflate_flush flush)
{
    const unsigned char *const bytes = input;
    TB_ASSERT(deflater != NULL);
    TB_ASSERT(input != NULL || input_length == 0);
    TB_ASSERT(output != NULL || output_capacity == 0);
    TB_ASSERT(input_used != NULL);
    TB_ASSERT(output_written != NULL);
    *input_used = 0;
    *output_written = 0;
    if (flush != TB_DEFLATE_NO_FLUSH && flush != TB_DEFLATE_SYNC_FLUSH &&
        flush != TB_DEFLATE_FINISH)
    {
        return TB_DEFLATE_INVALID;
    }
    if (deflater->phase == TB_DEFLATE_PHASE_HEADER)
    {
        tb_deflate_write_header(deflater);
        deflater->phase = TB_DEFLATE_PHASE_DATA;
    }
    for (;;)
    {
        tb_deflate_progress progress = TB_DEFLATE_PROGRESS_NEED_INPUT;
        bool flushing = false;
        if (!tb_deflate_drain(deflater, output, output_capacity, output_written))
        {
            return TB_DEFLATE_OK;
        }
        if (deflater->phase == TB_DEFLATE_PHASE_TRAILER)
        {
            tb_deflate_write_trailer(deflater);
            deflater->phase = TB_DEFLATE_PHASE_DONE;
            continue;
        }
        if (deflater->phase == TB_DEFLATE_PHASE_DONE)
        {
            return TB_DEFLATE_OK;
        }
        *input_used += tb_deflate_fill(deflater, bytes + *input_used, input_length - *input_used);
        flushing = flush != TB_DEFLATE_NO_FLUSH && *input_used == input_length;
        progress = tb_deflate_match(deflater, flushing);
        if (progress == TB_DEFLATE_PROGRESS_BLOCK_WRITTEN)
        {
            continue;
        }
        if (progress == TB_DEFLATE_PROGRESS_NEED_INPUT)
        {
            if (*input_used < input_length)
            {
                continue;
            }
            return TB_DEFLATE_OK;
        }
        /* Everything given has been matched, and a flush was asked for. */
        tb_deflate_settle_lazy(deflater);
        if (flush == TB_DEFLATE_FINISH)
        {
            tb_deflate_end_block(deflater, true);
            deflater->phase = TB_DEFLATE_PHASE_TRAILER;
            continue;
        }
        if (deflater->synced && deflater->synced_at == deflater->total_in)
        {
            return TB_DEFLATE_OK;
        }
        /* A sync flush: end the block, then an empty stored block to reach a byte. */
        if (deflater->covered > deflater->block_start)
        {
            tb_deflate_end_block(deflater, false);
        }
        tb_deflate_write_stored(deflater, deflater->covered, 0, false);
        deflater->synced = true;
        deflater->synced_at = deflater->total_in;
    }
}

size_t tb_deflate_bound(size_t input_length)
{
    /* Stored blocks at worst, five bytes each at most every 8192 input bytes,
     * and room for framing, a final empty block, and a sync flush. */
    const size_t blocks = input_length / 8192 + 4;
    const size_t overhead = blocks * 5 + 64;
    if (input_length > SIZE_MAX - overhead)
    {
        return SIZE_MAX;
    }
    return input_length + overhead;
}

tb_deflate_result tb_deflate(tb_deflate_format format, int level, const void *input,
                             size_t input_length, void *output, size_t capacity, size_t *written)
{
    tb_deflater *deflater = NULL;
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
    if (level < 0 || level > TB_DEFLATE_MAX_LEVEL)
    {
        return TB_DEFLATE_INVALID;
    }
    deflater = tb_deflater_create(format, level, NULL);
    result = tb_deflater_run(deflater, input, input_length, &used, output, capacity, written,
                             TB_DEFLATE_FINISH);
    if (result == TB_DEFLATE_OK && !tb_deflater_finished(deflater))
    {
        result = TB_DEFLATE_TOO_LARGE;
    }
    tb_deflater_destroy(deflater);
    return result;
}
