/* Private to the engine: searching and comparing ranges of bytes.
 *
 * The string modules present the same searches in two forms, on
 * NUL-terminated strings (badger/str.h) and on lengths (badger/text.h). Both
 * are thin layers over these functions, so each search is written, and
 * tested, once.
 *
 * Every function takes a pointer and a range of indexes from `begin` up to,
 * but not including, `end`. The caller has checked that the range is in order
 * and inside the bytes. A search that finds nothing returns
 * TB_BYTES_NOT_FOUND. */
#ifndef TB_CORE_BYTES_H
#define TB_CORE_BYTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TB_BYTES_NOT_FOUND SIZE_MAX

/* Which of the 256 byte values belong to a set. Start one empty, with `{0}`,
 * and add its members with tb_byte_set_add(). */
typedef struct tb_byte_set
{
    bool has[256];
} tb_byte_set;

/* Adds `length` bytes to a set; they may repeat and may include zero. The set
 * is filled where it is: a set is searched for far more often in a few bytes
 * than in many, and returning one by value copied its 256 flags for every
 * search. */
void tb_byte_set_add(tb_byte_set *set, const char *members, size_t length);

/* Three-way comparison of two byte ranges as unsigned bytes: negative, zero,
 * or positive as the left sorts before, equal to, or after the right. A range
 * that is a prefix of the other sorts first. */
int tb_bytes_compare(const char *left, size_t left_length, const char *right, size_t right_length);

/* Index of the first, or last, byte in the range that equals `byte`. */
size_t tb_bytes_find_byte(const char *bytes, size_t begin, size_t end, char byte);
size_t tb_bytes_rfind_byte(const char *bytes, size_t begin, size_t end, char byte);

/* Index of the first, or last, byte in the range that is in the set. */
size_t tb_bytes_find_any(const char *bytes, size_t begin, size_t end, const tb_byte_set *set);
size_t tb_bytes_rfind_any(const char *bytes, size_t begin, size_t end, const tb_byte_set *set);

/* Length of the run of bytes in the set at the start, or at the end, of the
 * range. */
size_t tb_bytes_span(const char *bytes, size_t begin, size_t end, const tb_byte_set *set);
size_t tb_bytes_rspan(const char *bytes, size_t begin, size_t end, const tb_byte_set *set);

/* Index of the first, or last, place in the range where the whole pattern
 * occurs. An empty pattern occurs everywhere: the first place is `begin` and
 * the last is `end`. */
size_t tb_bytes_find(const char *bytes, size_t begin, size_t end, const char *pattern,
                     size_t pattern_length);
size_t tb_bytes_rfind(const char *bytes, size_t begin, size_t end, const char *pattern,
                      size_t pattern_length);

#endif
