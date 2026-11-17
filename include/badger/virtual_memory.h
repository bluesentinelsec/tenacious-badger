/** @file virtual_memory.h
 * @brief Native address-space reservations and page commitment.
 *
 * Regions are exclusively owned, zero-initialized values. Do not copy ownership
 * or modify their fields. Reservations do not count as core heap allocations.
 * POSIX commitment grants access to demand-paged memory; overcommit can still
 * exhaust physical memory later. No executable mappings or implicit privilege
 * changes are requested.
 *
 * @par Threading
 * Externally synchronized: the caller serializes every call that names the
 * same region, and every access to the pages that a call commits or
 * decommits. Different regions are independent, and tb_vm_get_info() is
 * thread-safe.
 */
#ifndef TB_VIRTUAL_MEMORY_H
#define TB_VIRTUAL_MEMORY_H
#include "badger/api.h"
#include <stdbool.h>
#include <stddef.h>

/** @brief Virtual-memory operation result. */
typedef enum tb_vm_result
{
    TB_VM_OK,          /**< Success. */
    TB_VM_INVALID,     /**< Zero, unaligned, out-of-bounds, or unrepresentable size. */
    TB_VM_NO_MEMORY,   /**< Address space or native memory resources exhausted. */
    TB_VM_UNAVAILABLE, /**< Optional explicit large pages unavailable to this process. */
    TB_VM_OS_ERROR     /**< A native failure that no other result names. */
} tb_vm_result;
/** @brief Host page geometry. All sizes are bytes. */
typedef struct tb_vm_info
{
    size_t page_size;              /**< Regular page size; zero on native query failure. */
    size_t allocation_granularity; /**< Reservation address alignment. */
    size_t large_page_size;        /**< Explicit large-page size, zero when unsupported.
                                      A nonzero value does not promise privilege or free pages. */
} tb_vm_info;
/** @brief Owned address range; zero is empty. */
typedef struct tb_vm_region
{
    void *address;    /**< Base address, NULL only when empty. */
    size_t size;      /**< Total reservation bytes, including inaccessible pages. */
    bool large_pages; /**< True only for fully committed explicit large pages. */
} tb_vm_region;
/** @brief Query page geometry; no resources acquired. Thread-safe. */
TB_API tb_vm_info tb_vm_get_info(void);
/** @brief Reserve inaccessible address space, rounded up to whole pages.
 * @param size Requested bytes, nonzero and at most PTRDIFF_MAX after rounding.
 * @param[out] region Required zero region; unchanged on failure.
 * @return Status. Release the whole region with tb_vm_release. */
TB_API tb_vm_result tb_vm_reserve(size_t size, tb_vm_region *region);
/** @brief Make a regular reservation's pages readable/writable.
 * @param region Required owned regular region. @param offset Page-aligned offset.
 * @param size Nonzero page multiple, wholly within the region.
 * @return Status. Newly committed pages contain zeros; already committed pages
 * retain contents. On failure do not access the range until commit succeeds. */
TB_API tb_vm_result tb_vm_commit(tb_vm_region *region, size_t offset, size_t size);
/** @brief Discard contents and revoke access while retaining the address range.
 * @param region Required owned regular region. @param offset Page-aligned offset.
 * @param size Nonzero page multiple wholly within the region.
 * @return Status. Successful recommit supplies zeros. Contents are not preserved
 * on failure. The caller must prevent concurrent accesses and mapping changes. */
TB_API tb_vm_result tb_vm_decommit(tb_vm_region *region, size_t offset, size_t size);
/** @brief Release an entire region and zero it on success. Empty is accepted.
 * @param region Required owner; unchanged on native release failure.
 * @return Status. Every pointer into a released region becomes invalid. */
TB_API tb_vm_result tb_vm_release(tb_vm_region *region);
/** @brief Reserve and commit explicit large pages in one operation.
 * @param size Nonzero multiple of the reported large-page size.
 * @param[out] region Required zero owner; unchanged on failure.
 * @return UNAVAILABLE for absent support, privilege, or available large pages.
 * INVALID for an invalid size. Release with tb_vm_release; partial commit and
 * decommit are unavailable for these regions. Callers choose a regular fallback. */
TB_API tb_vm_result tb_vm_allocate_large(size_t size, tb_vm_region *region);
/** @brief Name a result, for a log or a message: "out of memory", "unavailable".
 *
 * It needs nothing from the platform, so the engine implements it once and
 * no platform adapter does.
 *
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 * @par Threading Thread-safe. */
TB_API const char *tb_vm_result_name(tb_vm_result result);
#endif
