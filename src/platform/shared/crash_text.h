/* Private to the platform adapters, and shared by them: the line that says
 * what a crash was, built in a buffer of fixed size. A crash handler cannot
 * count on the C library's formatting, which may take a lock or allocate,
 * so these do neither. */
#ifndef TB_PLATFORM_CRASH_TEXT_H
#define TB_PLATFORM_CRASH_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* A line being built: `length` bytes of `capacity` are used, and the text
 * stays terminated. Zero-initialize, then point `text` at the buffer. */
typedef struct tb_crash_text
{
    char *text;
    size_t capacity; /* At least one, for the terminator. */
    size_t length;
} tb_crash_text;

/* Adds a terminated string, cut off where the buffer is full. */
void tb_crash_text_add(tb_crash_text *line, const char *text);

/* Adds a value as 0x and sixteen hexadecimal digits, as an address. */
void tb_crash_text_add_address(tb_crash_text *line, uintptr_t value);

/* Adds a value in decimal, with a minus sign where it is negative. */
void tb_crash_text_add_integer(tb_crash_text *line, long long value);

#endif
