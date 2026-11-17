#include "processors.h"

/* Everything here is plain C over text and numbers. What the kernel says
 * arrives through a function that the caller supplies, so the tests can
 * describe any machine. */

#define TB_LINUX_DECIMAL_BASE ((size_t)10)

/* Decimal digits of the highest processor number, TB_LINUX_CPU_LIMIT - 1. */
#define TB_LINUX_CPU_DIGITS_MAX 4

/* ---------------------------------------------------------------------------
 * Sets of processors.
 * ------------------------------------------------------------------------- */

void tb_linux_cpu_set_add(tb_linux_cpu_set *set, size_t cpu)
{
    set->words[cpu / TB_LINUX_CPU_SET_WORD_BITS] |= UINT64_C(1)
                                                    << (cpu % TB_LINUX_CPU_SET_WORD_BITS);
}

bool tb_linux_cpu_set_has(const tb_linux_cpu_set *set, size_t cpu)
{
    if (cpu >= TB_LINUX_CPU_LIMIT)
    {
        return false;
    }
    return ((set->words[cpu / TB_LINUX_CPU_SET_WORD_BITS] >> (cpu % TB_LINUX_CPU_SET_WORD_BITS)) &
            UINT64_C(1)) != 0;
}

static void tb_linux_cpu_set_clear(tb_linux_cpu_set *set)
{
    const tb_linux_cpu_set empty = {0};

    *set = empty;
}

/* ---------------------------------------------------------------------------
 * The kernel's text.
 * ------------------------------------------------------------------------- */

static bool tb_linux_is_digit(char character)
{
    return character >= '0' && character <= '9';
}

/* Reads a decimal number at `*cursor` and moves the cursor past it. Returns
 * false when there is no digit there, or the number is `limit` or more. */
static bool tb_linux_parse_number(const char **cursor, size_t limit, size_t *number)
{
    const char *at = *cursor;
    size_t value = 0;

    if (!tb_linux_is_digit(*at))
    {
        return false;
    }
    while (tb_linux_is_digit(*at))
    {
        /* `value` is below `limit` here, so the product cannot overflow for
         * any limit this file uses. */
        value = value * TB_LINUX_DECIMAL_BASE + (size_t)(*at - '0');
        if (value >= limit)
        {
            return false;
        }
        at++;
    }
    *cursor = at;
    *number = value;
    return true;
}

/* Whether nothing but the newline that the kernel ends a file with is left. */
static bool tb_linux_is_at_end(const char *at)
{
    if (*at == '\n')
    {
        at++;
    }
    return *at == '\0';
}

static bool tb_linux_parse_ranges(const char *text, tb_linux_cpu_set *set)
{
    const char *at = text;

    if (tb_linux_is_at_end(at))
    {
        return true;
    }
    for (;;)
    {
        size_t first = 0;
        size_t last = 0;

        if (!tb_linux_parse_number(&at, TB_LINUX_CPU_LIMIT, &first))
        {
            return false;
        }
        last = first;
        if (*at == '-')
        {
            at++;
            if (!tb_linux_parse_number(&at, TB_LINUX_CPU_LIMIT, &last) || last < first)
            {
                return false;
            }
        }
        for (size_t cpu = first; cpu <= last; cpu++)
        {
            tb_linux_cpu_set_add(set, cpu);
        }
        if (*at != ',')
        {
            return tb_linux_is_at_end(at);
        }
        at++;
    }
}

bool tb_linux_cpu_list_parse(const char *text, tb_linux_cpu_set *set)
{
    tb_linux_cpu_set_clear(set);
    if (!tb_linux_parse_ranges(text, set))
    {
        tb_linux_cpu_set_clear(set);
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Paths of the kernel's files.
 * ------------------------------------------------------------------------- */

/* Text being built in a buffer of fixed capacity. Text that does not fit
 * marks the whole as overflowed, and nothing is written past the end. */
typedef struct tb_linux_path
{
    char text[TB_LINUX_PATH_CAPACITY];
    size_t length;
    bool has_overflowed;
} tb_linux_path;

static void tb_linux_path_append_character(tb_linux_path *path, char character)
{
    /* One byte is kept for the terminator. */
    if (path->length + 1 >= sizeof(path->text))
    {
        path->has_overflowed = true;
        return;
    }
    path->text[path->length] = character;
    path->length++;
    path->text[path->length] = '\0';
}

static void tb_linux_path_append(tb_linux_path *path, const char *text)
{
    for (const char *at = text; *at != '\0'; at++)
    {
        tb_linux_path_append_character(path, *at);
    }
}

static void tb_linux_path_append_number(tb_linux_path *path, size_t number)
{
    /* Digits of a number below TB_LINUX_CPU_LIMIT, least significant first. */
    char digits[TB_LINUX_CPU_DIGITS_MAX] = {0};
    size_t digit_count = 0;

    do
    {
        digits[digit_count] = (char)('0' + (char)(number % TB_LINUX_DECIMAL_BASE));
        digit_count++;
        number /= TB_LINUX_DECIMAL_BASE;
    } while (number != 0 && digit_count < sizeof(digits));
    while (digit_count > 0)
    {
        digit_count--;
        tb_linux_path_append_character(path, digits[digit_count]);
    }
}

/* Reads the file `leaf` of logical processor `cpu`. */
static bool tb_linux_read_cpu_file(tb_linux_read_file *read, void *context, size_t cpu,
                                   const char *leaf, char *buffer, size_t capacity)
{
    tb_linux_path path = {0};

    tb_linux_path_append(&path, "/sys/devices/system/cpu/cpu");
    tb_linux_path_append_number(&path, cpu);
    tb_linux_path_append_character(&path, '/');
    tb_linux_path_append(&path, leaf);
    if (path.has_overflowed)
    {
        return false;
    }
    return read(context, path.text, buffer, capacity);
}

/* ---------------------------------------------------------------------------
 * Listing and counting.
 *
 * Nothing here keeps a list of its own: a thread with a small stack must be
 * able to count, and a library's thread-local storage is placed on the stack
 * of every thread.
 * ------------------------------------------------------------------------- */

/* The lowest of the logical processors of `cpu`'s core that the process may
 * use: `cpu` itself when it is the first of its core that is allowed. A
 * processor whose siblings the kernel does not list is a core by itself. */
static size_t tb_linux_first_allowed_of_its_core(const tb_linux_cpu_set *allowed, size_t cpu,
                                                 tb_linux_read_file *read, void *context)
{
    char text[TB_LINUX_FILE_CAPACITY] = {0};
    tb_linux_cpu_set siblings = {0};

    if (!tb_linux_read_cpu_file(read, context, cpu, "topology/thread_siblings_list", text,
                                sizeof(text)))
    {
        return cpu;
    }
    if (!tb_linux_cpu_list_parse(text, &siblings) || !tb_linux_cpu_set_has(&siblings, cpu))
    {
        return cpu;
    }
    for (size_t lower = 0; lower < cpu; lower++)
    {
        if (tb_linux_cpu_set_has(&siblings, lower) && tb_linux_cpu_set_has(allowed, lower))
        {
            return lower;
        }
    }
    return cpu;
}

size_t tb_linux_cpu_set_rank(const tb_linux_cpu_set *set, size_t cpu)
{
    size_t rank = 0;

    for (size_t lower = 0; lower < cpu && lower < TB_LINUX_CPU_LIMIT; lower++)
    {
        if (tb_linux_cpu_set_has(set, lower))
        {
            rank++;
        }
    }
    return rank;
}

size_t tb_linux_cpu_set_nth(const tb_linux_cpu_set *set, size_t place)
{
    size_t seen = 0;

    for (size_t cpu = 0; cpu < TB_LINUX_CPU_LIMIT; cpu++)
    {
        if (!tb_linux_cpu_set_has(set, cpu))
        {
            continue;
        }
        if (seen == place)
        {
            return cpu;
        }
        seen++;
    }
    return TB_LINUX_CPU_LIMIT;
}

/* Intel processors with two kinds of core list the logical processors of the
 * fast kind in one file. Puts the cores among `first_of_core` that are in it
 * into `fast`; false when the file is not there, as on every other machine. */
static bool tb_linux_find_listed_fast_cores(const tb_linux_cpu_set *first_of_core,
                                            tb_linux_read_file *read, void *context,
                                            tb_linux_cpu_set *fast)
{
    char text[TB_LINUX_FILE_CAPACITY] = {0};
    tb_linux_cpu_set listed = {0};

    if (!read(context, "/sys/devices/cpu_core/cpus", text, sizeof(text)) ||
        !tb_linux_cpu_list_parse(text, &listed))
    {
        return false;
    }
    for (size_t cpu = 0; cpu < TB_LINUX_CPU_LIMIT; cpu++)
    {
        if (tb_linux_cpu_set_has(first_of_core, cpu) && tb_linux_cpu_set_has(&listed, cpu))
        {
            tb_linux_cpu_set_add(fast, cpu);
        }
    }
    return true;
}

/* Capacities are at most 1024 today; the limit only bounds the arithmetic. */
#define TB_LINUX_CAPACITY_LIMIT ((size_t)1000000)

/* Reads the capacity of `cpu`; false when it cannot be read or understood. */
static bool tb_linux_read_capacity(tb_linux_read_file *read, void *context, size_t cpu,
                                   size_t *capacity)
{
    char text[TB_LINUX_FILE_CAPACITY] = {0};
    const char *at = text;

    return tb_linux_read_cpu_file(read, context, cpu, "cpu_capacity", text, sizeof(text)) &&
           tb_linux_parse_number(&at, TB_LINUX_CAPACITY_LIMIT, capacity) && tb_linux_is_at_end(at);
}

/* Arm machines with several kinds of core give every logical processor a
 * capacity, highest for the fastest kind. Puts the cores among
 * `first_of_core` that have the highest into `fast`; false when a capacity
 * cannot be read, as on machines whose cores are all alike. */
static bool tb_linux_find_highest_capacity_cores(const tb_linux_cpu_set *first_of_core,
                                                 tb_linux_read_file *read, void *context,
                                                 tb_linux_cpu_set *fast)
{
    size_t highest = 0;
    bool has_any = false;

    for (size_t cpu = 0; cpu < TB_LINUX_CPU_LIMIT; cpu++)
    {
        size_t capacity = 0;

        if (!tb_linux_cpu_set_has(first_of_core, cpu))
        {
            continue;
        }
        if (!tb_linux_read_capacity(read, context, cpu, &capacity))
        {
            return false;
        }
        if (!has_any || capacity > highest)
        {
            highest = capacity;
        }
        has_any = true;
    }
    /* Read again, which keeps no table of capacities. */
    for (size_t cpu = 0; cpu < TB_LINUX_CPU_LIMIT; cpu++)
    {
        size_t capacity = 0;

        if (tb_linux_cpu_set_has(first_of_core, cpu) &&
            tb_linux_read_capacity(read, context, cpu, &capacity) && capacity == highest)
        {
            tb_linux_cpu_set_add(fast, cpu);
        }
    }
    return has_any;
}

/* Whether a set has any member. */
static bool tb_linux_cpu_set_is_empty(const tb_linux_cpu_set *set)
{
    for (size_t word = 0; word < TB_LINUX_CPU_LIMIT / TB_LINUX_CPU_SET_WORD_BITS; word++)
    {
        if (set->words[word] != 0)
        {
            return false;
        }
    }
    return true;
}

size_t tb_linux_list_processors(const tb_linux_cpu_set *allowed, tb_linux_read_file *read,
                                void *context, tb_system_processor *list, size_t capacity,
                                tb_system_processors *counts)
{
    /* For each allowed processor, the first allowed processor of its core. */
    uint16_t first_of[TB_LINUX_CPU_LIMIT] = {0};
    tb_linux_cpu_set first_of_core = {0};
    tb_linux_cpu_set fast = {0};
    size_t count = 0;
    size_t core_count = 0;
    size_t fast_count = 0;

    for (size_t cpu = 0; cpu < TB_LINUX_CPU_LIMIT; cpu++)
    {
        if (!tb_linux_cpu_set_has(allowed, cpu))
        {
            continue;
        }
        first_of[cpu] = (uint16_t)tb_linux_first_allowed_of_its_core(allowed, cpu, read, context);
        if (first_of[cpu] == cpu)
        {
            tb_linux_cpu_set_add(&first_of_core, cpu);
            core_count++;
        }
    }
    /* The fast kind: the cores that the kernel lists as fast, or failing a
     * list, those of the highest capacity. Where the system says neither,
     * or no core of the fast kind is allowed, the cores that are allowed are
     * taken to be alike, and all of the fastest kind there is. */
    if (!tb_linux_find_listed_fast_cores(&first_of_core, read, context, &fast))
    {
        tb_linux_cpu_set_clear(&fast);
        if (!tb_linux_find_highest_capacity_cores(&first_of_core, read, context, &fast))
        {
            tb_linux_cpu_set_clear(&fast);
        }
    }
    if (tb_linux_cpu_set_is_empty(&fast))
    {
        fast = first_of_core;
    }
    for (size_t cpu = 0; cpu < TB_LINUX_CPU_LIMIT; cpu++)
    {
        if (!tb_linux_cpu_set_has(allowed, cpu))
        {
            continue;
        }
        if (count < capacity)
        {
            list[count].core = tb_linux_cpu_set_rank(&first_of_core, first_of[cpu]);
            list[count].is_performance = tb_linux_cpu_set_has(&fast, first_of[cpu]);
        }
        if (first_of[cpu] == cpu && tb_linux_cpu_set_has(&fast, cpu))
        {
            fast_count++;
        }
        count++;
    }
    if (counts != NULL)
    {
        counts->logical_count = count;
        counts->core_count = core_count;
        counts->performance_core_count = fast_count;
    }
    return count;
}

void tb_linux_count_processors(const tb_linux_cpu_set *allowed, tb_linux_read_file *read,
                               void *context, tb_system_processors *processors)
{
    (void)tb_linux_list_processors(allowed, read, context, NULL, 0, processors);
}
