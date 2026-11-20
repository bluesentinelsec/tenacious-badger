/* The bits live in 64-bit words, bit `index` in word `index / 64` at position
 * `index % 64`. The positions of the last word beyond the length are always
 * zero, so whole words can be counted and compared without masking. */
#include "badger/bit.h"

#include "badger/assert.h"
#include "exclusive.h"

#include <string.h>

#define TB_BIT_WORD_WIDTH ((size_t)64)
#define TB_BIT_ALL_ONES UINT64_MAX

struct tb_bit
{
    tb_allocator allocator; /* Supplies the one block that holds everything. */
    size_t length;          /* Number of bits. */
    size_t word_count;      /* Number of words that hold them. */
    uint64_t *words;        /* The words, which follow this structure in memory. */
    tb_exclusive exclusive; /* One thread at a time changes it. */
};

/* The words are placed right after the structure, so its size must leave
 * them aligned. */
TB_STATIC_ASSERT(sizeof(tb_bit) % _Alignof(uint64_t) == 0,
                 "the words of a bit vector must be aligned after its structure");

/* How ranges of bits are changed. */
typedef enum tb_bit_change
{
    TB_BIT_CHANGE_SET,
    TB_BIT_CHANGE_CLEAR,
    TB_BIT_CHANGE_FLIP
} tb_bit_change;

/* ---------------------------------------------------------------------------
 * Words.
 * ------------------------------------------------------------------------- */

/* Number of bits set in a word, by adding neighboring groups of bits in
 * parallel. */
static size_t tb_bit_count_word(uint64_t word)
{
    word = word - ((word >> 1) & UINT64_C(0x5555555555555555));
    word = (word & UINT64_C(0x3333333333333333)) + ((word >> 2) & UINT64_C(0x3333333333333333));
    word = (word + (word >> 4)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
    return (size_t)((word * UINT64_C(0x0101010101010101)) >> 56);
}

/* Position of the lowest set bit of a word that is not zero. */
static size_t tb_bit_lowest_position(uint64_t word)
{
    /* The lowest set bit alone is a power of two, so multiplying by it shifts
     * the constant left by the position. The constant is a de Bruijn
     * sequence: every run of six of its bits is different, so the six bits
     * that reach the top name the position, and the table turns them back
     * into it. One multiplication, where counting the bits below takes a
     * dozen operations. */
    static const unsigned char positions[64] = {
        0,  1,  48, 2,  57, 49, 28, 3,  61, 58, 50, 42, 38, 29, 17, 4,  62, 55, 59, 36, 53, 51,
        43, 22, 45, 39, 33, 30, 24, 18, 12, 5,  63, 47, 56, 27, 60, 41, 37, 16, 54, 35, 52, 21,
        44, 32, 23, 11, 46, 26, 40, 15, 34, 20, 31, 10, 25, 14, 19, 9,  13, 8,  7,  6};
    const uint64_t lowest = word & (~word + 1);

    return positions[(lowest * UINT64_C(0x03F79D71B4CB0A89)) >> 58];
}

/* A word with the positions from `position` upward set. */
static uint64_t tb_bit_mask_from(size_t position)
{
    return TB_BIT_ALL_ONES << position;
}

/* A word with the positions up to and including `position` set. */
static uint64_t tb_bit_mask_through(size_t position)
{
    return TB_BIT_ALL_ONES >> (TB_BIT_WORD_WIDTH - 1 - position);
}

static void tb_bit_change_word(uint64_t *word, uint64_t mask, tb_bit_change change)
{
    switch (change)
    {
    case TB_BIT_CHANGE_SET:
        *word |= mask;
        break;
    case TB_BIT_CHANGE_CLEAR:
        *word &= ~mask;
        break;
    case TB_BIT_CHANGE_FLIP:
        *word ^= mask;
        break;
    }
}

static void tb_bit_change_range(tb_bit *bits, size_t begin, size_t end, tb_bit_change change)
{
    size_t first_word = {0};
    size_t last_word = {0};
    uint64_t first_mask = {0};
    uint64_t last_mask = {0};

    TB_ASSERT(bits != NULL);
    TB_ASSERT_MSG(begin <= end && end <= bits->length,
                  "the range must be in order and within the vector");
    if (begin == end)
    {
        return;
    }
    first_word = begin / TB_BIT_WORD_WIDTH;
    last_word = (end - 1) / TB_BIT_WORD_WIDTH;
    first_mask = tb_bit_mask_from(begin % TB_BIT_WORD_WIDTH);
    last_mask = tb_bit_mask_through((end - 1) % TB_BIT_WORD_WIDTH);
    if (first_word == last_word)
    {
        tb_bit_change_word(&bits->words[first_word], first_mask & last_mask, change);
        return;
    }
    tb_bit_change_word(&bits->words[first_word], first_mask, change);
    for (size_t word = first_word + 1; word < last_word; word++)
    {
        tb_bit_change_word(&bits->words[word], TB_BIT_ALL_ONES, change);
    }
    tb_bit_change_word(&bits->words[last_word], last_mask, change);
}

/* ---------------------------------------------------------------------------
 * Creating and destroying.
 * ------------------------------------------------------------------------- */

/* Size in bytes of the block that holds a vector of `word_count` words. It
 * cannot overflow: the words hold one bit per unit of a length that fits in
 * size_t, so they take about an eighth of the largest size. */
static size_t tb_bit_block_size(size_t word_count)
{
    return sizeof(tb_bit) + word_count * sizeof(uint64_t);
}

tb_bit *tb_bit_create(const tb_allocator *allocator, size_t length)
{
    /* Rounded up without adding to the length, which could overflow. */
    const size_t word_count =
        length / TB_BIT_WORD_WIDTH + (length % TB_BIT_WORD_WIDTH != 0 ? 1 : 0);
    const size_t size = tb_bit_block_size(word_count);
    tb_bit *const bits = tb_allocator_calloc(allocator, 1, size, _Alignof(tb_bit));

    bits->allocator = tb_allocator_resolve(allocator);
    bits->length = length;
    bits->word_count = word_count;
    bits->words = (uint64_t *)(void *)(bits + 1);
    return bits;
}

tb_bit *tb_bit_copy(const tb_bit *bits, const tb_allocator *allocator)
{
    tb_bit *copy = NULL;

    TB_ASSERT(bits != NULL);
    copy = tb_bit_create(allocator, bits->length);
    if (bits->word_count > 0)
    {
        memcpy(copy->words, bits->words, bits->word_count * sizeof(uint64_t));
    }
    return copy;
}

void tb_bit_destroy(tb_bit *bits)
{
    tb_allocator allocator = {0};

    if (bits == NULL)
    {
        return;
    }
    /* No thread may be inside a bit set that is going away. */
    TB_EXCLUSIVE_ENTER(&bits->exclusive, "a bit set");
    allocator = bits->allocator;
    tb_allocator_free(&allocator, bits, tb_bit_block_size(bits->word_count), _Alignof(tb_bit));
}

/* ---------------------------------------------------------------------------
 * Single bits.
 * ------------------------------------------------------------------------- */

size_t tb_bit_length(const tb_bit *bits)
{
    TB_ASSERT(bits != NULL);
    return bits->length;
}

size_t tb_bit_count(const tb_bit *bits)
{
    size_t count = {0};

    TB_ASSERT(bits != NULL);
    for (size_t word = 0; word < bits->word_count; word++)
    {
        count += tb_bit_count_word(bits->words[word]);
    }
    return count;
}

bool tb_bit_get(const tb_bit *bits, size_t index)
{
    TB_ASSERT(bits != NULL);
    TB_ASSERT_MSG(index < bits->length, "index must be less than the length");
    return ((bits->words[index / TB_BIT_WORD_WIDTH] >> (index % TB_BIT_WORD_WIDTH)) & 1u) != 0;
}

/* What each function that changes the object does once inside its guard. */
static bool tb_bit_put_inside(tb_bit *bits, size_t index, bool value);
static void tb_bit_set_inside(tb_bit *bits, size_t begin, size_t end);
static void tb_bit_clear_inside(tb_bit *bits, size_t begin, size_t end);
static void tb_bit_flip_inside(tb_bit *bits, size_t begin, size_t end);
static void tb_bit_union_inside(tb_bit *bits, const tb_bit *other);
static void tb_bit_intersection_inside(tb_bit *bits, const tb_bit *other);
static void tb_bit_difference_inside(tb_bit *bits, const tb_bit *other);
static void tb_bit_symmetric_difference_inside(tb_bit *bits, const tb_bit *other);

static bool tb_bit_put_inside(tb_bit *bits, size_t index, bool value)
{
    const bool previous = tb_bit_get(bits, index);
    tb_bit_change change = TB_BIT_CHANGE_CLEAR;

    if (value)
    {
        change = TB_BIT_CHANGE_SET;
    }
    tb_bit_change_word(&bits->words[index / TB_BIT_WORD_WIDTH],
                       UINT64_C(1) << (index % TB_BIT_WORD_WIDTH), change);
    return previous;
}

bool tb_bit_put(tb_bit *bits, size_t index, bool value)
{
    bool result = false;
    TB_ASSERT(bits != NULL);
    TB_EXCLUSIVE_ENTER(&bits->exclusive, "a bit set");
    result = tb_bit_put_inside(bits, index, value);
    TB_EXCLUSIVE_LEAVE(&bits->exclusive);
    return result;
}

/* ---------------------------------------------------------------------------
 * Ranges of bits.
 * ------------------------------------------------------------------------- */

static void tb_bit_set_inside(tb_bit *bits, size_t begin, size_t end)
{
    tb_bit_change_range(bits, begin, end, TB_BIT_CHANGE_SET);
}

void tb_bit_set(tb_bit *bits, size_t begin, size_t end)
{
    TB_ASSERT(bits != NULL);
    TB_EXCLUSIVE_ENTER(&bits->exclusive, "a bit set");
    tb_bit_set_inside(bits, begin, end);
    TB_EXCLUSIVE_LEAVE(&bits->exclusive);
}

static void tb_bit_clear_inside(tb_bit *bits, size_t begin, size_t end)
{
    tb_bit_change_range(bits, begin, end, TB_BIT_CHANGE_CLEAR);
}

void tb_bit_clear(tb_bit *bits, size_t begin, size_t end)
{
    TB_ASSERT(bits != NULL);
    TB_EXCLUSIVE_ENTER(&bits->exclusive, "a bit set");
    tb_bit_clear_inside(bits, begin, end);
    TB_EXCLUSIVE_LEAVE(&bits->exclusive);
}

static void tb_bit_flip_inside(tb_bit *bits, size_t begin, size_t end)
{
    tb_bit_change_range(bits, begin, end, TB_BIT_CHANGE_FLIP);
}

void tb_bit_flip(tb_bit *bits, size_t begin, size_t end)
{
    TB_ASSERT(bits != NULL);
    TB_EXCLUSIVE_ENTER(&bits->exclusive, "a bit set");
    tb_bit_flip_inside(bits, begin, end);
    TB_EXCLUSIVE_LEAVE(&bits->exclusive);
}

/* ---------------------------------------------------------------------------
 * Searching.
 * ------------------------------------------------------------------------- */

/* Finds the first bit at or after `from` that is set, or, when `inverted`,
 * the first that is clear. */
static size_t tb_bit_find(const tb_bit *bits, size_t from, bool inverted)
{
    size_t word_index = {0};
    uint64_t word = {0};
    /* Hides the positions before `from` in the first word that is looked at. */
    uint64_t mask = {0};

    TB_ASSERT(bits != NULL);
    TB_ASSERT_MSG(from <= bits->length, "the search must start within the vector");
    word_index = from / TB_BIT_WORD_WIDTH;
    mask = tb_bit_mask_from(from % TB_BIT_WORD_WIDTH);
    while (word_index < bits->word_count)
    {
        size_t index = {0};

        word = bits->words[word_index];
        if (inverted)
        {
            word = ~word;
        }
        word &= mask;
        if (word == 0)
        {
            word_index++;
            mask = TB_BIT_ALL_ONES;
            continue;
        }
        index = word_index * TB_BIT_WORD_WIDTH + tb_bit_lowest_position(word);
        /* The unused positions of the last word read as clear bits. */
        if (index >= bits->length)
        {
            return TB_BIT_NOT_FOUND;
        }
        return index;
    }
    return TB_BIT_NOT_FOUND;
}

size_t tb_bit_find_set(const tb_bit *bits, size_t from)
{
    return tb_bit_find(bits, from, false);
}

size_t tb_bit_find_clear(const tb_bit *bits, size_t from)
{
    return tb_bit_find(bits, from, true);
}

bool tb_bit_next_set(const tb_bit *bits, tb_bit_cursor *cursor, size_t *index)
{
    TB_ASSERT(bits != NULL);
    TB_ASSERT(cursor != NULL);
    TB_ASSERT(index != NULL);
    /* The cursor holds the set bits of one word that have not been returned
     * yet. When it has none left it takes the next word that has any, which
     * passes over 64 clear bits at a step. The positions of the last word
     * beyond the length are always clear, so no bit is checked against it. */
    while (cursor->remaining == 0)
    {
        if (cursor->next_word >= bits->word_count)
        {
            return false;
        }
        cursor->remaining = bits->words[cursor->next_word];
        cursor->next_word++;
    }
    *index =
        (cursor->next_word - 1) * TB_BIT_WORD_WIDTH + tb_bit_lowest_position(cursor->remaining);
    /* Subtracting one clears the lowest set bit and sets the bits below it,
     * which were clear and are cleared again. */
    cursor->remaining &= cursor->remaining - 1;
    return true;
}

void tb_bit_visit(const tb_bit *bits, tb_bit_visitor *visitor, void *context)
{
    TB_ASSERT(bits != NULL);
    TB_ASSERT(visitor != NULL);
    for (size_t index = 0; index < bits->length; index++)
    {
        visitor(index, tb_bit_get(bits, index), context);
    }
}

/* ---------------------------------------------------------------------------
 * Two vectors of the same length.
 * ------------------------------------------------------------------------- */

static void tb_bit_require_pair(const tb_bit *left, const tb_bit *right)
{
    TB_ASSERT(left != NULL);
    TB_ASSERT(right != NULL);
    TB_ASSERT_MSG(left->length == right->length, "the vectors must have the same length");
}

bool tb_bit_is_subset(const tb_bit *left, const tb_bit *right)
{
    tb_bit_require_pair(left, right);
    for (size_t word = 0; word < left->word_count; word++)
    {
        if ((left->words[word] & ~right->words[word]) != 0)
        {
            return false;
        }
    }
    return true;
}

bool tb_bit_equal(const tb_bit *left, const tb_bit *right)
{
    tb_bit_require_pair(left, right);
    for (size_t word = 0; word < left->word_count; word++)
    {
        if (left->words[word] != right->words[word])
        {
            return false;
        }
    }
    return true;
}

bool tb_bit_is_proper_subset(const tb_bit *left, const tb_bit *right)
{
    return tb_bit_is_subset(left, right) && !tb_bit_equal(left, right);
}

/* The four functions below read the words and their number out of the
 * structures once, before the loop. A loop that reads them from the structures
 * on every pass must assume that storing a word may have changed them, and
 * cannot combine several words in one step. */
static void tb_bit_union_inside(tb_bit *bits, const tb_bit *other)
{
    uint64_t *words = NULL;
    const uint64_t *other_words = NULL;
    size_t word_count = {0};

    tb_bit_require_pair(bits, other);
    words = bits->words;
    other_words = other->words;
    word_count = bits->word_count;
    for (size_t word = 0; word < word_count; word++)
    {
        words[word] |= other_words[word];
    }
}

void tb_bit_union(tb_bit *bits, const tb_bit *other)
{
    tb_bit_require_pair(bits, other);
    TB_EXCLUSIVE_ENTER(&bits->exclusive, "a bit set");
    tb_bit_union_inside(bits, other);
    TB_EXCLUSIVE_LEAVE(&bits->exclusive);
}

static void tb_bit_intersection_inside(tb_bit *bits, const tb_bit *other)
{
    uint64_t *words = NULL;
    const uint64_t *other_words = NULL;
    size_t word_count = {0};

    tb_bit_require_pair(bits, other);
    words = bits->words;
    other_words = other->words;
    word_count = bits->word_count;
    for (size_t word = 0; word < word_count; word++)
    {
        words[word] &= other_words[word];
    }
}

void tb_bit_intersection(tb_bit *bits, const tb_bit *other)
{
    tb_bit_require_pair(bits, other);
    TB_EXCLUSIVE_ENTER(&bits->exclusive, "a bit set");
    tb_bit_intersection_inside(bits, other);
    TB_EXCLUSIVE_LEAVE(&bits->exclusive);
}

static void tb_bit_difference_inside(tb_bit *bits, const tb_bit *other)
{
    uint64_t *words = NULL;
    const uint64_t *other_words = NULL;
    size_t word_count = {0};

    tb_bit_require_pair(bits, other);
    words = bits->words;
    other_words = other->words;
    word_count = bits->word_count;
    for (size_t word = 0; word < word_count; word++)
    {
        words[word] &= ~other_words[word];
    }
}

void tb_bit_difference(tb_bit *bits, const tb_bit *other)
{
    tb_bit_require_pair(bits, other);
    TB_EXCLUSIVE_ENTER(&bits->exclusive, "a bit set");
    tb_bit_difference_inside(bits, other);
    TB_EXCLUSIVE_LEAVE(&bits->exclusive);
}

static void tb_bit_symmetric_difference_inside(tb_bit *bits, const tb_bit *other)
{
    uint64_t *words = NULL;
    const uint64_t *other_words = NULL;
    size_t word_count = {0};

    tb_bit_require_pair(bits, other);
    words = bits->words;
    other_words = other->words;
    word_count = bits->word_count;
    for (size_t word = 0; word < word_count; word++)
    {
        words[word] ^= other_words[word];
    }
}

void tb_bit_symmetric_difference(tb_bit *bits, const tb_bit *other)
{
    tb_bit_require_pair(bits, other);
    TB_EXCLUSIVE_ENTER(&bits->exclusive, "a bit set");
    tb_bit_symmetric_difference_inside(bits, other);
    TB_EXCLUSIVE_LEAVE(&bits->exclusive);
}
