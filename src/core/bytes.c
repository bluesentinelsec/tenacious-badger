#include "bytes.h"

#include <string.h>

/* A word with the lowest, and with the highest, bit of every byte set. */
#define TB_BYTES_LOW_BITS UINT64_C(0x0101010101010101)
#define TB_BYTES_HIGH_BITS UINT64_C(0x8080808080808080)

/* The multiplier of the rolling hash: the prime of 32-bit FNV. */
#define TB_BYTES_ROLLING_PRIME ((uint32_t)16777619)

/* A pattern search turns to the rolling hash when more than one candidate in
 * this many bytes has turned out false, after a first few that are free. */
#define TB_BYTES_FALSE_CANDIDATE_INTERVAL ((size_t)8)
#define TB_BYTES_FREE_FALSE_CANDIDATES ((size_t)2)

/* ---------------------------------------------------------------------------
 * Sets of bytes.
 * ------------------------------------------------------------------------- */

void tb_byte_set_add(tb_byte_set *set, const char *members, size_t length)
{
    for (size_t index = 0; index < length; index++)
    {
        set->has[(unsigned char)members[index]] = true;
    }
}

static bool tb_byte_set_has(const tb_byte_set *set, char byte)
{
    return set->has[(unsigned char)byte];
}

/* ---------------------------------------------------------------------------
 * Comparing.
 * ------------------------------------------------------------------------- */

int tb_bytes_compare(const char *left, size_t left_length, const char *right, size_t right_length)
{
    size_t shared_length = left_length;
    int order = {0};

    if (right_length < shared_length)
    {
        shared_length = right_length;
    }
    /* memcmp compares as unsigned bytes, which is the order wanted here. It
     * must not be given a null pointer, even for no bytes. */
    if (shared_length > 0)
    {
        order = memcmp(left, right, shared_length);
    }
    if (order != 0)
    {
        return order;
    }
    if (left_length < right_length)
    {
        return -1;
    }
    if (left_length > right_length)
    {
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Searching for one byte.
 * ------------------------------------------------------------------------- */

size_t tb_bytes_find_byte(const char *bytes, size_t begin, size_t end, char byte)
{
    const char *found = NULL;

    if (begin == end)
    {
        return TB_BYTES_NOT_FOUND;
    }
    found = memchr(bytes + begin, (unsigned char)byte, end - begin);
    if (found == NULL)
    {
        return TB_BYTES_NOT_FOUND;
    }
    return (size_t)(found - bytes);
}

/* Whether any of the eight bytes of a word is zero. Subtracting one from
 * every byte borrows into the top bit of exactly the bytes that were zero or
 * had that bit set already, and the second term drops the latter. */
static bool tb_bytes_has_zero_byte(uint64_t word)
{
    return ((word - TB_BYTES_LOW_BITS) & ~word & TB_BYTES_HIGH_BITS) != 0;
}

size_t tb_bytes_rfind_byte(const char *bytes, size_t begin, size_t end, char byte)
{
    /* The C library has no search from the end, so this one reads a word at
     * a time: a word with the byte in every place, exclusive-or'ed with eight
     * bytes of the text, has a zero byte exactly where the text has the byte.
     * Words without one are passed over whole. */
    const uint64_t pattern = TB_BYTES_LOW_BITS * (uint64_t)(unsigned char)byte;
    size_t index = end;

    while (index - begin >= sizeof(uint64_t))
    {
        uint64_t word = {0};

        memcpy(&word, bytes + index - sizeof(word), sizeof(word));
        if (tb_bytes_has_zero_byte(word ^ pattern))
        {
            break;
        }
        index -= sizeof(word);
    }
    /* The byte is in the word just before `index`, or fewer than eight bytes
     * are left: they are looked at one by one. */
    for (; index > begin; index--)
    {
        if (bytes[index - 1] == byte)
        {
            return index - 1;
        }
    }
    return TB_BYTES_NOT_FOUND;
}

/* ---------------------------------------------------------------------------
 * Searching for the bytes of a set.
 * ------------------------------------------------------------------------- */

size_t tb_bytes_find_any(const char *bytes, size_t begin, size_t end, const tb_byte_set *set)
{
    for (size_t index = begin; index < end; index++)
    {
        if (tb_byte_set_has(set, bytes[index]))
        {
            return index;
        }
    }
    return TB_BYTES_NOT_FOUND;
}

size_t tb_bytes_rfind_any(const char *bytes, size_t begin, size_t end, const tb_byte_set *set)
{
    for (size_t index = end; index > begin; index--)
    {
        if (tb_byte_set_has(set, bytes[index - 1]))
        {
            return index - 1;
        }
    }
    return TB_BYTES_NOT_FOUND;
}

size_t tb_bytes_span(const char *bytes, size_t begin, size_t end, const tb_byte_set *set)
{
    size_t index = begin;

    while (index < end && tb_byte_set_has(set, bytes[index]))
    {
        index++;
    }
    return index - begin;
}

size_t tb_bytes_rspan(const char *bytes, size_t begin, size_t end, const tb_byte_set *set)
{
    size_t index = end;

    while (index > begin && tb_byte_set_has(set, bytes[index - 1]))
    {
        index--;
    }
    return end - index;
}

/* ---------------------------------------------------------------------------
 * Searching for a pattern.
 *
 * The C library finds one byte quickly, so the search looks for one byte of
 * the pattern and compares the whole pattern wherever that byte is. Which
 * byte matters: a search for " = " that looked for the space would stop at
 * every word. So it looks for the byte of the pattern that is rarest in the
 * text an engine searches, and checks the second rarest before comparing.
 *
 * That is fast for ordinary text and patterns, and slow when the text is full
 * of places that nearly match. When too many candidates turn out false, the
 * search carries on with a rolling hash, whose time does not depend on what
 * the bytes are. Go's strings.Index and Rust's memchr crate search this way.
 * ------------------------------------------------------------------------- */

/* How common each byte value is in prose, source, paths, and data files. Only
 * the order matters, and a value that is not listed is rarer than every one
 * that is. A wrong guess costs speed, never correctness. */
static const unsigned char tb_bytes_commonness[256] = {
    [' '] = 255, ['e'] = 250, ['t'] = 245,  ['a'] = 240,  ['o'] = 235, ['i'] = 230, ['n'] = 225,
    ['s'] = 220, ['r'] = 215, ['h'] = 210,  ['l'] = 205,  ['d'] = 200, ['c'] = 195, ['u'] = 190,
    ['m'] = 185, ['f'] = 180, ['p'] = 175,  ['g'] = 170,  ['w'] = 165, ['y'] = 160, ['b'] = 155,
    ['v'] = 150, ['k'] = 145, ['\n'] = 140, ['0'] = 135,  ['_'] = 130, ['/'] = 125, ['.'] = 120,
    [','] = 118, ['1'] = 115, ['2'] = 112,  ['\0'] = 110, ['"'] = 108, ['x'] = 105, ['-'] = 102,
    ['('] = 100, [')'] = 99,  ['='] = 98,   [';'] = 96,   [':'] = 94,  ['\t'] = 92, ['3'] = 90,
    ['4'] = 88,  ['5'] = 86,  ['6'] = 84,   ['7'] = 82,   ['8'] = 80,  ['9'] = 78,  ['E'] = 70,
    ['T'] = 68,  ['A'] = 66,  ['S'] = 64,   ['I'] = 62,   ['R'] = 60,  ['N'] = 58,  ['O'] = 56,
    ['C'] = 54,  ['L'] = 52,  ['D'] = 50,   ['P'] = 48,   ['M'] = 46,  ['j'] = 40,  ['q'] = 38,
    ['z'] = 36,  ['\r'] = 34, ['{'] = 30,   ['}'] = 29,   ['['] = 28,  [']'] = 27,  ['*'] = 26,
    ['<'] = 24,  ['>'] = 23,  ['\''] = 22,  ['\\'] = 20,  [0xFF] = 18,
};

/* Index of the rarest byte of a pattern, leaving out the byte at `excluded`;
 * pass the pattern's length to leave none out. The first of equally rare
 * bytes is chosen. The pattern has a byte that is not left out. */
static size_t tb_bytes_rarest(const char *pattern, size_t pattern_length, size_t excluded)
{
    size_t rarest = (excluded == 0) ? 1 : 0;

    for (size_t index = rarest + 1; index < pattern_length; index++)
    {
        if (index != excluded && tb_bytes_commonness[(unsigned char)pattern[index]] <
                                     tb_bytes_commonness[(unsigned char)pattern[rarest]])
        {
            rarest = index;
        }
    }
    return rarest;
}

/* Finds a pattern with a rolling hash: every stretch of the pattern's length
 * is hashed in constant time from the one before it, and only a stretch with
 * the pattern's hash is compared. The range holds at least the pattern. */
static size_t tb_bytes_find_rolling(const char *bytes, size_t begin, size_t end,
                                    const char *pattern, size_t pattern_length)
{
    uint32_t wanted = 0;
    uint32_t hash = 0;
    /* The prime to the power of the length: what the byte that leaves the
     * stretch was multiplied by when it has been rolled over the whole of it. */
    uint32_t leaving_weight = 1;

    for (size_t index = 0; index < pattern_length; index++)
    {
        wanted = wanted * TB_BYTES_ROLLING_PRIME + (uint32_t)(unsigned char)pattern[index];
        hash = hash * TB_BYTES_ROLLING_PRIME + (uint32_t)(unsigned char)bytes[begin + index];
        leaving_weight *= TB_BYTES_ROLLING_PRIME;
    }
    for (size_t start = begin;; start++)
    {
        if (hash == wanted && memcmp(bytes + start, pattern, pattern_length) == 0)
        {
            return start;
        }
        if (start + pattern_length == end)
        {
            return TB_BYTES_NOT_FOUND;
        }
        hash = hash * TB_BYTES_ROLLING_PRIME +
               (uint32_t)(unsigned char)bytes[start + pattern_length] -
               leaving_weight * (uint32_t)(unsigned char)bytes[start];
    }
}

size_t tb_bytes_find(const char *bytes, size_t begin, size_t end, const char *pattern,
                     size_t pattern_length)
{
    size_t last_start = {0};
    size_t start = begin;
    size_t rare = {0};
    size_t check = {0};
    size_t false_candidates = {0};

    if (pattern_length == 0)
    {
        return begin;
    }
    if (pattern_length > end - begin)
    {
        return TB_BYTES_NOT_FOUND;
    }
    if (pattern_length == 1)
    {
        return tb_bytes_find_byte(bytes, begin, end, pattern[0]);
    }
    rare = tb_bytes_rarest(pattern, pattern_length, pattern_length);
    check = tb_bytes_rarest(pattern, pattern_length, rare);
    last_start = end - pattern_length;
    while (start <= last_start)
    {
        /* The rare byte of a match that starts at `start` or later is at
         * `start + rare` or later, and no later than its place in the last
         * match that fits. */
        const size_t found =
            tb_bytes_find_byte(bytes, start + rare, last_start + rare + 1, pattern[rare]);

        if (found == TB_BYTES_NOT_FOUND)
        {
            return TB_BYTES_NOT_FOUND;
        }
        start = found - rare;
        if (bytes[start + check] == pattern[check] &&
            memcmp(bytes + start, pattern, pattern_length) == 0)
        {
            return start;
        }
        start++;
        false_candidates++;
        if (false_candidates >
            TB_BYTES_FREE_FALSE_CANDIDATES + (start - begin) / TB_BYTES_FALSE_CANDIDATE_INTERVAL)
        {
            break;
        }
    }
    if (start > last_start)
    {
        return TB_BYTES_NOT_FOUND;
    }
    /* The rare byte is not rare here. */
    return tb_bytes_find_rolling(bytes, start, end, pattern, pattern_length);
}

size_t tb_bytes_rfind(const char *bytes, size_t begin, size_t end, const char *pattern,
                      size_t pattern_length)
{
    if (pattern_length == 0)
    {
        return end;
    }
    if (pattern_length > end - begin)
    {
        return TB_BYTES_NOT_FOUND;
    }
    for (size_t start = end - pattern_length + 1; start > begin; start--)
    {
        if (memcmp(bytes + start - 1, pattern, pattern_length) == 0)
        {
            return start - 1;
        }
    }
    return TB_BYTES_NOT_FOUND;
}
