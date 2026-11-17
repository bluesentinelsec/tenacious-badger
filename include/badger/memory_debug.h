/** @file memory_debug.h
 * @brief Memory functions that check how memory is used.
 *
 * The checking functions are a set of tb_memory_functions. Installed with
 * tb_memory_set_functions(), they stand between the engine and the functions
 * that do the allocating, and find what those do not:
 *
 * - a block that is freed twice, and a pointer that is freed or resized but
 *   is no block;
 * - bytes written just before a block or just after it;
 * - blocks that are never freed, and who allocated them;
 * - how many blocks and bytes are allocated, now and at the most.
 *
 * @code
 * tb_memory_functions checking = {0};
 *
 * // At startup, before anything is allocated:
 * tb_memory_debug_get_functions(&checking);
 * (void)tb_memory_set_functions(&checking);
 * ...
 * // At shutdown: what is still allocated, and under which labels.
 * tb_memory_debug_report();
 * @endcode
 *
 * A sanitizer finds more, and finds it at the instruction that does it. These
 * functions are for builds and platforms where no sanitizer runs, and for
 * what a sanitizer does not answer: who holds how much memory.
 *
 * **A fault ends the process.** What the functions find goes to a handler,
 * and the default handler writes it to stderr and ends the process with
 * tb_platform_abort(). A written byte is found when its block is freed or
 * resized, or when tb_memory_debug_check() looks at every block.
 *
 * **A block is known by a number and a label.** Blocks are numbered from 1 in
 * the order they are allocated. The label is whatever its thread last gave
 * tb_memory_debug_set_label(), which is how memory is attributed to a module.
 * To see the call stack of a block that a report names, run the program
 * again under a debugger with tb_memory_debug_watch() given its number: the
 * process stops as that block is allocated. The numbers repeat from run to
 * run as far as the program does.
 *
 * **Every block costs more.** A block has 16 guard bytes on each side and a
 * record before them. New memory is filled with 0xCD and freed memory with
 * 0xDD, so that a value read from either stands out. One lock serializes
 * every call, so threads that allocate wait for one another.
 *
 * @par Threading
 * The checking functions and the functions of this header are thread-safe,
 * except where one says that it is stage-owned.
 */
#ifndef TB_MEMORY_DEBUG_H
#define TB_MEMORY_DEBUG_H

#include "badger/api.h"
#include "badger/memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * The checking functions.
 * ------------------------------------------------------------------------- */

/**
 * @brief Get the checking functions, to install with tb_memory_set_functions().
 *
 * They may also be called directly, which checks the blocks of one part of a
 * program and leaves the rest alone.
 *
 * @param[out] functions Required non-null; receives the four functions.
 * @par Threading Thread-safe.
 */
TB_API void tb_memory_debug_get_functions(tb_memory_functions *functions);

/**
 * @brief Choose the functions that the checking functions allocate with.
 *
 * They are the engine's built-in functions unless this says otherwise. An
 * application with an allocator of its own gives it here, and installs the
 * checking functions in the engine: the checks then stand in front of that
 * allocator.
 *
 * @param functions The functions to allocate with, all four members
 * non-null; or NULL for the engine's built-in functions. The structure is
 * copied.
 * @return true when the functions were taken. false, changing nothing, when
 * a member is NULL, a member is a checking function (recursive), or a block
 * from the checking functions is still allocated:
 * the new functions could not free it.
 * @par Threading Stage-owned: call during single-threaded startup or teardown.
 */
TB_API bool tb_memory_debug_set_underlying(const tb_memory_functions *functions);

/**
 * @brief Forget everything that has been counted and numbered.
 *
 * The statistics return to zero, the next block is number 1 again, no block
 * is watched, and the blocks that were freed are forgotten. The handler and
 * the underlying functions stay as they are.
 *
 * @return true when it was done. false, changing nothing, when a block from
 * the checking functions is still allocated.
 * @par Threading Thread-safe.
 */
TB_API bool tb_memory_debug_reset(void);

/* -------------------------------------------------------------------------
 * Blocks and who allocated them.
 * ------------------------------------------------------------------------- */

/** @brief A block from the checking functions. */
typedef struct tb_memory_debug_block
{
    const void *memory; /**< The address that the caller was given. */
    size_t size;        /**< The bytes that the caller asked for. */
    uint64_t number;    /**< Its place in the order of allocation, from 1. */
    const char *label;  /**< The label of the thread that allocated it, or NULL. */
    const char *file;   /**< Borrowed allocation source file, or NULL if unannotated. */
    size_t line;        /**< Allocation source line, or 0 if unannotated. */
} tb_memory_debug_block;

/**
 * @brief Label the blocks that the calling thread allocates from now on.
 *
 * A label says whose memory a block is: a module, a subsystem, a phase of
 * loading. Set it on entering the code to be charged, and put the previous
 * label back on leaving:
 *
 * @code
 * const char *const previous = tb_memory_debug_set_label("assets");
 *
 * load_everything();
 * (void)tb_memory_debug_set_label(previous);
 * @endcode
 *
 * A block keeps the label that it was allocated under, whatever is done to
 * it later and whichever thread does it.
 *
 * @param label The label, or NULL for none. Borrowed for as long as any block
 * allocated under it may be reported, including freed-block history until
 * reset, so pass a string literal.
 * @return The label that was in force on this thread; NULL when there was none.
 * @par Threading Thread-safe. Every thread has a label of its own, and starts
 * with none.
 */
TB_API const char *tb_memory_debug_set_label(const char *label);

/** @brief Source location attributed to allocations on a thread. */
typedef struct tb_memory_debug_site
{
    const char *file; /**< Borrowed static string, or NULL for no location. */
    size_t line;      /**< Source line; 0 when file is NULL. */
} tb_memory_debug_site;

/**
 * @brief Set the source location for subsequent allocations on this thread.
 * @param site Source file and line, normally {__FILE__, __LINE__}; {0} clears it.
 * The file string must outlive live blocks and freed-block history until reset.
 * @return Previous site, to restore on leaving the annotated scope.
 * @par Ownership The site is copied; its file string is borrowed.
 * @par Threading Thread-safe, thread-local. Resizing preserves the original site.
 * @par Errors Asserts if a NULL file is paired with a nonzero line.
 */
TB_API tb_memory_debug_site tb_memory_debug_set_site(tb_memory_debug_site site);

/**
 * @brief Receives one allocated block.
 * @param block Borrowed description, valid for the duration of the call.
 * @param context The pointer given to tb_memory_debug_visit().
 */
typedef void tb_memory_debug_visitor(const tb_memory_debug_block *block, void *context);

/**
 * @brief Call a function for every block that is allocated, oldest first.
 *
 * @param visitor Required non-null. It runs under the lock of the checking
 * functions, so it must not allocate or free with them, nor call a function
 * of this header.
 * @param context Passed to every call; may be NULL.
 * @par Threading Thread-safe.
 */
TB_API void tb_memory_debug_visit(tb_memory_debug_visitor *visitor, void *context);

/** @brief How much the checking functions have allocated. */
typedef struct tb_memory_debug_statistics
{
    size_t live_blocks;        /**< Blocks allocated and not freed. */
    size_t live_bytes;         /**< The bytes that their callers asked for. */
    size_t peak_blocks;        /**< The most blocks that were allocated at one time. */
    size_t peak_bytes;         /**< The most bytes that were allocated at one time. */
    uint64_t allocated_blocks; /**< Blocks ever allocated: the number of the newest. */
} tb_memory_debug_statistics;

/**
 * @brief Get the counts, as they are at one moment.
 * @param[out] statistics Required non-null.
 * @par Threading Thread-safe.
 */
TB_API void tb_memory_debug_get_statistics(tb_memory_debug_statistics *statistics);

/**
 * @brief Write what is allocated to stderr.
 *
 * The counts, then the blocks and bytes under each label, then the oldest
 * blocks one by one. At shutdown, what it lists has leaked.
 *
 * @par Threading Thread-safe.
 */
TB_API void tb_memory_debug_report(void);

/* -------------------------------------------------------------------------
 * Faults.
 * ------------------------------------------------------------------------- */

/** @brief What the checking functions can find. */
typedef enum tb_memory_debug_fault
{
    /** A pointer was freed or resized that is not an allocated block: it was
     * never one, it points into one, or it was freed too long ago to be
     * remembered. */
    TB_MEMORY_DEBUG_NOT_A_BLOCK,
    /** A block was freed or resized after it had been freed. */
    TB_MEMORY_DEBUG_FREED_TWICE,
    /** A byte was written in the 16 before a block. */
    TB_MEMORY_DEBUG_WRITTEN_BEFORE,
    /** A byte was written in the 16 after a block. */
    TB_MEMORY_DEBUG_WRITTEN_AFTER,
    /** The block given to tb_memory_debug_watch() has just been allocated. */
    TB_MEMORY_DEBUG_WATCHED
} tb_memory_debug_fault;

/** @brief One thing that the checking functions found. */
typedef struct tb_memory_debug_finding
{
    tb_memory_debug_fault fault; /**< What was found. */
    /** The block at fault. For TB_MEMORY_DEBUG_NOT_A_BLOCK only `memory` is
     * set, to the pointer; for TB_MEMORY_DEBUG_FREED_TWICE it is the block as
     * it was when it was freed. */
    tb_memory_debug_block block;
    /** For a written byte, how far it is from the block: 1 for the byte next
     * to it, up to 16. It is the nearest one that was written. Otherwise 0. */
    size_t distance;
} tb_memory_debug_finding;

/**
 * @brief Receives a finding in place of the default report.
 * @param finding Borrowed description, valid for the duration of the call.
 * @param context The pointer registered with tb_memory_debug_set_handler().
 * @par Errors A handler must not return: the program's memory can no longer
 * be trusted. If it returns, the process ends. The lock of the checking
 * functions is not held, so a handler may leave by a non-local jump, as a
 * test does; the block at fault then stays allocated.
 */
typedef void tb_memory_debug_handler(const tb_memory_debug_finding *finding, void *context);

/**
 * @brief Replace what happens when the checking functions find a fault.
 *
 * The default writes the finding to stderr and ends the process with
 * tb_platform_abort().
 *
 * @param handler Handler to install, or NULL to restore the default.
 * @param context Passed to every handler call; may be NULL. The caller keeps
 * ownership and must keep it valid while the handler is installed.
 * @par Threading Stage-owned: call during single-threaded startup.
 */
TB_API void tb_memory_debug_set_handler(tb_memory_debug_handler *handler, void *context);

/**
 * @brief Look at the guard bytes of every allocated block.
 *
 * A block is otherwise looked at only when it is freed or resized. Calling
 * this between two steps of a program says which of them wrote where it
 * should not have.
 *
 * @par Errors The oldest block with a written byte is reported as a fault.
 * @par Threading Thread-safe.
 */
TB_API void tb_memory_debug_check(void);

/**
 * @brief Stop the process when the block with a number is allocated.
 *
 * The block is reported as TB_MEMORY_DEBUG_WATCHED from inside the call that
 * allocates it, so a debugger or a crash dump shows who made it.
 *
 * @param number The number of the block, as a report gave it; 0 to watch none.
 * @par Threading Thread-safe.
 */
TB_API void tb_memory_debug_watch(uint64_t number);

#endif
