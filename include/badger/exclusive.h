/** @file exclusive.h
 * @brief Room for the debug check that one thread at a time changes an
 * object, for public structures whose caller serializes their use.
 *
 * Builds with assertions record, in this structure, which thread is changing
 * the object that embeds it and how many times threads have begun changing
 * it, and fail an
 * assertion when two threads overlap where the object's threading contract
 * forbids it. Builds without assertions only keep its room, so that every
 * build agrees on the layout of the structures that embed it. Its members are
 * private to the engine.
 */
#ifndef TB_EXCLUSIVE_H
#define TB_EXCLUSIVE_H

#include "badger/platform.h"

#include <stddef.h>

/**
 * @brief The state of the check. Zero, as #TB_EXCLUSIVE_CHECK_INIT gives, is
 * an object no thread is using.
 */
typedef struct tb_exclusive_check
{
    tb_platform_atomic_size owner;      /**< Private: the changing thread, or 0. */
    size_t depth;                       /**< Private: how often it has entered. */
    tb_platform_atomic_size generation; /**< Private: changes begun so far. */
} tb_exclusive_check;

/** @brief Initializer for a check that no thread is inside. */
#define TB_EXCLUSIVE_CHECK_INIT {{0}, 0, {0}}

#endif
