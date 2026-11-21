/* Sorting with the engine's own algorithms, so that the order of equal
 * elements is the same on every platform.
 *
 * tb_sort() is introsort. Quicksort partitions around a median pivot, the
 * median of three elements, or of three such medians in a long range. When a
 * range has been partitioned more times than a well-split one would need,
 * the pivots are being chosen badly (input built to defeat the median, for
 * example) and the range is finished with heapsort, which is O(n log n)
 * whatever the input. Short ranges are finished with insertion sort, which is
 * faster there than either.
 *
 * tb_sort_stable() is merge sort. Each merge copies its left half to scratch
 * memory and merges back into the array, so the scratch memory is half the
 * array. Two halves that are already in order are not merged at all, which
 * makes a sorted array cost one comparison per element. */
#include "badger/sort.h"

#include "badger/assert.h"
#include "badger/size.h"

#include <stdint.h>
#include <string.h>

/* Ranges this short are insertion-sorted. Insertion sort is quadratic, but
 * below about this length it does fewer swaps than partitioning would set up. */
#define TB_SORT_INSERTION_LIMIT ((size_t)16)

/* Ranges longer than this take the median of three medians as their pivot,
 * which is much harder to make a poor choice. */
#define TB_SORT_NINTHER_LIMIT ((size_t)128)

/* Elements larger than this are swapped in pieces of this size. */
#define TB_SORT_SWAP_PIECE ((size_t)64)

/* The strictest alignment the scratch memory of a stable sort is given. */
#define TB_SORT_MOST_ALIGNMENT ((size_t)64)

typedef void tb_sort_swap_function(unsigned char *left, unsigned char *right, size_t size);
typedef void tb_sort_copy_function(unsigned char *to, const unsigned char *from, size_t size);

/* One sort's parameters, so that each step of it takes one argument for all
 * of them. The swap and copy are chosen once, by the size of an element:
 * moving 4, 8, or 16 bytes of known size compiles to a few instructions,
 * where moving a size known only at run time calls memcpy. */
typedef struct tb_sort_job
{
    size_t size;
    tb_sort_compare_function *compare;
    void *context;
    tb_sort_swap_function *swap;
    tb_sort_copy_function *copy;
} tb_sort_job;

static void tb_sort_swap_4(unsigned char *left, unsigned char *right, size_t size)
{
    uint32_t held = 0;

    (void)size;
    memcpy(&held, left, sizeof(held));
    memcpy(left, right, sizeof(held));
    memcpy(right, &held, sizeof(held));
}

static void tb_sort_swap_8(unsigned char *left, unsigned char *right, size_t size)
{
    uint64_t held = 0;

    (void)size;
    memcpy(&held, left, sizeof(held));
    memcpy(left, right, sizeof(held));
    memcpy(right, &held, sizeof(held));
}

static void tb_sort_swap_16(unsigned char *left, unsigned char *right, size_t size)
{
    uint64_t held[2] = {0};

    (void)size;
    memcpy(held, left, sizeof(held));
    memcpy(left, right, sizeof(held));
    memcpy(right, held, sizeof(held));
}

/* Any multiple of 8 bytes, a word at a time: most structures are. */
static void tb_sort_swap_words(unsigned char *left, unsigned char *right, size_t size)
{
    for (size_t offset = 0; offset < size; offset += sizeof(uint64_t))
    {
        uint64_t held = 0;

        memcpy(&held, left + offset, sizeof(held));
        memcpy(left + offset, right + offset, sizeof(held));
        memcpy(right + offset, &held, sizeof(held));
    }
}

static void tb_sort_swap_any(unsigned char *left, unsigned char *right, size_t size)
{
    unsigned char held[TB_SORT_SWAP_PIECE] = {0};

    while (size > 0)
    {
        size_t piece = TB_SORT_SWAP_PIECE;

        if (size < piece)
        {
            piece = size;
        }
        memcpy(held, left, piece);
        memcpy(left, right, piece);
        memcpy(right, held, piece);
        left += piece;
        right += piece;
        size -= piece;
    }
}

static void tb_sort_copy_4(unsigned char *to, const unsigned char *from, size_t size)
{
    (void)size;
    memcpy(to, from, 4);
}

static void tb_sort_copy_8(unsigned char *to, const unsigned char *from, size_t size)
{
    (void)size;
    memcpy(to, from, 8);
}

static void tb_sort_copy_16(unsigned char *to, const unsigned char *from, size_t size)
{
    (void)size;
    memcpy(to, from, 16);
}

static void tb_sort_copy_any(unsigned char *to, const unsigned char *from, size_t size)
{
    memcpy(to, from, size);
}

static tb_sort_job tb_sort_job_for(size_t size, tb_sort_compare_function *compare, void *context)
{
    tb_sort_job job = {
        .size = size,
        .compare = compare,
        .context = context,
        .swap = tb_sort_swap_any,
        .copy = tb_sort_copy_any,
    };

    if (size == 4)
    {
        job.swap = tb_sort_swap_4;
        job.copy = tb_sort_copy_4;
    }
    else if (size == 8)
    {
        job.swap = tb_sort_swap_8;
        job.copy = tb_sort_copy_8;
    }
    else if (size == 16)
    {
        job.swap = tb_sort_swap_16;
        job.copy = tb_sort_copy_16;
    }
    else if (size % sizeof(uint64_t) == 0 && size <= TB_SORT_SWAP_PIECE)
    {
        job.swap = tb_sort_swap_words;
    }
    return job;
}

/* The checks every function makes of its arguments. */
static void tb_sort_check_arguments(const void *elements, size_t count, size_t size,
                                    tb_sort_compare_function *compare)
{
    size_t total = 0;

    TB_ASSERT_MSG(elements != NULL || count == 0, "elements may be NULL only when count is 0");
    TB_ASSERT(size >= 1);
    TB_ASSERT(compare != NULL);
    TB_ASSERT_MSG(tb_size_multiply(count, size, &total), "count * size must not overflow");
    (void)total;
}

static unsigned char *tb_sort_at(const tb_sort_job *job, unsigned char *base, size_t index)
{
    return base + index * job->size;
}

static int tb_sort_order(const tb_sort_job *job, const unsigned char *left,
                         const unsigned char *right)
{
    return job->compare(left, right, job->context);
}

/* Sorts by moving each element back past the ones greater than it. Equal
 * elements never pass each other, so this is stable too, and the merge sort
 * uses it for its short ranges. */
static void tb_sort_insertion(const tb_sort_job *job, unsigned char *base, size_t count)
{
    for (size_t next = 1; next < count; next++)
    {
        size_t index = next;

        while (index > 0 && tb_sort_order(job, tb_sort_at(job, base, index - 1),
                                          tb_sort_at(job, base, index)) > 0)
        {
            job->swap(tb_sort_at(job, base, index - 1), tb_sort_at(job, base, index), job->size);
            index--;
        }
    }
}

/* Moves the element at `root` down the heap of the first `count` elements
 * until neither child is greater. */
static void tb_sort_sift_down(const tb_sort_job *job, unsigned char *base, size_t root,
                              size_t count)
{
    for (;;)
    {
        size_t child = 2 * root + 1;

        if (child >= count)
        {
            return;
        }
        if (child + 1 < count &&
            tb_sort_order(job, tb_sort_at(job, base, child), tb_sort_at(job, base, child + 1)) < 0)
        {
            child++;
        }
        if (tb_sort_order(job, tb_sort_at(job, base, root), tb_sort_at(job, base, child)) >= 0)
        {
            return;
        }
        job->swap(tb_sort_at(job, base, root), tb_sort_at(job, base, child), job->size);
        root = child;
    }
}

static void tb_sort_heap(const tb_sort_job *job, unsigned char *base, size_t count)
{
    for (size_t parent = count / 2; parent > 0; parent--)
    {
        tb_sort_sift_down(job, base, parent - 1, count);
    }
    for (size_t end = count - 1; end > 0; end--)
    {
        job->swap(base, tb_sort_at(job, base, end), job->size);
        tb_sort_sift_down(job, base, 0, end);
    }
}

/* The index of whichever of the three elements is between the other two. */
static size_t tb_sort_median(const tb_sort_job *job, unsigned char *base, size_t first,
                             size_t second, size_t third)
{
    const unsigned char *const a = tb_sort_at(job, base, first);
    const unsigned char *const b = tb_sort_at(job, base, second);
    const unsigned char *const c = tb_sort_at(job, base, third);

    if (tb_sort_order(job, a, b) < 0)
    {
        if (tb_sort_order(job, b, c) < 0)
        {
            return second;
        }
        if (tb_sort_order(job, a, c) < 0)
        {
            return third;
        }
        return first;
    }
    /* b <= a. */
    if (tb_sort_order(job, a, c) < 0)
    {
        return first;
    }
    /* b <= a and c <= a: the greater of b and c. */
    if (tb_sort_order(job, b, c) < 0)
    {
        return third;
    }
    return second;
}

static size_t tb_sort_choose_pivot(const tb_sort_job *job, unsigned char *base, size_t count)
{
    const size_t middle = count / 2;
    const size_t last = count - 1;

    if (count > TB_SORT_NINTHER_LIMIT)
    {
        const size_t step = count / 8;
        const size_t low = tb_sort_median(job, base, 0, step, 2 * step);
        const size_t mid = tb_sort_median(job, base, middle - step, middle, middle + step);
        const size_t high = tb_sort_median(job, base, last - 2 * step, last - step, last);

        return tb_sort_median(job, base, low, mid, high);
    }
    return tb_sort_median(job, base, 0, middle, last);
}

/* Partitions around the first element, the pivot, and returns where the
 * pivot ends: everything before it is no greater, everything after it no
 * less. Both scans stop at elements equal to the pivot and swap them, which
 * spreads a run of equal elements over both sides instead of piling it onto
 * one. A consistent comparison stops the scan from the right at the pivot
 * itself; both scans are bounded anyway, so that a comparison that is not
 * consistent cannot take either one outside the range. */
static size_t tb_sort_partition(const tb_sort_job *job, unsigned char *base, size_t count)
{
    size_t left = 1;
    size_t right = count - 1;

    for (;;)
    {
        while (left < count && tb_sort_order(job, tb_sort_at(job, base, left), base) < 0)
        {
            left++;
        }
        while (right > 0 && tb_sort_order(job, tb_sort_at(job, base, right), base) > 0)
        {
            right--;
        }
        if (left >= right)
        {
            break;
        }
        job->swap(tb_sort_at(job, base, left), tb_sort_at(job, base, right), job->size);
        left++;
        right--;
    }
    job->swap(base, tb_sort_at(job, base, right), job->size);
    return right;
}

/* Sorts the shorter side of each partition by recursion and loops on the
 * longer, so the recursion is never deeper than log2 of the count. */
static void tb_sort_intro(const tb_sort_job *job, unsigned char *base, size_t count,
                          size_t partitions_left)
{
    while (count > TB_SORT_INSERTION_LIMIT)
    {
        size_t pivot = 0;
        size_t before = 0;
        size_t after = 0;
        unsigned char *after_base = NULL;

        if (partitions_left == 0)
        {
            tb_sort_heap(job, base, count);
            return;
        }
        partitions_left--;
        pivot = tb_sort_choose_pivot(job, base, count);
        job->swap(base, tb_sort_at(job, base, pivot), job->size);
        before = tb_sort_partition(job, base, count);
        after = count - before - 1;
        after_base = tb_sort_at(job, base, before + 1);
        if (before < after)
        {
            tb_sort_intro(job, base, before, partitions_left);
            base = after_base;
            count = after;
        }
        else
        {
            tb_sort_intro(job, after_base, after, partitions_left);
            count = before;
        }
    }
    tb_sort_insertion(job, base, count);
}

/* Twice the number of halvings that bring `count` to 1: as many partitions as
 * a range may go through before heapsort takes over. */
static size_t tb_sort_partition_budget(size_t count)
{
    size_t halvings = 0;

    while (count > 1)
    {
        count /= 2;
        halvings++;
    }
    return 2 * halvings;
}

void tb_sort(void *elements, size_t count, size_t size, tb_sort_compare_function *compare,
             void *context)
{
    tb_sort_job job = {0};

    tb_sort_check_arguments(elements, count, size, compare);
    if (count < 2)
    {
        return;
    }
    job = tb_sort_job_for(size, compare, context);
    tb_sort_intro(&job, elements, count, tb_sort_partition_budget(count));
}

/* Merges the sorted ranges [0, middle) and [middle, count). The left range
 * moves to the scratch memory, and the merge writes from the start of the
 * array: it never overtakes the right range, whose elements it has not yet
 * read, because it has written fewer elements than it has taken from the
 * left. When the elements of the right range run out first, what is left of
 * the left range is copied after them; when the left range runs out first,
 * the rest of the right range is already in place. */
static void tb_sort_merge(const tb_sort_job *job, unsigned char *base, size_t middle, size_t count,
                          unsigned char *scratch)
{
    const unsigned char *left = scratch;
    const unsigned char *const left_end = scratch + middle * job->size;
    const unsigned char *right = tb_sort_at(job, base, middle);
    const unsigned char *const right_end = tb_sort_at(job, base, count);
    unsigned char *out = base;

    memcpy(scratch, base, middle * job->size);
    while (left < left_end && right < right_end)
    {
        /* Ties take the left element, which keeps equal elements in order. */
        if (tb_sort_order(job, right, left) < 0)
        {
            job->copy(out, right, job->size);
            right += job->size;
        }
        else
        {
            job->copy(out, left, job->size);
            left += job->size;
        }
        out += job->size;
    }
    memcpy(out, left, (size_t)(left_end - left));
}

static void tb_sort_merge_sort(const tb_sort_job *job, unsigned char *base, size_t count,
                               unsigned char *scratch)
{
    size_t middle = 0;

    if (count <= TB_SORT_INSERTION_LIMIT)
    {
        tb_sort_insertion(job, base, count);
        return;
    }
    middle = count / 2;
    tb_sort_merge_sort(job, base, middle, scratch);
    tb_sort_merge_sort(job, tb_sort_at(job, base, middle), count - middle, scratch);
    if (tb_sort_order(job, tb_sort_at(job, base, middle - 1), tb_sort_at(job, base, middle)) <= 0)
    {
        return;
    }
    tb_sort_merge(job, base, middle, count, scratch);
}

/* The largest power of two that divides `size`, up to the most the scratch
 * memory is given. The alignment of a C type divides its size, so this is
 * at least the alignment of the elements, and an element copied to the
 * scratch memory is aligned as it was in the array. */
static size_t tb_sort_scratch_alignment(size_t size)
{
    size_t alignment = size & (~size + 1);

    if (alignment > TB_SORT_MOST_ALIGNMENT)
    {
        alignment = TB_SORT_MOST_ALIGNMENT;
    }
    return alignment;
}

void tb_sort_stable(void *elements, size_t count, size_t size, tb_sort_compare_function *compare,
                    void *context, const tb_allocator *allocator)
{
    tb_sort_job job = {0};
    unsigned char *scratch = NULL;
    size_t scratch_size = 0;
    size_t alignment = 0;

    tb_sort_check_arguments(elements, count, size, compare);
    if (count < 2)
    {
        return;
    }
    job = tb_sort_job_for(size, compare, context);
    if (count <= TB_SORT_INSERTION_LIMIT)
    {
        tb_sort_insertion(&job, elements, count);
        return;
    }
    /* The longest left range any merge copies is the first half. */
    scratch_size = (count / 2) * size;
    alignment = tb_sort_scratch_alignment(size);
    scratch = tb_allocator_alloc(allocator, scratch_size, alignment);
    tb_sort_merge_sort(&job, elements, count, scratch);
    tb_allocator_free(allocator, scratch, scratch_size, alignment);
}

size_t tb_sort_lower_bound(const void *elements, size_t count, size_t size, const void *key,
                           tb_sort_compare_function *compare, void *context)
{
    const unsigned char *const base = elements;
    size_t low = 0;
    size_t high = count;

    tb_sort_check_arguments(elements, count, size, compare);
    TB_ASSERT(key != NULL);
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;

        if (compare(base + middle * size, key, context) < 0)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return low;
}
