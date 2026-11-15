/** @file test_allocator.h
 * @brief An allocator for tests that checks how the code under test uses it.
 *
 * Every module that takes a tb_allocator promises to get all of its memory
 * from it, to say truthfully how large each block is when it resizes or
 * releases one, and to release everything. A test proves that by giving the
 * module this allocator and looking at what it recorded:
 *
 * @code
 * tb_test_allocator tracker = {0};
 * const tb_allocator allocator = tb_test_allocator_start(&tracker);
 *
 * use_the_module(&allocator);
 * TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
 * TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
 * @endcode
 *
 * It moves a block every time the block is resized, so a pointer into the old
 * block that the module kept is caught by AddressSanitizer. Its memory comes
 * from the engine's allocation functions, so the leak check that runs after
 * every case covers it as well.
 *
 * @par Threading
 * Stage-owned: use an allocator from the thread that runs the test case.
 */
#ifndef TB_TEST_ALLOCATOR_H
#define TB_TEST_ALLOCATOR_H

#include "badger/allocator.h"

#include <stdbool.h>
#include <stddef.h>

/** @brief Blocks one test allocator can have live at once. */
#define TB_TEST_ALLOCATOR_BLOCK_LIMIT 1024

/** @brief A block the allocator handed out and has not been given back. */
typedef struct tb_test_allocator_block
{
    void *memory;     /**< Address handed out. */
    size_t size;      /**< Size it was last given. */
    size_t alignment; /**< Alignment it was allocated with. */
} tb_test_allocator_block;

/**
 * @brief State of one test allocator. Declare it zero-initialized, start it
 * with tb_test_allocator_start(), and read the members afterwards.
 */
typedef struct tb_test_allocator
{
    tb_test_allocator_block blocks[TB_TEST_ALLOCATOR_BLOCK_LIMIT]; /**< The live blocks. */
    size_t live_block_count;   /**< Blocks handed out and not yet released. */
    size_t live_byte_count;    /**< Sum of the sizes of the live blocks. */
    size_t peak_byte_count;    /**< Largest value live_byte_count has had. */
    size_t request_count;      /**< Requests for a new block or a resize, granted or not. */
    size_t allocation_count;   /**< Successful requests for a new block. */
    size_t reallocation_count; /**< Successful requests to resize a block. */
    size_t release_count;      /**< Requests to release a block. */
    size_t refusal_count;      /**< Requests refused by failure injection. */
    size_t violation_count;    /**< Calls that broke the allocator contract. */
    /** What the first such call did wrong; "none" while violation_count is 0. */
    const char *first_violation;
    bool is_failing;       /**< Failure injection is on. */
    size_t successes_left; /**< Requests that still succeed while it is on. */
} tb_test_allocator;

/**
 * @brief Reset a test allocator and get the tb_allocator that uses it.
 * @param tracker Required non-null; must outlive every use of the result.
 * @return An allocator to pass, by address, to the code under test.
 */
tb_allocator tb_test_allocator_start(tb_test_allocator *tracker);

/**
 * @brief Make the allocator start refusing, to test how code copes.
 *
 * Counts every request for a new block and every request to resize one.
 * Releases always succeed.
 *
 * @param tracker Required non-null.
 * @param successes Number of further requests that still succeed.
 */
void tb_test_allocator_fail_after(tb_test_allocator *tracker, size_t successes);

/** @brief Let requests succeed again. */
void tb_test_allocator_stop_failing(tb_test_allocator *tracker);

/**
 * @brief Whether an address range lies inside one live block of the allocator.
 * @param tracker Required non-null.
 * @param memory First byte of the range.
 * @param size Length of the range in bytes.
 */
bool tb_test_allocator_owns(const tb_test_allocator *tracker, const void *memory, size_t size);

#endif
