/** @file path.h
 * @brief Pure UTF-8 path operations with explicit Windows or POSIX rules.
 *
 * No filesystem access, case folding, percent decoding, or Unicode normalization.
 * Input views must contain valid UTF-8 and no embedded NUL. Results use '/'.
 * Windows drive-relative paths remain drive-relative. Extended drive/UNC paths
 * are recognized and retain literal '.' and '..' components. Device namespaces
 * are rejected. POSIX preserves exactly two leading slashes.
 *
 * All functions are thread-safe with synchronized input/output buffers and
 * allocators. Views are borrowed for a call; parsed views borrow the original
 * input. Builders use temporary heap storage and fail fast on allocation
 * failure. They never partially write a destination on an error.
 */
#ifndef TB_PATH_H
#define TB_PATH_H
#include "badger/allocator.h"
#include "badger/api.h"
#include <stdbool.h>
#include <stddef.h>

/** @brief Rules to apply, independent of the host operating system. */
typedef enum tb_path_style
{
    TB_PATH_POSIX,  /**< Only '/' separates components. */
    TB_PATH_WINDOWS /**< Both slash forms separate components. */
} tb_path_style;
/** @brief Result of path processing. */
typedef enum tb_path_result
{
    TB_PATH_OK,               /**< Operation succeeded. */
    TB_PATH_INVALID,          /**< Invalid UTF-8, NUL, malformed root or unsupported namespace. */
    TB_PATH_BUFFER_TOO_SMALL, /**< Destination is too small; required size is available. */
    TB_PATH_DIFFERENT_ROOT,   /**< Relative-path inputs do not have matching absolute roots. */
    TB_PATH_ESCAPE,           /**< Rooted join received a rooted name or one escaping its root. */
    TB_PATH_TOO_LARGE         /**< A required size is not representable. */
} tb_path_result;
/** @brief Borrowed byte range; NULL is allowed only when length is zero. */
typedef struct tb_path_view
{
    const char *data; /**< Input bytes; no terminator is required. */
    size_t length;    /**< Number of bytes. */
} tb_path_view;
/** @brief Borrowed parts of an unmodified input path. */
typedef struct tb_path_parts
{
    tb_path_view root;      /**< Root including a following separator when present. */
    tb_path_view directory; /**< Everything before the last component, including separators. */
    tb_path_view name;      /**< Last component; empty after a trailing separator. */
    tb_path_view stem;      /**< Last component before its final extension. */
    tb_path_view extension; /**< Final extension including '.', or empty; dotfiles have none. */
    bool absolute;          /**< True for a full drive/UNC root or a POSIX root. */
    bool verbatim;          /**< Windows extended drive/UNC namespace: dots stay literal. */
} tb_path_parts;
/**
 * @brief Split a path without allocating or normalizing it.
 * @param style Host-independent syntax; unknown enum values assert.
 * @param input Borrowed path. NULL requires zero length.
 * @param[out] parts Required; unchanged on invalid input.
 * @return TB_PATH_OK or TB_PATH_INVALID. A Windows current-drive root ('/x')
 * is rooted but not fully absolute. A trailing dot is an empty extension '.'.
 */
TB_API tb_path_result tb_path_parse(tb_path_style style, tb_path_view input, tb_path_parts *parts);

/**
 * @brief Normalize separators and ordinary dot components.
 * @param style Path syntax.
 * @param input Borrowed path.
 * @param destination Output bytes, or NULL with capacity zero to measure.
 * @param capacity Bytes available, including the terminator.
 * @param[out] required Required pointer; receives bytes including the NUL on
 * success/BUFFER_TOO_SMALL, zero on other errors.
 * @return A tb_path_result. Inputs may overlap destination. Empty input becomes
 * '.'. Trailing separators are removed except on roots. Leading parents are
 * retained for relative paths and clamped at rooted paths.
 */
TB_API tb_path_result tb_path_normalize(tb_path_style style, tb_path_view input, char *destination,
                                        size_t capacity, size_t *required);
/** @brief Owned form of tb_path_normalize. Allocator NULL selects the heap.
 * @param allocator Allocator for the result only.
 * @param style Path syntax.
 * @param input Borrowed input.
 * @param[out] result Required status; NULL returned only for input/size errors.
 * @return Owned NUL-terminated string. Release with tb_str_free using the same
 * allocator and original string length. Allocation exhaustion is fatal.
 */
TB_API char *tb_path_normalize_new(const tb_allocator *allocator, tb_path_style style,
                                   tb_path_view input, tb_path_result *result);

/** @brief Join and normalize. A rooted second path replaces the first.
 * @param style Path syntax.
 * @param first Borrowed first path.
 * @param second Borrowed second path or extension.
 * @param destination Output or NULL to measure.
 * @param capacity Bytes available including NUL; zero for measurement.
 * @param[out] required Required output; same contract as tb_path_normalize.
 * @return Status; destination unchanged on errors. Temporary allocation is fail-fast.
 */
TB_API tb_path_result tb_path_join(tb_path_style style, tb_path_view first, tb_path_view second,
                                   char *destination, size_t capacity, size_t *required);
/** @brief Owned form of tb_path_join; same input semantics.
 * @param allocator Result allocator, or NULL for the heap.
 * @param style Path syntax.
 * @param first First path.
 * @param second Second path or extension.
 * @param[out] result Required status.
 * @return Owned string for tb_str_free, or NULL on input/size errors. Allocation
 * failure is fatal. Inputs are borrowed only for the call.
 */
TB_API char *tb_path_join_new(const tb_allocator *allocator, tb_path_style style,
                              tb_path_view first, tb_path_view second, tb_path_result *result);

/** @brief Join a relative name below an absolute root. Reject rooted names and any parent traversal
 * above that root. This is lexical containment only; it does not protect against symlinks, reparse
 * points, filesystem aliases, or a later decoding step. Verbatim roots are refused.
 * @param style Path syntax.
 * @param first Borrowed first path.
 * @param second Borrowed second path or extension.
 * @param destination Output or NULL to measure.
 * @param capacity Bytes available including NUL; zero for measurement.
 * @param[out] required Required output; same contract as tb_path_normalize.
 * @return Status; destination unchanged on errors. Temporary allocation is fail-fast.
 */
TB_API tb_path_result tb_path_rooted_join(tb_path_style style, tb_path_view first,
                                          tb_path_view second, char *destination, size_t capacity,
                                          size_t *required);
/** @brief Owned form of tb_path_rooted_join; same input semantics.
 * @param allocator Result allocator, or NULL for the heap.
 * @param style Path syntax.
 * @param first First path.
 * @param second Second path or extension.
 * @param[out] result Required status.
 * @return Owned string for tb_str_free, or NULL on input/size errors. Allocation
 * failure is fatal. Inputs are borrowed only for the call.
 */
TB_API char *tb_path_rooted_join_new(const tb_allocator *allocator, tb_path_style style,
                                     tb_path_view first, tb_path_view second,
                                     tb_path_result *result);

/** @brief Make the second absolute path relative to the first absolute directory. Roots and
 * components compare bytewise. Verbatim paths are refused.
 * @param style Path syntax.
 * @param first Borrowed first path.
 * @param second Borrowed second path or extension.
 * @param destination Output or NULL to measure.
 * @param capacity Bytes available including NUL; zero for measurement.
 * @param[out] required Required output; same contract as tb_path_normalize.
 * @return Status; destination unchanged on errors. Temporary allocation is fail-fast.
 */
TB_API tb_path_result tb_path_relative(tb_path_style style, tb_path_view first, tb_path_view second,
                                       char *destination, size_t capacity, size_t *required);
/** @brief Owned form of tb_path_relative; same input semantics.
 * @param allocator Result allocator, or NULL for the heap.
 * @param style Path syntax.
 * @param first First path.
 * @param second Second path or extension.
 * @param[out] result Required status.
 * @return Owned string for tb_str_free, or NULL on input/size errors. Allocation
 * failure is fatal. Inputs are borrowed only for the call.
 */
TB_API char *tb_path_relative_new(const tb_allocator *allocator, tb_path_style style,
                                  tb_path_view first, tb_path_view second, tb_path_result *result);

/** @brief Replace the final extension with the second view. It must be empty or start with a dot
 * and contain no separators. Empty removes the extension. A path with no final filename is refused.
 * @param style Path syntax.
 * @param first Borrowed first path.
 * @param second Borrowed second path or extension.
 * @param destination Output or NULL to measure.
 * @param capacity Bytes available including NUL; zero for measurement.
 * @param[out] required Required output; same contract as tb_path_normalize.
 * @return Status; destination unchanged on errors. Temporary allocation is fail-fast.
 */
TB_API tb_path_result tb_path_replace_extension(tb_path_style style, tb_path_view first,
                                                tb_path_view second, char *destination,
                                                size_t capacity, size_t *required);
/** @brief Owned form of tb_path_replace_extension; same input semantics.
 * @param allocator Result allocator, or NULL for the heap.
 * @param style Path syntax.
 * @param first First path.
 * @param second Second path or extension.
 * @param[out] result Required status.
 * @return Owned string for tb_str_free, or NULL on input/size errors. Allocation
 * failure is fatal. Inputs are borrowed only for the call.
 */
TB_API char *tb_path_replace_extension_new(const tb_allocator *allocator, tb_path_style style,
                                           tb_path_view first, tb_path_view second,
                                           tb_path_result *result);

/** @brief Return the lexical parent of a normalized path. A root is its own parent; a lone relative
 * name has parent dot.
 * @param style Path syntax.
 * @param input Borrowed input path.
 * @param destination Output or NULL to measure.
 * @param capacity Output bytes including NUL; zero for measurement.
 * @param[out] required Required size; same contract as tb_path_normalize.
 * @return Status; destination unchanged on error. Temporary allocation is fail-fast.
 */
TB_API tb_path_result tb_path_parent(tb_path_style style, tb_path_view input, char *destination,
                                     size_t capacity, size_t *required);
/** @brief Owned form of tb_path_parent; same semantics.
 * @param allocator Result allocator, or NULL for heap.
 * @param style Path syntax.
 * @param input Borrowed input path.
 * @param[out] result Required status.
 * @return Owned string for tb_str_free, or NULL on input/size errors. Allocation failure is fatal.
 */
TB_API char *tb_path_parent_new(const tb_allocator *allocator, tb_path_style style,
                                tb_path_view input, tb_path_result *result);

/** @brief Normalize and replace separators with backslashes for Windows, or slashes for POSIX. This
 * remains UTF-8; platform adapters convert its encoding.
 * @param style Path syntax.
 * @param input Borrowed input path.
 * @param destination Output or NULL to measure.
 * @param capacity Output bytes including NUL; zero for measurement.
 * @param[out] required Required size; same contract as tb_path_normalize.
 * @return Status; destination unchanged on error. Temporary allocation is fail-fast.
 */
TB_API tb_path_result tb_path_to_native(tb_path_style style, tb_path_view input, char *destination,
                                        size_t capacity, size_t *required);
/** @brief Owned form of tb_path_to_native; same semantics.
 * @param allocator Result allocator, or NULL for heap.
 * @param style Path syntax.
 * @param input Borrowed input path.
 * @param[out] result Required status.
 * @return Owned string for tb_str_free, or NULL on input/size errors. Allocation failure is fatal.
 */
TB_API char *tb_path_to_native_new(const tb_allocator *allocator, tb_path_style style,
                                   tb_path_view input, tb_path_result *result);

/** @brief Name a result, for a log or a message: "invalid path", "buffer too small".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 * @par Threading Thread-safe. */
TB_API const char *tb_path_result_name(tb_path_result result);

#endif
