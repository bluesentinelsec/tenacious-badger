/** @file stack.h
 * @brief The call stack of a thread: where it is, and how it got there.
 * Implementations are build-selected.
 *
 * A stack is captured as addresses, which is cheap, and an address is turned
 * into names only when somebody is going to read them, which is not.
 *
 * @code
 * void *frames[32] = {0};
 * const size_t count = tb_stack_capture(frames, 32, 0);
 *
 * for (size_t index = 0; index < count; index++)
 * {
 *     tb_stack_location location = {0};
 *
 *     tb_stack_describe(frames[index], &location);
 *     // location.function "load_level", location.module "burrow.exe",
 *     // location.file ".../level.c", location.line 212
 * }
 * @endcode
 *
 * **What can be named depends on the platform and the build.** Windows names
 * functions, files, and lines when the program's debugging information (its
 * .pdb files) is beside it. Linux and macOS name the module, and the
 * function when it is one that the module exports or has kept the name of;
 * they do not name files or lines. What cannot be named is left empty, and
 * the module and the offset within it are enough for the platform's own
 * tools to name the rest later.
 *
 * @par Threading
 * Thread-safe.
 */
#ifndef TB_STACK_H
#define TB_STACK_H

#include "badger/api.h"

#include <stddef.h>
#include <stdint.h>

/** @brief Bytes for the name of a function, its terminator included. */
#define TB_STACK_FUNCTION_CAPACITY 128
/** @brief Bytes for the name of a module, its terminator included. */
#define TB_STACK_MODULE_CAPACITY 64
/** @brief Bytes for the path of a source file, its terminator included. */
#define TB_STACK_FILE_CAPACITY 260

/**
 * @brief What an address in a program is part of.
 *
 * Every string is UTF-8 and terminated. A name that is longer than its
 * capacity is cut off at a whole character.
 */
typedef struct tb_stack_location
{
    const void *address;                       /**< The address that was described. */
    char function[TB_STACK_FUNCTION_CAPACITY]; /**< The function; empty when not known. */
    size_t function_offset;                    /**< Bytes from the start of the function. */
    char module[TB_STACK_MODULE_CAPACITY];     /**< The program or library file, without its
                                                    directory; empty when not known. */
    size_t module_offset;                      /**< Bytes from where the module is loaded. */
    char file[TB_STACK_FILE_CAPACITY];         /**< The source file; empty when not known. */
    uint32_t line;                             /**< The line in it; 0 when not known. */
} tb_stack_location;

/**
 * @brief Capture the call stack of the calling thread.
 *
 * The addresses are those that the calls return to, the innermost first:
 * `frames[0]` is in the function that called tb_stack_capture(), `frames[1]`
 * in the function that called that one, and so on outward.
 *
 * A build that optimizes may join a function into its caller, or leave one
 * by a jump where the source has a call, and such a function has no frame of
 * its own.
 *
 * @param[out] frames Receives the addresses; required non-null unless
 * `capacity` is 0.
 * @param capacity How many addresses `frames` has room for.
 * @param skip How many of the innermost frames to leave out: 1 for a function
 * that captures a stack on behalf of its caller.
 * @return The number of addresses written, which is `capacity` at most, and
 * fewer when the stack is shallower. At most 128 frames are captured from the
 * innermost.
 * @par Ownership Nothing is allocated.
 * @par Threading Thread-safe.
 */
TB_API size_t tb_stack_capture(void **frames, size_t capacity, size_t skip);

/**
 * @brief Say what an address is part of.
 *
 * The first call on Windows loads the system's debugging library and the
 * program's debugging information, which takes tens of milliseconds and
 * allocates. It is for a report that someone will read, not for a loop.
 *
 * @param address Any address; one from tb_stack_capture() is where a call
 * returns to, which is the instruction after the call.
 * @param[out] location Required non-null; every field is set, to empty or
 * zero where nothing is known.
 * @par Threading Thread-safe; calls are serialized where the system's
 * library requires it.
 */
TB_API void tb_stack_describe(const void *address, tb_stack_location *location);

#endif
