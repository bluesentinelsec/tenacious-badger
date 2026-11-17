/* The checking memory functions promise three things, and the cases press on
 * each.
 *
 * They are memory functions: what they return can be used, resized, and
 * freed like any block. They count: the numbers they keep match a model
 * through thousands of random operations. And they find faults: every kind
 * is committed here on purpose, and must be reported as that kind, about
 * that block, without anything being read that was not allocated.
 *
 * The cases call the four functions directly. That checks them without
 * installing them in the engine, where the test program's own allocations
 * would be counted with the case's. One case installs them, to see that the
 * engine's allocation functions work on top of them.
 *
 * A fault reaches a handler that records the finding and traps, so a case
 * commits a fault inside TB_TEST_EXPECT_TRAP and then looks at what was
 * found. */
#include "badger/memory_debug.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/thread.h"
#include "engine_test.h"
#include "tb_test.h"

#include <stdint.h>
#include <string.h>

/* What the header says the functions fill memory with. */
#define FRESH_BYTE 0xCD
#define GUARD_SIZE ((size_t)16)

/* --- the handler --------------------------------------------------------------- */

typedef struct found
{
    tb_memory_debug_finding finding; /* The last one. */
    size_t count;                    /* How many there have been. */
} found;

static found last = {0};

static const char *name_of(tb_memory_debug_fault fault)
{
    switch (fault)
    {
    case TB_MEMORY_DEBUG_NOT_A_BLOCK:
        return "not a block";
    case TB_MEMORY_DEBUG_FREED_TWICE:
        return "freed twice";
    case TB_MEMORY_DEBUG_WRITTEN_BEFORE:
        return "written before";
    case TB_MEMORY_DEBUG_WRITTEN_AFTER:
        return "written after";
    case TB_MEMORY_DEBUG_WATCHED:
        return "watched";
    }
    return "a fault that the header does not name";
}

static void record_and_trap(const tb_memory_debug_finding *finding, void *context)
{
    found *const record = context;

    record->finding = *finding;
    record->count++;
    tb_test_trap(__FILE__, __LINE__, name_of(finding->fault));
}

/* --- an allocator to put under the checking functions ---------------------------- */

/* It hands out memory from one large block and never uses an address twice,
 * so a case knows that an address it freed is not a block again. It poisons
 * what is freed or moved away from, so that anything the checking functions
 * read there would be wrong. And it can be told to refuse. */
#define PLAIN_CAPACITY ((size_t)8 << 20)
#define PLAIN_ALIGNMENT ((size_t)16)
#define PLAIN_POISON 0xEE

typedef struct plain_allocator
{
    unsigned char *memory;   /* PLAIN_CAPACITY bytes from the C library. */
    size_t used;             /* Bytes handed out, headers included. */
    size_t refuse_at;        /* Requests of this many bytes or more fail; 0 for none. */
    size_t allocation_count; /* Blocks handed out. */
    size_t free_count;       /* Blocks taken back. */
} plain_allocator;

static plain_allocator plain = {0};
static tb_memory_functions original = {0};

/* The size of a block is kept in the PLAIN_ALIGNMENT bytes before it. */
static size_t plain_size_of(const void *memory)
{
    size_t size = 0;

    memcpy(&size, (const unsigned char *)memory - PLAIN_ALIGNMENT, sizeof(size));
    return size;
}

static void *plain_malloc(size_t size)
{
    const size_t rounded = (size + PLAIN_ALIGNMENT - 1) / PLAIN_ALIGNMENT * PLAIN_ALIGNMENT;
    unsigned char *block = NULL;

    if (plain.refuse_at != 0 && size >= plain.refuse_at)
    {
        return NULL;
    }
    if (rounded > PLAIN_CAPACITY - plain.used - PLAIN_ALIGNMENT)
    {
        return NULL;
    }
    block = plain.memory + plain.used + PLAIN_ALIGNMENT;
    memcpy(block - PLAIN_ALIGNMENT, &size, sizeof(size));
    plain.used += PLAIN_ALIGNMENT + rounded;
    plain.allocation_count++;
    return block;
}

static void *plain_calloc(size_t count, size_t size)
{
    void *const block = plain_malloc(count * size);

    if (block != NULL)
    {
        memset(block, 0, count * size);
    }
    return block;
}

static void plain_free(void *memory)
{
    memset(memory, PLAIN_POISON, plain_size_of(memory));
    plain.free_count++;
}

/* Always moves the block, which is the harder case for whoever keeps its
 * address. */
static void *plain_realloc(void *memory, size_t size)
{
    const size_t old_size = plain_size_of(memory);
    void *const block = plain_malloc(size);
    size_t kept = old_size;

    if (block == NULL)
    {
        return NULL;
    }
    if (size < kept)
    {
        kept = size;
    }
    memcpy(block, memory, kept);
    plain_free(memory);
    return block;
}

static const tb_memory_functions plain_functions = {.malloc_function = plain_malloc,
                                                    .calloc_function = plain_calloc,
                                                    .realloc_function = plain_realloc,
                                                    .free_function = plain_free};

/* --- starting and stopping -------------------------------------------------------- */

/* The checking functions, as a case calls them. */
static tb_memory_functions checking = {0};

/* Starts a case with nothing counted, on the engine's built-in functions. */
static void start_checking(void)
{
    if (tb_engine_test_is_checking_memory())
    {
        TB_TEST_SKIP("this run has the checking functions installed, and the case resets them");
    }
    memset(&last, 0, sizeof(last));
    tb_memory_debug_set_handler(record_and_trap, &last);
    (void)tb_memory_debug_set_label(NULL);
    (void)tb_memory_debug_set_site((tb_memory_debug_site){0});
    TB_TEST_REQUIRE(tb_memory_debug_set_underlying(NULL));
    TB_TEST_REQUIRE(tb_memory_debug_reset());
    tb_memory_debug_get_functions(&checking);
}

/* The same, on the allocator above. */
static void start_checking_on_plain(void)
{
    start_checking();
    tb_memory_get_original_functions(&original);
    memset(&plain, 0, sizeof(plain));
    plain.memory = original.malloc_function(PLAIN_CAPACITY);
    TB_TEST_REQUIRE_POINTER(plain.memory, !=, NULL);
    TB_TEST_REQUIRE(tb_memory_debug_set_underlying(&plain_functions));
}

/* Ends a case, which has freed every block. */
static void stop_checking(void)
{
    tb_memory_debug_statistics statistics = {0};

    tb_memory_debug_get_statistics(&statistics);
    TB_TEST_EXPECT_UINT(statistics.live_blocks, ==, 0);
    TB_TEST_EXPECT_UINT(statistics.live_bytes, ==, 0);
    TB_TEST_EXPECT(tb_memory_debug_set_underlying(NULL));
    TB_TEST_EXPECT(tb_memory_debug_reset());
    tb_memory_debug_set_handler(NULL, NULL);
    (void)tb_memory_debug_set_label(NULL);
    (void)tb_memory_debug_set_site((tb_memory_debug_site){0});
    if (plain.memory != NULL)
    {
        original.free_function(plain.memory);
        plain.memory = NULL;
    }
}

static void expect_counts(size_t live_blocks, size_t live_bytes, size_t peak_blocks,
                          size_t peak_bytes, uint64_t allocated_blocks)
{
    tb_memory_debug_statistics statistics = {0};

    tb_memory_debug_get_statistics(&statistics);
    TB_TEST_EXPECT_UINT(statistics.live_blocks, ==, live_blocks);
    TB_TEST_EXPECT_UINT(statistics.live_bytes, ==, live_bytes);
    TB_TEST_EXPECT_UINT(statistics.peak_blocks, ==, peak_blocks);
    TB_TEST_EXPECT_UINT(statistics.peak_bytes, ==, peak_bytes);
    TB_TEST_EXPECT_UINT(statistics.allocated_blocks, ==, allocated_blocks);
}

static bool every_byte_is(const unsigned char *bytes, size_t size, unsigned char value)
{
    for (size_t index = 0; index < size; index++)
    {
        if (bytes[index] != value)
        {
            return false;
        }
    }
    return true;
}

/* What a visit saw, up to a number of blocks. */
#define SEEN_CAPACITY 8

typedef struct seen
{
    tb_memory_debug_block blocks[SEEN_CAPACITY];
    size_t count;
} seen;

static void see_block(const tb_memory_debug_block *block, void *context)
{
    seen *const all = context;

    if (all->count < SEEN_CAPACITY)
    {
        all->blocks[all->count] = *block;
    }
    all->count++;
}

/* --- blocks -------------------------------------------------------------------- */

TB_TEST(core_memory_debug, a_block_can_be_used_like_any_other)
{
    unsigned char *block = NULL;
    unsigned char *zeroed = NULL;

    start_checking();
    block = checking.malloc_function(100);
    TB_TEST_REQUIRE_POINTER(block, !=, NULL);
    /* Aligned for any object type, as the underlying block is. */
    TB_TEST_EXPECT_UINT((uintptr_t)block % 16, ==, 0);
    /* New memory is filled, so that reading it before writing it shows. */
    TB_TEST_EXPECT(every_byte_is(block, 100, FRESH_BYTE));
    for (size_t index = 0; index < 100; index++)
    {
        block[index] = (unsigned char)index;
    }

    zeroed = checking.calloc_function(7, 9);
    TB_TEST_REQUIRE_POINTER(zeroed, !=, NULL);
    TB_TEST_EXPECT(every_byte_is(zeroed, 63, 0));

    /* Growing keeps what was there and fills what is new. */
    block = checking.realloc_function(block, 5000);
    TB_TEST_REQUIRE_POINTER(block, !=, NULL);
    for (size_t index = 0; index < 100; index++)
    {
        TB_TEST_REQUIRE_UINT(block[index], ==, (unsigned char)index);
    }
    TB_TEST_EXPECT(every_byte_is(block + 100, 4900, FRESH_BYTE));
    /* Shrinking keeps what still fits. */
    block = checking.realloc_function(block, 10);
    TB_TEST_REQUIRE_POINTER(block, !=, NULL);
    for (size_t index = 0; index < 10; index++)
    {
        TB_TEST_EXPECT_UINT(block[index], ==, (unsigned char)index);
    }
    /* Every block is whole: nothing above wrote outside one. */
    tb_memory_debug_check();
    checking.free_function(block);
    checking.free_function(zeroed);
    TB_TEST_EXPECT_UINT(last.count, ==, 0);
    stop_checking();
}

TB_TEST(core_memory_debug, a_block_of_no_bytes_is_a_block)
{
    void *nothing = NULL;
    void *other = NULL;

    start_checking();
    /* The engine never asks for none, and a direct caller may. */
    nothing = checking.malloc_function(0);
    other = checking.malloc_function(0);
    TB_TEST_REQUIRE_POINTER(nothing, !=, NULL);
    TB_TEST_EXPECT_POINTER(nothing, !=, other);
    expect_counts(2, 0, 2, 0, 2);
    nothing = checking.realloc_function(nothing, 40);
    TB_TEST_EXPECT(every_byte_is(nothing, 40, FRESH_BYTE));
    checking.free_function(nothing);
    checking.free_function(other);
    stop_checking();
}

/* --- counting ------------------------------------------------------------------- */

TB_TEST(core_memory_debug, blocks_and_bytes_are_counted_now_and_at_the_most)
{
    void *first = NULL;
    void *second = NULL;
    void *third = NULL;
    void *fourth = NULL;
    seen all = {0};

    start_checking();
    expect_counts(0, 0, 0, 0, 0);
    first = checking.malloc_function(10);
    second = checking.calloc_function(4, 25);
    third = checking.malloc_function(1000);
    expect_counts(3, 1110, 3, 1110, 3);

    /* Freeing lowers what is allocated now, and not the most there was. */
    checking.free_function(second);
    expect_counts(2, 1010, 3, 1110, 3);

    /* Resizing changes the bytes and allocates no new block. */
    third = checking.realloc_function(third, 5000);
    expect_counts(2, 5010, 3, 5010, 3);
    third = checking.realloc_function(third, 20);
    expect_counts(2, 30, 3, 5010, 3);

    fourth = checking.malloc_function(1);
    expect_counts(3, 31, 3, 5010, 4);

    /* The blocks, oldest first. A block keeps its number when it is resized. */
    tb_memory_debug_visit(see_block, &all);
    TB_TEST_REQUIRE_UINT(all.count, ==, 3);
    TB_TEST_EXPECT_POINTER(all.blocks[0].memory, ==, first);
    TB_TEST_EXPECT_UINT(all.blocks[0].size, ==, 10);
    TB_TEST_EXPECT_UINT(all.blocks[0].number, ==, 1);
    TB_TEST_EXPECT_POINTER(all.blocks[1].memory, ==, third);
    TB_TEST_EXPECT_UINT(all.blocks[1].size, ==, 20);
    TB_TEST_EXPECT_UINT(all.blocks[1].number, ==, 3);
    TB_TEST_EXPECT_POINTER(all.blocks[2].memory, ==, fourth);
    TB_TEST_EXPECT_UINT(all.blocks[2].size, ==, 1);
    TB_TEST_EXPECT_UINT(all.blocks[2].number, ==, 4);

    checking.free_function(first);
    checking.free_function(third);
    checking.free_function(fourth);
    expect_counts(0, 0, 3, 5010, 4);

    /* Resetting forgets all of it. */
    TB_TEST_EXPECT(tb_memory_debug_reset());
    expect_counts(0, 0, 0, 0, 0);
    first = checking.malloc_function(1);
    expect_counts(1, 1, 1, 1, 1);
    checking.free_function(first);
    stop_checking();
}

#define MODEL_SLOTS ((size_t)300)
#define MODEL_STEPS ((size_t)20000)
#define MODEL_SIZE_MAX ((uint64_t)600)

typedef struct model_block
{
    unsigned char *memory; /* NULL when the slot is empty. */
    size_t size;
    unsigned char fill; /* What every byte of it was set to. */
} model_block;

typedef struct model
{
    model_block blocks[MODEL_SLOTS];
    tb_memory_debug_statistics expected;
} model;

static void model_note_peaks(model *all)
{
    if (all->expected.live_blocks > all->expected.peak_blocks)
    {
        all->expected.peak_blocks = all->expected.live_blocks;
    }
    if (all->expected.live_bytes > all->expected.peak_bytes)
    {
        all->expected.peak_bytes = all->expected.live_bytes;
    }
}

static void model_fill(model_block *block)
{
    block->fill = (unsigned char)tb_test_random_below(256);
    memset(block->memory, block->fill, block->size);
}

/* One random operation on one slot: allocate when it is empty, and otherwise
 * check its bytes and free it or resize it. */
static void model_step(model *all)
{
    model_block *const block = &all->blocks[tb_test_random_below(MODEL_SLOTS)];
    const size_t size = (size_t)tb_test_random_below(MODEL_SIZE_MAX + 1);
    size_t kept = block->size;

    if (block->memory == NULL)
    {
        if (tb_test_random_below(2) == 0)
        {
            block->memory = checking.malloc_function(size);
        }
        else
        {
            block->memory = checking.calloc_function(size, 1);
        }
        TB_TEST_REQUIRE_POINTER(block->memory, !=, NULL);
        block->size = size;
        model_fill(block);
        all->expected.live_blocks++;
        all->expected.live_bytes += size;
        all->expected.allocated_blocks++;
        model_note_peaks(all);
        return;
    }
    if (!every_byte_is(block->memory, block->size, block->fill))
    {
        TB_TEST_FAIL("a block of %zu bytes lost what was written to it", block->size);
    }
    if (tb_test_random_below(3) == 0)
    {
        checking.free_function(block->memory);
        all->expected.live_blocks--;
        all->expected.live_bytes -= block->size;
        block->memory = NULL;
        return;
    }
    block->memory = checking.realloc_function(block->memory, size);
    TB_TEST_REQUIRE_POINTER(block->memory, !=, NULL);
    if (size < kept)
    {
        kept = size;
    }
    if (!every_byte_is(block->memory, kept, block->fill))
    {
        TB_TEST_FAIL("a block resized from %zu to %zu bytes lost its bytes", block->size, size);
    }
    all->expected.live_bytes -= block->size;
    all->expected.live_bytes += size;
    block->size = size;
    model_fill(block);
    model_note_peaks(all);
}

static bool are_the_same_counts(const tb_memory_debug_statistics *left,
                                const tb_memory_debug_statistics *right)
{
    return left->live_blocks == right->live_blocks && left->live_bytes == right->live_bytes &&
           left->peak_blocks == right->peak_blocks && left->peak_bytes == right->peak_bytes &&
           left->allocated_blocks == right->allocated_blocks;
}

static void count_block(const tb_memory_debug_block *block, void *context)
{
    size_t *const count = context;

    (void)block;
    (*count)++;
}

/* Twenty thousand random allocations, resizes, and frees of up to 300 blocks,
 * with a model keeping its own count beside the functions'. */
TB_TEST(core_memory_debug, the_counts_match_a_model_through_random_operations)
{
    static model all = {0};
    size_t visited = 0;

    start_checking();
    memset(&all, 0, sizeof(all));
    for (size_t step = 0; step < MODEL_STEPS; step++)
    {
        tb_memory_debug_statistics statistics = {0};

        model_step(&all);
        tb_memory_debug_get_statistics(&statistics);
        if (!are_the_same_counts(&statistics, &all.expected))
        {
            TB_TEST_FAIL("after step %zu the functions count %zu blocks and %zu bytes, "
                         "and the model %zu and %zu",
                         step, statistics.live_blocks, statistics.live_bytes,
                         all.expected.live_blocks, all.expected.live_bytes);
            break;
        }
        if (step % 1000 == 0)
        {
            tb_memory_debug_check();
        }
    }
    tb_memory_debug_visit(count_block, &visited);
    TB_TEST_EXPECT_UINT(visited, ==, all.expected.live_blocks);
    for (size_t index = 0; index < MODEL_SLOTS; index++)
    {
        if (all.blocks[index].memory != NULL)
        {
            checking.free_function(all.blocks[index].memory);
        }
    }
    TB_TEST_EXPECT_UINT(last.count, ==, 0);
    stop_checking();
}

#define MANY_BLOCKS ((size_t)5000)

/* More blocks than the first table of addresses has places for, so that the
 * table is made larger four times with blocks in it, and every block must
 * still be found when it is freed, in an order that is not the order of
 * allocation. */
TB_TEST(core_memory_debug, every_block_is_found_again_among_thousands)
{
    static unsigned char *blocks[MANY_BLOCKS] = {0};

    start_checking();
    for (size_t index = 0; index < MANY_BLOCKS; index++)
    {
        blocks[index] = checking.malloc_function(index % 40);
    }
    expect_counts(MANY_BLOCKS, 97500, MANY_BLOCKS, 97500, MANY_BLOCKS);
    tb_memory_debug_check();
    /* Every third block, then every other of the rest, then what is left. */
    for (size_t stride = 3; stride >= 1; stride--)
    {
        for (size_t index = 0; index < MANY_BLOCKS; index += stride)
        {
            if (blocks[index] != NULL)
            {
                checking.free_function(blocks[index]);
                blocks[index] = NULL;
            }
        }
    }
    TB_TEST_EXPECT_UINT(last.count, ==, 0);
    expect_counts(0, 0, MANY_BLOCKS, 97500, MANY_BLOCKS);
    stop_checking();
}

/* --- labels -------------------------------------------------------------------- */

typedef struct labelled
{
    const char *label_at_start; /* What the thread had before it set one. */
    void *block;                /* Allocated under the thread's own label. */
} labelled;

static void allocate_under_a_label_of_its_own(void *argument)
{
    labelled *const result = argument;

    result->label_at_start = tb_memory_debug_set_label("worker");
    result->block = checking.malloc_function(8);
}

TB_TEST(core_memory_debug, a_block_keeps_the_label_of_the_thread_that_allocated_it)
{
    labelled from_thread = {.label_at_start = "not yet known"};
    tb_thread thread = {0};
    void *render = NULL;
    void *audio = NULL;
    void *unlabelled = NULL;
    seen all = {0};

    start_checking();
    TB_TEST_EXPECT_POINTER(tb_memory_debug_set_label("render"), ==, NULL);
    render = checking.malloc_function(16);
    TB_TEST_EXPECT_STRING(tb_memory_debug_set_label("audio"), ==, "render");
    audio = checking.calloc_function(1, 32);

    /* Another thread starts with no label, and its label is its own. */
    tb_thread_create(&thread, allocate_under_a_label_of_its_own, &from_thread);
    tb_thread_join(&thread);
    TB_TEST_EXPECT_POINTER(from_thread.label_at_start, ==, NULL);

    /* Resized under another label, and by another thread than allocated it,
     * a block is still whose it was. */
    render = checking.realloc_function(render, 64);
    from_thread.block = checking.realloc_function(from_thread.block, 24);
    TB_TEST_EXPECT_STRING(tb_memory_debug_set_label(NULL), ==, "audio");
    unlabelled = checking.malloc_function(1);

    tb_memory_debug_visit(see_block, &all);
    TB_TEST_REQUIRE_UINT(all.count, ==, 4);
    TB_TEST_EXPECT_STRING(all.blocks[0].label, ==, "render");
    TB_TEST_EXPECT_STRING(all.blocks[1].label, ==, "audio");
    TB_TEST_EXPECT_STRING(all.blocks[2].label, ==, "worker");
    TB_TEST_EXPECT_POINTER(all.blocks[3].label, ==, NULL);

    checking.free_function(render);
    checking.free_function(audio);
    checking.free_function(from_thread.block);
    checking.free_function(unlabelled);
    stop_checking();
}

/* --- faults --------------------------------------------------------------------- */

/* What a body that commits a fault works on. */
typedef struct scene
{
    unsigned char *block;
    size_t size;
} scene;

static void free_it(void *context)
{
    const scene *const at = context;

    checking.free_function(at->block);
}

static void resize_it(void *context)
{
    const scene *const at = context;

    (void)checking.realloc_function(at->block, at->size);
}

static void check_every_block(void *context)
{
    (void)context;
    tb_memory_debug_check();
}

TB_TEST(core_memory_debug, freeing_a_block_twice_is_found)
{
    scene at = {.size = 24};
    void *other = NULL;

    start_checking();
    (void)tb_memory_debug_set_label("textures");
    other = checking.malloc_function(1);
    at.block = checking.malloc_function(at.size);
    checking.free_function(at.block);
    /* The address is looked up and never read through, so a sanitizer has
     * nothing to say about this. */
    TB_TEST_EXPECT_TRAP(free_it, &at, "freed twice");
    TB_TEST_EXPECT_UINT(last.count, ==, 1);
    TB_TEST_EXPECT_POINTER(last.finding.block.memory, ==, at.block);
    TB_TEST_EXPECT_UINT(last.finding.block.size, ==, 24);
    TB_TEST_EXPECT_UINT(last.finding.block.number, ==, 2);
    TB_TEST_EXPECT_STRING(last.finding.block.label, ==, "textures");
    TB_TEST_EXPECT_UINT(last.finding.distance, ==, 0);
    /* Resizing it is the same fault. */
    TB_TEST_EXPECT_TRAP(resize_it, &at, "freed twice");
    TB_TEST_EXPECT_UINT(last.count, ==, 2);
    /* Nothing was counted for either. */
    expect_counts(1, 1, 2, 25, 2);
    checking.free_function(other);
    stop_checking();
}

TB_TEST(core_memory_debug, a_pointer_that_is_no_block_is_found)
{
    unsigned char on_the_stack[32] = {0};
    scene at = {.size = 8};
    unsigned char *block = NULL;
    void *from_elsewhere = NULL;

    start_checking();
    tb_memory_get_original_functions(&original);
    block = checking.malloc_function(64);

    /* Memory that no allocator gave out. */
    at.block = on_the_stack;
    TB_TEST_EXPECT_TRAP(free_it, &at, "not a block");
    TB_TEST_EXPECT_POINTER(last.finding.block.memory, ==, on_the_stack);
    TB_TEST_EXPECT_UINT(last.finding.block.size, ==, 0);
    TB_TEST_EXPECT_UINT(last.finding.block.number, ==, 0);
    TB_TEST_EXPECT_POINTER(last.finding.block.label, ==, NULL);
    TB_TEST_EXPECT_TRAP(resize_it, &at, "not a block");

    /* A pointer into a block is not the block. */
    at.block = block + 8;
    TB_TEST_EXPECT_TRAP(free_it, &at, "not a block");
    TB_TEST_EXPECT_POINTER(last.finding.block.memory, ==, block + 8);

    /* A block of another allocator. */
    from_elsewhere = original.malloc_function(64);
    at.block = from_elsewhere;
    TB_TEST_EXPECT_TRAP(free_it, &at, "not a block");
    original.free_function(from_elsewhere);

    /* No pointer at all, which the engine never passes on. */
    at.block = NULL;
    TB_TEST_EXPECT_TRAP(free_it, &at, "not a block");
    TB_TEST_EXPECT_UINT(last.count, ==, 5);

    /* The block that was there through all of it is whole and still one. */
    expect_counts(1, 64, 1, 64, 1);
    checking.free_function(block);
    TB_TEST_EXPECT_UINT(last.count, ==, 5);
    stop_checking();
}

#define REMEMBERED ((size_t)1024)

/* A freed block is remembered for the next 1,024 frees, and after that
 * freeing it again is the freeing of a stranger. The allocator underneath
 * never gives an address out twice, so the address is not a block again. */
TB_TEST(core_memory_debug, a_block_freed_long_ago_is_no_longer_known)
{
    scene at = {.size = 8};

    start_checking_on_plain();
    at.block = checking.malloc_function(at.size);
    checking.free_function(at.block);
    for (size_t index = 0; index < REMEMBERED - 1; index++)
    {
        checking.free_function(checking.malloc_function(8));
    }
    /* It is the oldest that is remembered. */
    TB_TEST_EXPECT_TRAP(free_it, &at, "freed twice");
    TB_TEST_EXPECT_UINT(last.finding.block.number, ==, 1);
    checking.free_function(checking.malloc_function(8));
    TB_TEST_EXPECT_TRAP(free_it, &at, "not a block");
    TB_TEST_EXPECT_POINTER(last.finding.block.memory, ==, at.block);
    TB_TEST_EXPECT_UINT(last.finding.block.number, ==, 0);
    stop_checking();
}

/* One byte written outside a block, at each end of the 16 that are guarded
 * on each side, found when the block is freed, when it is resized, and when
 * every block is looked at. The byte is then put back, and the block must be
 * accepted again. */
TB_TEST(core_memory_debug, a_byte_written_outside_a_block_is_found)
{
    static const struct
    {
        bool is_before;
        size_t distance;
        const char *fault;
    } rows[] = {
        {true, 1, "written before"},
        {true, GUARD_SIZE, "written before"},
        {false, 1, "written after"},
        {false, GUARD_SIZE, "written after"},
    };
    scene at = {.size = 40};
    void *other = NULL;

    start_checking();
    other = checking.malloc_function(3);
    at.block = checking.malloc_function(at.size);
    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        unsigned char *byte = at.block + at.size + rows[row].distance - 1;
        unsigned char held = 0;

        TB_TEST_CONTEXT("%s, at distance %zu", rows[row].fault, rows[row].distance);
        if (rows[row].is_before)
        {
            byte = at.block - rows[row].distance;
        }
        held = *byte;
        *byte = (unsigned char)(held ^ 1);

        TB_TEST_EXPECT_TRAP(free_it, &at, rows[row].fault);
        TB_TEST_EXPECT_POINTER(last.finding.block.memory, ==, at.block);
        TB_TEST_EXPECT_UINT(last.finding.block.size, ==, 40);
        TB_TEST_EXPECT_UINT(last.finding.block.number, ==, 2);
        TB_TEST_EXPECT_UINT(last.finding.distance, ==, rows[row].distance);
        TB_TEST_EXPECT_TRAP(resize_it, &at, rows[row].fault);
        TB_TEST_EXPECT_TRAP(check_every_block, NULL, rows[row].fault);
        TB_TEST_EXPECT_UINT(last.finding.distance, ==, rows[row].distance);
        /* The block was neither freed nor resized. */
        expect_counts(2, 43, 2, 43, 2);

        *byte = held;
        tb_memory_debug_check();
    }
    TB_TEST_EXPECT_UINT(last.count, ==, 3 * TB_TEST_COUNT_OF(rows));
    checking.free_function(at.block);
    checking.free_function(other);
    stop_checking();
}

/* Of several written bytes the nearest to the block is the one reported, and
 * of several damaged blocks the oldest. */
TB_TEST(core_memory_debug, the_nearest_byte_and_the_oldest_block_are_reported)
{
    unsigned char held_before[GUARD_SIZE] = {0};
    unsigned char held_after[GUARD_SIZE] = {0};
    unsigned char *older = NULL;
    unsigned char *newer = NULL;
    unsigned char held = 0;

    start_checking();
    older = checking.malloc_function(10);
    newer = checking.malloc_function(20);
    memcpy(held_before, older - GUARD_SIZE, GUARD_SIZE);
    memcpy(held_after, older + 10, GUARD_SIZE);
    held = newer[20];
    newer[20] = (unsigned char)(held ^ 1);
    older[10 + 2] = (unsigned char)(held ^ 1);
    older[10 + 7] = (unsigned char)(held ^ 1);
    TB_TEST_EXPECT_TRAP(check_every_block, NULL, "written after");
    TB_TEST_EXPECT_POINTER(last.finding.block.memory, ==, older);
    TB_TEST_EXPECT_UINT(last.finding.distance, ==, 3);
    /* The bytes before a block are looked at first. */
    *(older - 5) = (unsigned char)(held ^ 1);
    TB_TEST_EXPECT_TRAP(check_every_block, NULL, "written before");
    TB_TEST_EXPECT_POINTER(last.finding.block.memory, ==, older);
    TB_TEST_EXPECT_UINT(last.finding.distance, ==, 5);

    /* With the older block put right, the newer one is the one at fault. */
    memcpy(older - GUARD_SIZE, held_before, GUARD_SIZE);
    memcpy(older + 10, held_after, GUARD_SIZE);
    TB_TEST_EXPECT_TRAP(check_every_block, NULL, "written after");
    TB_TEST_EXPECT_POINTER(last.finding.block.memory, ==, newer);
    TB_TEST_EXPECT_UINT(last.finding.distance, ==, 1);
    newer[20] = held;
    checking.free_function(older);
    checking.free_function(newer);
    TB_TEST_EXPECT_UINT(last.count, ==, 3);
    stop_checking();
}

static void allocate_one(void *context)
{
    (void)context;
    (void)checking.malloc_function(77);
}

TB_TEST(core_memory_debug, a_watched_block_is_reported_as_it_is_allocated)
{
    void *first = NULL;
    void *second = NULL;
    void *fourth = NULL;

    start_checking();
    (void)tb_memory_debug_set_label("levels");
    tb_memory_debug_watch(3);
    first = checking.malloc_function(1);
    second = checking.calloc_function(1, 2);
    TB_TEST_EXPECT_UINT(last.count, ==, 0);
    TB_TEST_EXPECT_TRAP(allocate_one, NULL, "watched");
    TB_TEST_EXPECT_UINT(last.finding.block.number, ==, 3);
    TB_TEST_EXPECT_UINT(last.finding.block.size, ==, 77);
    TB_TEST_EXPECT_STRING(last.finding.block.label, ==, "levels");
    /* The block is allocated: the report came from inside the call. */
    expect_counts(3, 80, 3, 80, 3);
    checking.free_function((void *)(uintptr_t)last.finding.block.memory);

    /* Only that block, and none once the watch is taken off. */
    fourth = checking.malloc_function(1);
    tb_memory_debug_watch(5);
    tb_memory_debug_watch(0);
    checking.free_function(checking.malloc_function(1));
    TB_TEST_EXPECT_UINT(last.count, ==, 1);
    checking.free_function(first);
    checking.free_function(second);
    checking.free_function(fourth);
    stop_checking();
}

/* --- the allocator underneath ---------------------------------------------------- */

TB_TEST(core_memory_debug, the_functions_allocate_with_the_allocator_they_are_given)
{
    unsigned char *block = NULL;
    void *other = NULL;
    seen all = {0};

    start_checking_on_plain();
    block = checking.malloc_function(100);
    TB_TEST_REQUIRE_POINTER(block, !=, NULL);
    TB_TEST_EXPECT(block > plain.memory && block < plain.memory + PLAIN_CAPACITY);
    memset(block, 7, 100);
    /* This allocator moves every block that is resized, and poisons where
     * it was: the functions must follow the block and not look back. */
    block = checking.realloc_function(block, 200);
    TB_TEST_EXPECT(every_byte_is(block, 100, 7));
    other = checking.malloc_function(1);
    block = checking.realloc_function(block, 50);
    TB_TEST_EXPECT(every_byte_is(block, 50, 7));
    tb_memory_debug_check();
    /* A block that moved keeps its place among the others. */
    tb_memory_debug_visit(see_block, &all);
    TB_TEST_REQUIRE_UINT(all.count, ==, 2);
    TB_TEST_EXPECT_POINTER(all.blocks[0].memory, ==, block);
    TB_TEST_EXPECT_UINT(all.blocks[0].size, ==, 50);
    TB_TEST_EXPECT_POINTER(all.blocks[1].memory, ==, other);
    checking.free_function(other);

    /* It cannot be changed while it holds a block, and neither can the
     * counts be forgotten. */
    TB_TEST_EXPECT(!tb_memory_debug_set_underlying(NULL));
    TB_TEST_EXPECT(!tb_memory_debug_reset());
    checking.free_function(block);
    /* Every block went back to it, and the table that the functions keep
     * goes back when they are given another allocator. */
    TB_TEST_EXPECT_UINT(plain.free_count + 1, ==, plain.allocation_count);
    TB_TEST_EXPECT(tb_memory_debug_set_underlying(NULL));
    TB_TEST_EXPECT_UINT(plain.free_count, ==, plain.allocation_count);
    stop_checking();
}

TB_TEST(core_memory_debug, an_allocator_with_a_function_missing_is_refused)
{
    start_checking();
    for (size_t missing = 0; missing < 4; missing++)
    {
        tb_memory_functions incomplete = plain_functions;

        TB_TEST_CONTEXT("member %zu", missing);
        switch (missing)
        {
        case 0:
            incomplete.malloc_function = NULL;
            break;
        case 1:
            incomplete.calloc_function = NULL;
            break;
        case 2:
            incomplete.realloc_function = NULL;
            break;
        default:
            incomplete.free_function = NULL;
            break;
        }
        TB_TEST_EXPECT(!tb_memory_debug_set_underlying(&incomplete));
    }
    /* The functions still allocate with what they had. */
    checking.free_function(checking.malloc_function(10));
    TB_TEST_EXPECT_UINT(last.count, ==, 0);
    stop_checking();
}

TB_TEST(core_memory_debug, an_allocation_that_cannot_be_made_changes_nothing)
{
    unsigned char *block = NULL;

    start_checking_on_plain();
    /* With no memory at all there is not even a table of addresses. */
    plain.refuse_at = 1;
    TB_TEST_EXPECT_POINTER(checking.malloc_function(10), ==, NULL);
    TB_TEST_EXPECT_POINTER(checking.calloc_function(2, 5), ==, NULL);
    expect_counts(0, 0, 0, 0, 0);

    plain.refuse_at = 0;
    block = checking.malloc_function(100);
    memset(block, 9, 100);
    expect_counts(1, 100, 1, 100, 1);

    /* Requests that are too large for the allocator, and requests whose size
     * cannot be expressed. */
    plain.refuse_at = 4096;
    TB_TEST_EXPECT_POINTER(checking.malloc_function(5000), ==, NULL);
    TB_TEST_EXPECT_POINTER(checking.calloc_function(5000, 1), ==, NULL);
    TB_TEST_EXPECT_POINTER(checking.malloc_function(SIZE_MAX), ==, NULL);
    TB_TEST_EXPECT_POINTER(checking.malloc_function(SIZE_MAX - 64), ==, NULL);
    TB_TEST_EXPECT_POINTER(checking.calloc_function(SIZE_MAX / 2, 3), ==, NULL);
    /* A block that cannot be resized is left as it was. */
    TB_TEST_EXPECT_POINTER(checking.realloc_function(block, 5000), ==, NULL);
    TB_TEST_EXPECT_POINTER(checking.realloc_function(block, SIZE_MAX), ==, NULL);
    expect_counts(1, 100, 1, 100, 1);
    TB_TEST_EXPECT(every_byte_is(block, 100, 9));
    tb_memory_debug_check();

    /* And it is still a block: it can be resized and freed. */
    block = checking.realloc_function(block, 300);
    TB_TEST_REQUIRE_POINTER(block, !=, NULL);
    TB_TEST_EXPECT(every_byte_is(block, 100, 9));
    checking.free_function(block);
    TB_TEST_EXPECT_UINT(last.count, ==, 0);
    expect_counts(0, 0, 1, 300, 1);
    stop_checking();
}

#define BLOCKS_PAST_THE_TABLE ((size_t)3000)

/* The table of addresses grows when there are more blocks than it has
 * places. When it cannot grow, the functions carry on with the table they
 * have. Here the allocator refuses anything as large as the second table. */
TB_TEST(core_memory_debug, blocks_are_found_when_the_table_of_addresses_cannot_grow)
{
    static void *blocks[BLOCKS_PAST_THE_TABLE] = {0};

    start_checking_on_plain();
    plain.refuse_at = 2048 * sizeof(void *);
    for (size_t index = 0; index < BLOCKS_PAST_THE_TABLE; index++)
    {
        blocks[index] = checking.malloc_function(8);
        TB_TEST_REQUIRE_POINTER(blocks[index], !=, NULL);
    }
    expect_counts(BLOCKS_PAST_THE_TABLE, 8 * BLOCKS_PAST_THE_TABLE, BLOCKS_PAST_THE_TABLE,
                  8 * BLOCKS_PAST_THE_TABLE, BLOCKS_PAST_THE_TABLE);
    for (size_t index = 0; index < BLOCKS_PAST_THE_TABLE; index += 2)
    {
        checking.free_function(blocks[index]);
    }
    for (size_t index = 1; index < BLOCKS_PAST_THE_TABLE; index += 2)
    {
        checking.free_function(blocks[index]);
    }
    TB_TEST_EXPECT_UINT(last.count, ==, 0);
    stop_checking();
}

/* --- threads --------------------------------------------------------------------- */

#define THREAD_COUNT 8
#define BLOCKS_PER_THREAD ((size_t)2000)
#define HELD_PER_THREAD ((size_t)16)

static void allocate_resize_and_free(void *argument)
{
    unsigned char *held[HELD_PER_THREAD] = {0};
    tb_platform_atomic_size *const wrong_count = argument;

    (void)tb_memory_debug_set_label("thread");
    for (size_t index = 0; index < BLOCKS_PER_THREAD; index++)
    {
        const size_t slot = index % HELD_PER_THREAD;
        const unsigned char fill = (unsigned char)index;

        if (held[slot] != NULL)
        {
            /* What this thread wrote is what it reads, whatever the others do. */
            held[slot] = checking.realloc_function(held[slot], 96);
            if (!every_byte_is(held[slot], 48, (unsigned char)(index - HELD_PER_THREAD)))
            {
                (void)tb_platform_atomic_size_add(wrong_count, 1);
            }
            checking.free_function(held[slot]);
        }
        held[slot] = checking.malloc_function(48);
        memset(held[slot], fill, 48);
        if (index % 256 == 0)
        {
            tb_memory_debug_check();
        }
    }
    for (size_t slot = 0; slot < HELD_PER_THREAD; slot++)
    {
        checking.free_function(held[slot]);
    }
}

TB_TEST(core_memory_debug, threads_allocate_and_free_at_the_same_time)
{
    tb_platform_atomic_size wrong_count = {0};
    tb_thread threads[THREAD_COUNT] = {0};

    start_checking();
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_create(&threads[index], allocate_resize_and_free, &wrong_count);
    }
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&wrong_count), ==, 0);
    TB_TEST_EXPECT_UINT(last.count, ==, 0);
    {
        tb_memory_debug_statistics statistics = {0};

        tb_memory_debug_get_statistics(&statistics);
        TB_TEST_EXPECT_UINT(statistics.allocated_blocks, ==, THREAD_COUNT * BLOCKS_PER_THREAD);
        TB_TEST_EXPECT_UINT(statistics.peak_blocks, <=, THREAD_COUNT * HELD_PER_THREAD);
        TB_TEST_EXPECT_UINT(statistics.peak_blocks, >=, HELD_PER_THREAD);
    }
    stop_checking();
}

/* --- installed in the engine ------------------------------------------------------ */

/* The engine's own allocation functions, on top of the checking functions:
 * every block the engine allocates is one that they count, aligned blocks
 * included, which the engine makes out of larger ones. */
TB_TEST(core_memory_debug, the_engine_allocates_through_the_functions_when_they_are_installed)
{
    unsigned char *plain_block = NULL;
    unsigned char *zeroed = NULL;
    unsigned char *aligned = NULL;

    start_checking();
    TB_TEST_REQUIRE(tb_memory_set_functions(&checking));
    (void)tb_memory_debug_set_label("engine");

    plain_block = tb_malloc(100);
    zeroed = tb_calloc(10, 10);
    aligned = tb_aligned_alloc(256, 1000);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, 3);
    TB_TEST_EXPECT(every_byte_is(plain_block, 100, FRESH_BYTE));
    TB_TEST_EXPECT(every_byte_is(zeroed, 100, 0));
    TB_TEST_EXPECT_UINT((uintptr_t)aligned % 256, ==, 0);
    memset(aligned, 1, 1000);
    plain_block = tb_realloc(plain_block, 4000);
    TB_TEST_EXPECT(every_byte_is(plain_block, 100, FRESH_BYTE));
    {
        tb_memory_debug_statistics statistics = {0};
        seen all = {0};

        tb_memory_debug_get_statistics(&statistics);
        TB_TEST_EXPECT_UINT(statistics.live_blocks, ==, 3);
        TB_TEST_EXPECT_UINT(statistics.live_bytes, >=, 4000 + 100 + 1000);
        tb_memory_debug_visit(see_block, &all);
        TB_TEST_REQUIRE_UINT(all.count, ==, 3);
        TB_TEST_EXPECT_POINTER(all.blocks[0].memory, ==, plain_block);
        TB_TEST_EXPECT_STRING(all.blocks[0].label, ==, "engine");
    }
    tb_memory_debug_check();
    tb_free(plain_block);
    tb_free(zeroed);
    tb_aligned_free(aligned);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, 0);
    TB_TEST_EXPECT_UINT(last.count, ==, 0);

    tb_engine_test_install_memory_functions();
    stop_checking();
}

/* --- contracts ------------------------------------------------------------------- */

static void get_functions_into_null(void *context)
{
    (void)context;
    tb_memory_debug_get_functions(NULL);
}

static void get_statistics_into_null(void *context)
{
    (void)context;
    tb_memory_debug_get_statistics(NULL);
}

static void visit_with_null(void *context)
{
    (void)context;
    tb_memory_debug_visit(NULL, NULL);
}

TB_TEST(core_memory_debug, missing_arguments_violate_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(get_functions_into_null, NULL, "functions != NULL");
    TB_TEST_EXPECT_TRAP(get_statistics_into_null, NULL, "statistics != NULL");
    TB_TEST_EXPECT_TRAP(visit_with_null, NULL, "visitor != NULL");
}

/* Locations describe the original allocation even after a moving resize. */
TB_TEST(core_memory_debug, source_locations_survive_resize_and_are_thread_local)
{
    tb_memory_debug_site previous = {0};
    seen all = {0};
    tb_thread thread = {0};
    labelled worker = {0};
    void *block = NULL;

    start_checking();
    previous = tb_memory_debug_set_site((tb_memory_debug_site){"allocation.c", 42});
    TB_TEST_EXPECT_POINTER(previous.file, ==, NULL);
    block = checking.malloc_function(16);
    (void)tb_memory_debug_set_site(previous);
    block = checking.realloc_function(block, 32);
    tb_thread_create(&thread, allocate_under_a_label_of_its_own, &worker);
    tb_thread_join(&thread);
    tb_memory_debug_visit(see_block, &all);
    TB_TEST_REQUIRE_UINT(all.count, ==, 2);
    TB_TEST_EXPECT_STRING(all.blocks[0].file, ==, "allocation.c");
    TB_TEST_EXPECT_UINT(all.blocks[0].line, ==, 42);
    TB_TEST_EXPECT_POINTER(all.blocks[1].file, ==, NULL);
    TB_TEST_EXPECT_UINT(all.blocks[1].line, ==, 0);
    checking.free_function(block);
    checking.free_function(worker.block);
    stop_checking();
}

TB_TEST(core_memory_debug, checking_functions_cannot_be_their_own_allocator)
{
    tb_memory_functions recursive = {0};

    start_checking();
    TB_TEST_EXPECT(!tb_memory_debug_set_underlying(&checking));
    for (size_t index = 0; index < 4; index++)
    {
        tb_memory_get_original_functions(&recursive);
        switch (index)
        {
        case 0:
            recursive.malloc_function = checking.malloc_function;
            break;
        case 1:
            recursive.calloc_function = checking.calloc_function;
            break;
        case 2:
            recursive.realloc_function = checking.realloc_function;
            break;
        case 3:
            recursive.free_function = checking.free_function;
            break;
        default:
            break;
        }
        TB_TEST_EXPECT(!tb_memory_debug_set_underlying(&recursive));
    }
    checking.free_function(checking.malloc_function(1));
    stop_checking();
}

static void set_invalid_site(void *context)
{
    (void)context;
    (void)tb_memory_debug_set_site((tb_memory_debug_site){NULL, 1});
}

TB_TEST(core_memory_debug, a_source_line_requires_a_file)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(set_invalid_site, NULL, "site.file != NULL || site.line == 0");
}
