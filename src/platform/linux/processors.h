/* Private to the Linux adapter: the counting of processors, apart from the
 * system calls and files that describe them, so that it can be tested with
 * machines nobody has on a desk. Nothing here touches the system. */
#ifndef TB_LINUX_PROCESSORS_H
#define TB_LINUX_PROCESSORS_H

#include "badger/system.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Logical processors are numbered from zero. Numbers from this one up are
 * not represented; it is the size of the system's own fixed set. */
#define TB_LINUX_CPU_LIMIT ((size_t)1024)

#define TB_LINUX_CPU_SET_WORD_BITS ((size_t)64)

/* A set of logical processor numbers below TB_LINUX_CPU_LIMIT.
 * Zero-initialized, it is empty. */
typedef struct tb_linux_cpu_set
{
    uint64_t words[TB_LINUX_CPU_LIMIT / TB_LINUX_CPU_SET_WORD_BITS];
} tb_linux_cpu_set;

/* Adds `cpu`, which is below TB_LINUX_CPU_LIMIT. */
void tb_linux_cpu_set_add(tb_linux_cpu_set *set, size_t cpu);

/* Whether `cpu` is in the set. Any number may be asked about. */
bool tb_linux_cpu_set_has(const tb_linux_cpu_set *set, size_t cpu);

/* Reads a list of processors as the kernel writes one, such as "0-7,16-23"
 * followed by a newline: numbers and ranges separated by commas. `text` is
 * NUL-terminated. Returns false, with `set` empty, when the text is not such
 * a list or names a processor at or beyond TB_LINUX_CPU_LIMIT. An empty list
 * is a list. Both pointers are required non-null. */
bool tb_linux_cpu_list_parse(const char *text, tb_linux_cpu_set *set);

/* Longest text of a file that the counting reads, terminator included. A
 * list of every processor one by one fits. */
#define TB_LINUX_FILE_CAPACITY ((size_t)8192)

/* Longest path that the counting asks for, terminator included. */
#define TB_LINUX_PATH_CAPACITY ((size_t)96)

/* Reads the whole of a small text file into `buffer`, of `capacity` bytes,
 * and terminates it. Returns false when the file cannot be read or does not
 * fit. `context` is the pointer given beside the function. */
typedef bool tb_linux_read_file(void *context, const char *path, char *buffer, size_t capacity);

/* One logical processor that the process may use: its number to the system,
 * the number of its core among the allowed cores, counted from zero in the
 * order of their first processors, and whether its core is of the fastest
 * kind. */
typedef struct tb_linux_processor
{
    uint32_t cpu;
    uint32_t core;
    bool is_performance;
} tb_linux_processor;

/* Lists the processors among `allowed`, which is not empty, in the order of
 * their numbers, from what the kernel's files say about them. Writes at most
 * `capacity` to `list` and returns how many there are. `list` may be NULL
 * when `capacity` is 0. The other pointers but `context` are required
 * non-null. */
size_t tb_linux_list_processors(const tb_linux_cpu_set *allowed, tb_linux_read_file *read,
                                void *context, tb_linux_processor *list, size_t capacity);

/* Counts the processors among `allowed`, which is not empty, from what the
 * kernel's files say about them. `read` is given paths under /sys. A file
 * that cannot be read is something the system does not say, and the counts
 * fall back as badger/system.h describes; it is never an error. All pointers
 * but `context` are required non-null. */
void tb_linux_count_processors(const tb_linux_cpu_set *allowed, tb_linux_read_file *read,
                               void *context, tb_system_processors *processors);

/* The processors that this process may use, listed as above. Unlike the rest
 * of this header it asks the system: it is defined in system.c, for the
 * thread functions, which confine a thread by these numbers. */
size_t tb_linux_list_process_processors(tb_linux_processor *list, size_t capacity);

#endif
