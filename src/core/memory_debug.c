/* The checking memory functions.
 *
 * What a caller is given lies inside a larger block from the underlying
 * functions:
 *
 *     record | 16 guard bytes | the caller's bytes | 16 guard bytes
 *
 * The record says what the block is, and links it into two structures: the
 * list of every allocated block from the oldest to the newest, which is what
 * a report walks, and a hash table from the caller's address to the record.
 *
 * The table is what makes a wrong pointer safe to look at. A pointer that is
 * freed or resized is looked up, never read through, so one that is no block
 * is found out without touching whatever it points to.
 *
 * One lock guards all of it and is held for the whole of every call, the
 * underlying allocation included. A fault is reported after the lock is
 * released, so that a handler may leave by a non-local jump. */
#include "badger/memory_debug.h"

#include "badger/assert.h"
#include "badger/fatal.h"
#include "badger/fmt.h"
#include "badger/hash.h"
#include "badger/platform.h"
#include "badger/size.h"
#include "badger/str.h"
#include "badger/thread.h"

#include <string.h>

/* Bytes on each side of a block that nothing should write. A multiple of
 * every fundamental alignment. */
#define TB_MEMORY_DEBUG_GUARD_SIZE ((size_t)16)

/* What the guard bytes hold, what new memory is filled with, and what freed
 * memory is filled with. None is a likely value of anything, and each is odd,
 * so that none is a valid address of an object either. */
#define TB_MEMORY_DEBUG_GUARD_BYTE 0xFD
#define TB_MEMORY_DEBUG_FRESH_BYTE 0xCD
#define TB_MEMORY_DEBUG_FREED_BYTE 0xDD

/* Buckets of the first table. A power of two, like every later one, so that
 * a hash is reduced to a bucket by masking. The table doubles when there are
 * more blocks than buckets. */
#define TB_MEMORY_DEBUG_FIRST_BUCKET_COUNT ((size_t)1024)

/* Blocks that are remembered after they are freed, so that freeing one of
 * them again can be told from freeing a pointer that never was a block. */
#define TB_MEMORY_DEBUG_FREED_CAPACITY ((size_t)1024)

/* The most labels and the most blocks that a report lists one by one. */
#define TB_MEMORY_DEBUG_REPORT_LABELS_MAX ((size_t)32)
#define TB_MEMORY_DEBUG_REPORT_BLOCKS_MAX ((size_t)32)

typedef struct tb_memory_debug_record
{
    struct tb_memory_debug_record *next_in_bucket; /* The next with the same hash, or NULL. */
    struct tb_memory_debug_record *older;          /* The block allocated before, or NULL. */
    struct tb_memory_debug_record *newer;          /* The block allocated after, or NULL. */
    const char *label;                             /* Of the thread that allocated it, or NULL. */
    const char *file;
    size_t line;
    uint64_t number; /* Its place in the order of allocation. */
    size_t size;     /* The bytes that the caller asked for. */
} tb_memory_debug_record;

/* From the start of what the underlying functions return to the caller's
 * bytes. It keeps the alignment that the underlying functions give. */
#define TB_MEMORY_DEBUG_BLOCK_OFFSET (sizeof(tb_memory_debug_record) + TB_MEMORY_DEBUG_GUARD_SIZE)

TB_STATIC_ASSERT(TB_MEMORY_DEBUG_BLOCK_OFFSET % TB_MEMORY_DEBUG_GUARD_SIZE == 0,
                 "the caller's bytes must be as aligned as the underlying block");

typedef struct tb_memory_debug_state
{
    tb_mutex lock; /* Guards everything below but the handler. */
    /* What does the allocating. Its members are NULL until the first use,
     * which takes the engine's built-in functions. */
    tb_memory_functions underlying;
    tb_memory_debug_record **buckets; /* The table, or NULL before the first block. */
    size_t bucket_count;              /* 0 or a power of two. */
    tb_memory_debug_record *oldest;   /* The allocated blocks, in order; NULL when none. */
    tb_memory_debug_record *newest;
    tb_memory_debug_statistics statistics;
    uint64_t watched; /* The number given to tb_memory_debug_watch(); 0 for none. */
    /* The blocks freed last, as they were. The next goes at `freed_next`; `freed_count` is bounded
     * by capacity. */
    tb_memory_debug_block freed[TB_MEMORY_DEBUG_FREED_CAPACITY];
    size_t freed_count;
    size_t freed_next;
    tb_memory_debug_handler *handler; /* NULL selects the default report. */
    void *handler_context;
} tb_memory_debug_state;

static tb_memory_debug_state tb_memory_debug = {0};

/* The label of the calling thread. */
static _Thread_local const char *tb_memory_debug_label = NULL;
static _Thread_local tb_memory_debug_site tb_memory_debug_location = {0};

/* ---------------------------------------------------------------------------
 * Faults.
 * ------------------------------------------------------------------------- */

static const char *tb_memory_debug_label_text(const char *label)
{
    if (label == NULL)
    {
        return "none";
    }
    return label;
}

/* The default report of a finding: a fatal condition like any other, so that
 * it is written with the call stack and goes to the handler of
 * badger/fatal.h. */
static _Noreturn void tb_memory_debug_report_as_fatal(const tb_memory_debug_finding *finding)
{
    const tb_memory_debug_block *const block = &finding->block;
    const unsigned long long number = block->number;
    const char *const label = tb_memory_debug_label_text(block->label);
    char fault[TB_FATAL_MESSAGE_CAPACITY] = {0};

    switch (finding->fault)
    {
    case TB_MEMORY_DEBUG_NOT_A_BLOCK:
        (void)tb_fmt_buffer(
            fault, sizeof(fault),
            "badger: memory fault: %p was freed or resized, and is not an allocated block",
            block->memory);
        break;
    case TB_MEMORY_DEBUG_FREED_TWICE:
        (void)tb_fmt_buffer(fault, sizeof(fault),
                            "badger: memory fault: block %llu was freed or resized after it was "
                            "freed (bytes %zu, at %p, label %s)",
                            number, block->size, block->memory, label);
        break;
    case TB_MEMORY_DEBUG_WRITTEN_BEFORE:
        (void)tb_fmt_buffer(fault, sizeof(fault),
                            "badger: memory fault: the byte %zu before block %llu was written "
                            "(bytes %zu, at %p, label %s)",
                            finding->distance, number, block->size, block->memory, label);
        break;
    case TB_MEMORY_DEBUG_WRITTEN_AFTER:
        (void)tb_fmt_buffer(fault, sizeof(fault),
                            "badger: memory fault: the byte %zu after block %llu was written "
                            "(bytes %zu, at %p, label %s)",
                            finding->distance, number, block->size, block->memory, label);
        break;
    case TB_MEMORY_DEBUG_WATCHED:
        (void)tb_fmt_buffer(
            fault, sizeof(fault),
            "badger: memory: block %llu is allocated here (bytes %zu, at %p, label %s)", number,
            block->size, block->memory, label);
        break;
    }
    if (block->file != NULL)
    {
        tb_fatal("%s; allocated at %s:%zu", fault, block->file, block->line);
    }
    tb_fatal_message(fault);
}

/* Reports a finding and ends the process. The caller does not hold the lock. */
static _Noreturn void tb_memory_debug_fail(const tb_memory_debug_finding *finding)
{
    if (tb_memory_debug.handler != NULL)
    {
        tb_memory_debug.handler(finding, tb_memory_debug.handler_context);
    }
    else
    {
        tb_memory_debug_report_as_fatal(finding);
    }
    /* Reached by a handler that returned: memory that has been misused
     * cannot be relied on. */
    (void)tb_fmt_flush();
    tb_platform_abort();
}

void tb_memory_debug_set_handler(tb_memory_debug_handler *handler, void *context)
{
    tb_memory_debug.handler = handler;
    tb_memory_debug.handler_context = context;
}

/* ---------------------------------------------------------------------------
 * Records. The caller of every function from here to the checking functions
 * holds the lock.
 * ------------------------------------------------------------------------- */

static const tb_memory_functions *tb_memory_debug_underlying(void)
{
    if (tb_memory_debug.underlying.malloc_function == NULL)
    {
        tb_memory_get_original_functions(&tb_memory_debug.underlying);
    }
    return &tb_memory_debug.underlying;
}

static const unsigned char *tb_memory_debug_guard_before(const tb_memory_debug_record *record)
{
    return (const unsigned char *)(record + 1);
}

static const unsigned char *tb_memory_debug_block_of(const tb_memory_debug_record *record)
{
    return (const unsigned char *)record + TB_MEMORY_DEBUG_BLOCK_OFFSET;
}

static const unsigned char *tb_memory_debug_guard_after(const tb_memory_debug_record *record)
{
    return tb_memory_debug_block_of(record) + record->size;
}

static tb_memory_debug_block tb_memory_debug_describe(const tb_memory_debug_record *record)
{
    const tb_memory_debug_block block = {.memory = tb_memory_debug_block_of(record),
                                         .size = record->size,
                                         .number = record->number,
                                         .label = record->label,
                                         .file = record->file,
                                         .line = record->line};

    return block;
}

/* The bucket for an address. There is a table. */
static tb_memory_debug_record **tb_memory_debug_bucket_of(const void *memory)
{
    const size_t mask = tb_memory_debug.bucket_count - 1;

    return &tb_memory_debug.buckets[(size_t)tb_hash_pointer(memory) & mask];
}

static void tb_memory_debug_bucket_add(tb_memory_debug_record *record)
{
    tb_memory_debug_record **const bucket =
        tb_memory_debug_bucket_of(tb_memory_debug_block_of(record));

    record->next_in_bucket = *bucket;
    *bucket = record;
}

/* Takes a record that is in the table out of it. */
static void tb_memory_debug_bucket_remove(tb_memory_debug_record *record)
{
    tb_memory_debug_record **link = tb_memory_debug_bucket_of(tb_memory_debug_block_of(record));

    while (*link != record)
    {
        link = &(*link)->next_in_bucket;
    }
    *link = record->next_in_bucket;
}

/* The record of the allocated block at an address, or NULL when there is
 * none. The address is compared and never read through. */
static tb_memory_debug_record *tb_memory_debug_find(const void *memory)
{
    tb_memory_debug_record *record = NULL;

    if (tb_memory_debug.buckets == NULL)
    {
        return NULL;
    }
    record = *tb_memory_debug_bucket_of(memory);
    while (record != NULL && tb_memory_debug_block_of(record) != memory)
    {
        record = record->next_in_bucket;
    }
    return record;
}

static void tb_memory_debug_forget_table(void)
{
    if (tb_memory_debug.buckets != NULL)
    {
        tb_memory_debug_underlying()->free_function(tb_memory_debug.buckets);
    }
    tb_memory_debug.buckets = NULL;
    tb_memory_debug.bucket_count = 0;
}

/* Makes the table large enough for one more block. Returns false only when
 * there is no table and none could be allocated; a table that could not be
 * made larger serves as it is, more slowly. */
static bool tb_memory_debug_make_room(void)
{
    tb_memory_debug_record **buckets = NULL;
    size_t count = TB_MEMORY_DEBUG_FIRST_BUCKET_COUNT;
    size_t bytes = 0;
    const bool has_table = tb_memory_debug.buckets != NULL;

    if (has_table && tb_memory_debug.statistics.live_blocks < tb_memory_debug.bucket_count)
    {
        return true;
    }
    if (has_table && !tb_size_multiply(tb_memory_debug.bucket_count, 2, &count))
    {
        return true;
    }
    if (tb_size_multiply(count, sizeof(*buckets), &bytes))
    {
        buckets = tb_memory_debug_underlying()->calloc_function(count, sizeof(*buckets));
    }
    if (buckets == NULL)
    {
        return has_table;
    }
    tb_memory_debug_forget_table();
    tb_memory_debug.buckets = buckets;
    tb_memory_debug.bucket_count = count;
    for (tb_memory_debug_record *record = tb_memory_debug.oldest; record != NULL;
         record = record->newer)
    {
        tb_memory_debug_bucket_add(record);
    }
    return true;
}

static void tb_memory_debug_note_peaks(void)
{
    tb_memory_debug_statistics *const statistics = &tb_memory_debug.statistics;

    if (statistics->live_blocks > statistics->peak_blocks)
    {
        statistics->peak_blocks = statistics->live_blocks;
    }
    if (statistics->live_bytes > statistics->peak_bytes)
    {
        statistics->peak_bytes = statistics->live_bytes;
    }
}

/* Allocates a block of `size` bytes and records it as the newest. Returns
 * its record, or NULL when the underlying functions have no memory for it. */
static tb_memory_debug_record *tb_memory_debug_allocate(size_t size, bool is_zeroed)
{
    const tb_memory_functions *const underlying = tb_memory_debug_underlying();
    tb_memory_debug_record *record = NULL;
    size_t total = 0;

    if (tb_memory_debug.statistics.allocated_blocks == UINT64_MAX)
    {
        return NULL;
    }
    if (!tb_size_add(size, TB_MEMORY_DEBUG_BLOCK_OFFSET + TB_MEMORY_DEBUG_GUARD_SIZE, &total))
    {
        return NULL;
    }
    if (!tb_memory_debug_make_room())
    {
        return NULL;
    }
    if (is_zeroed)
    {
        record = underlying->calloc_function(1, total);
    }
    else
    {
        record = underlying->malloc_function(total);
    }
    if (record == NULL)
    {
        return NULL;
    }
    *record = (tb_memory_debug_record){0};
    record->file = tb_memory_debug_location.file;
    record->line = tb_memory_debug_location.line;
    record->size = size;
    record->label = tb_memory_debug_label;
    memset((unsigned char *)(record + 1), TB_MEMORY_DEBUG_GUARD_BYTE, TB_MEMORY_DEBUG_GUARD_SIZE);
    if (!is_zeroed)
    {
        memset((unsigned char *)record + TB_MEMORY_DEBUG_BLOCK_OFFSET, TB_MEMORY_DEBUG_FRESH_BYTE,
               size);
    }
    memset((unsigned char *)record + TB_MEMORY_DEBUG_BLOCK_OFFSET + record->size,
           TB_MEMORY_DEBUG_GUARD_BYTE, TB_MEMORY_DEBUG_GUARD_SIZE);

    tb_memory_debug.statistics.allocated_blocks++;
    record->number = tb_memory_debug.statistics.allocated_blocks;
    record->newer = NULL;
    record->older = tb_memory_debug.newest;
    if (tb_memory_debug.newest != NULL)
    {
        tb_memory_debug.newest->newer = record;
    }
    else
    {
        tb_memory_debug.oldest = record;
    }
    tb_memory_debug.newest = record;
    tb_memory_debug_bucket_add(record);

    /* The sizes of the blocks that are allocated cannot add up to more than
     * the address space, so the sum does not overflow. */
    tb_memory_debug.statistics.live_blocks++;
    tb_memory_debug.statistics.live_bytes += size;
    tb_memory_debug_note_peaks();
    return record;
}

/* Tells the neighbours of a record in the list where it is: after it was
 * moved, or to take it out when `record` is given its own neighbours. */
static void tb_memory_debug_join(tb_memory_debug_record *older, tb_memory_debug_record *newer,
                                 tb_memory_debug_record *between)
{
    tb_memory_debug_record *after_older = between;
    tb_memory_debug_record *before_newer = between;

    if (between == NULL)
    {
        after_older = newer;
        before_newer = older;
    }
    if (older != NULL)
    {
        older->newer = after_older;
    }
    else
    {
        tb_memory_debug.oldest = after_older;
    }
    if (newer != NULL)
    {
        newer->older = before_newer;
    }
    else
    {
        tb_memory_debug.newest = before_newer;
    }
}

/* Frees a block that was found to be sound, and remembers that it was. */
static void tb_memory_debug_release(tb_memory_debug_record *record)
{
    const size_t index = tb_memory_debug.freed_next;

    tb_memory_debug_bucket_remove(record);
    tb_memory_debug_join(record->older, record->newer, NULL);
    tb_memory_debug.freed[index] = tb_memory_debug_describe(record);
    tb_memory_debug.freed_next = (index + 1) % TB_MEMORY_DEBUG_FREED_CAPACITY;
    if (tb_memory_debug.freed_count < TB_MEMORY_DEBUG_FREED_CAPACITY)
    {
        tb_memory_debug.freed_count++;
    }
    tb_memory_debug.statistics.live_blocks--;
    tb_memory_debug.statistics.live_bytes -= record->size;
    memset((unsigned char *)record + TB_MEMORY_DEBUG_BLOCK_OFFSET, TB_MEMORY_DEBUG_FREED_BYTE,
           record->size);
    tb_memory_debug_underlying()->free_function(record);
}

/* Resizes a block that was found to be sound. Returns the caller's new
 * address, or NULL, with the block as it was, when the underlying functions
 * have no memory for it. */
static void *tb_memory_debug_resize(tb_memory_debug_record *record, size_t size)
{
    const size_t old_size = record->size;
    tb_memory_debug_record *moved = NULL;
    size_t total = 0;

    if (!tb_size_add(size, TB_MEMORY_DEBUG_BLOCK_OFFSET + TB_MEMORY_DEBUG_GUARD_SIZE, &total))
    {
        return NULL;
    }
    /* Out of the table first: its place there is its address, which the
     * underlying functions may be about to change. */
    tb_memory_debug_bucket_remove(record);
    moved = tb_memory_debug_underlying()->realloc_function(record, total);
    if (moved == NULL)
    {
        tb_memory_debug_bucket_add(record);
        return NULL;
    }
    /* The record moved with its block, and still names its neighbours. */
    tb_memory_debug_join(moved->older, moved->newer, moved);
    moved->size = size;
    if (size > old_size)
    {
        memset((unsigned char *)moved + TB_MEMORY_DEBUG_BLOCK_OFFSET + old_size,
               TB_MEMORY_DEBUG_FRESH_BYTE, size - old_size);
    }
    memset((unsigned char *)moved + TB_MEMORY_DEBUG_BLOCK_OFFSET + moved->size,
           TB_MEMORY_DEBUG_GUARD_BYTE, TB_MEMORY_DEBUG_GUARD_SIZE);
    tb_memory_debug_bucket_add(moved);
    tb_memory_debug.statistics.live_bytes -= old_size;
    tb_memory_debug.statistics.live_bytes += size;
    tb_memory_debug_note_peaks();
    return (unsigned char *)moved + TB_MEMORY_DEBUG_BLOCK_OFFSET;
}

/* ---------------------------------------------------------------------------
 * Looking for faults. The caller holds the lock.
 * ------------------------------------------------------------------------- */

/* How far before a block the nearest written guard byte is; 0 when none was
 * written. */
static size_t tb_memory_debug_written_before(const tb_memory_debug_record *record)
{
    const unsigned char *const guard = tb_memory_debug_guard_before(record);

    for (size_t distance = 1; distance <= TB_MEMORY_DEBUG_GUARD_SIZE; distance++)
    {
        if (guard[TB_MEMORY_DEBUG_GUARD_SIZE - distance] != TB_MEMORY_DEBUG_GUARD_BYTE)
        {
            return distance;
        }
    }
    return 0;
}

/* The same for the bytes after a block. */
static size_t tb_memory_debug_written_after(const tb_memory_debug_record *record)
{
    const unsigned char *const guard = tb_memory_debug_guard_after(record);

    for (size_t distance = 1; distance <= TB_MEMORY_DEBUG_GUARD_SIZE; distance++)
    {
        if (guard[distance - 1] != TB_MEMORY_DEBUG_GUARD_BYTE)
        {
            return distance;
        }
    }
    return 0;
}

/* Whether a guard byte of a block was written, and if so what to report. */
static bool tb_memory_debug_is_damaged(const tb_memory_debug_record *record,
                                       tb_memory_debug_finding *finding)
{
    tb_memory_debug_fault fault = TB_MEMORY_DEBUG_WRITTEN_BEFORE;
    size_t distance = tb_memory_debug_written_before(record);

    if (distance == 0)
    {
        fault = TB_MEMORY_DEBUG_WRITTEN_AFTER;
        distance = tb_memory_debug_written_after(record);
    }
    if (distance == 0)
    {
        return false;
    }
    finding->fault = fault;
    finding->block = tb_memory_debug_describe(record);
    finding->distance = distance;
    return true;
}

/* What to report for an address that is not an allocated block: a block that
 * was freed, when it is remembered as one, and a stranger otherwise. */
static tb_memory_debug_finding tb_memory_debug_explain(const void *memory)
{
    tb_memory_debug_finding finding = {.fault = TB_MEMORY_DEBUG_NOT_A_BLOCK,
                                       .block = {.memory = memory}};
    size_t remembered = tb_memory_debug.freed_count;

    if (remembered > TB_MEMORY_DEBUG_FREED_CAPACITY)
    {
        remembered = TB_MEMORY_DEBUG_FREED_CAPACITY;
    }
    /* The one freed last first: an address may have been a block many times. */
    for (size_t age = 1; age <= remembered; age++)
    {
        const size_t index = (tb_memory_debug.freed_next + TB_MEMORY_DEBUG_FREED_CAPACITY - age) %
                             TB_MEMORY_DEBUG_FREED_CAPACITY;

        if (tb_memory_debug.freed[index].memory == memory)
        {
            finding.fault = TB_MEMORY_DEBUG_FREED_TWICE;
            finding.block = tb_memory_debug.freed[index];
            return finding;
        }
    }
    return finding;
}

/* Finds the allocated block at an address and looks at its guard bytes.
 * Returns its record, or NULL with `*finding` set when the address is no
 * block or the block is damaged. */
static tb_memory_debug_record *tb_memory_debug_find_sound(const void *memory,
                                                          tb_memory_debug_finding *finding)
{
    tb_memory_debug_record *const record = tb_memory_debug_find(memory);

    if (record == NULL)
    {
        *finding = tb_memory_debug_explain(memory);
        return NULL;
    }
    if (tb_memory_debug_is_damaged(record, finding))
    {
        return NULL;
    }
    return record;
}

/* ---------------------------------------------------------------------------
 * The checking functions.
 * ------------------------------------------------------------------------- */

static void *tb_memory_debug_new(size_t size, bool is_zeroed)
{
    tb_memory_debug_finding finding = {.fault = TB_MEMORY_DEBUG_WATCHED};
    tb_memory_debug_record *record = NULL;
    void *memory = NULL;
    bool is_watched = false;

    tb_mutex_lock(&tb_memory_debug.lock);
    record = tb_memory_debug_allocate(size, is_zeroed);
    if (record != NULL)
    {
        memory = (unsigned char *)record + TB_MEMORY_DEBUG_BLOCK_OFFSET;
        is_watched = record->number == tb_memory_debug.watched;
        finding.block = tb_memory_debug_describe(record);
    }
    tb_mutex_unlock(&tb_memory_debug.lock);
    if (is_watched)
    {
        tb_memory_debug_fail(&finding);
    }
    return memory;
}

static void *tb_memory_debug_malloc(size_t size)
{
    return tb_memory_debug_new(size, false);
}

static void *tb_memory_debug_calloc(size_t count, size_t size)
{
    size_t bytes = 0;

    /* The engine has checked the product; a direct caller may not have. */
    if (!tb_size_multiply(count, size, &bytes))
    {
        return NULL;
    }
    return tb_memory_debug_new(bytes, true);
}

static void *tb_memory_debug_realloc(void *memory, size_t size)
{
    tb_memory_debug_finding finding = {0};
    tb_memory_debug_record *record = NULL;
    bool is_sound = false;
    void *resized = NULL;

    tb_mutex_lock(&tb_memory_debug.lock);
    record = tb_memory_debug_find_sound(memory, &finding);
    is_sound = record != NULL;
    if (is_sound)
    {
        resized = tb_memory_debug_resize(record, size);
    }
    tb_mutex_unlock(&tb_memory_debug.lock);
    if (!is_sound)
    {
        tb_memory_debug_fail(&finding);
    }
    return resized;
}

static void tb_memory_debug_free(void *memory)
{
    tb_memory_debug_finding finding = {0};
    tb_memory_debug_record *record = NULL;
    bool is_sound = false;

    tb_mutex_lock(&tb_memory_debug.lock);
    record = tb_memory_debug_find_sound(memory, &finding);
    is_sound = record != NULL;
    if (is_sound)
    {
        tb_memory_debug_release(record);
    }
    tb_mutex_unlock(&tb_memory_debug.lock);
    if (!is_sound)
    {
        tb_memory_debug_fail(&finding);
    }
}

void tb_memory_debug_get_functions(tb_memory_functions *functions)
{
    TB_ASSERT(functions != NULL);
    functions->malloc_function = tb_memory_debug_malloc;
    functions->calloc_function = tb_memory_debug_calloc;
    functions->realloc_function = tb_memory_debug_realloc;
    functions->free_function = tb_memory_debug_free;
}

static bool tb_memory_debug_is_complete(const tb_memory_functions *functions)
{
    return functions->malloc_function != NULL && functions->calloc_function != NULL &&
           functions->realloc_function != NULL && functions->free_function != NULL &&
           functions->malloc_function != tb_memory_debug_malloc &&
           functions->calloc_function != tb_memory_debug_calloc &&
           functions->realloc_function != tb_memory_debug_realloc &&
           functions->free_function != tb_memory_debug_free;
}

bool tb_memory_debug_set_underlying(const tb_memory_functions *functions)
{
    const tb_memory_functions built_in_at_first_use = {0};
    bool is_taken = false;

    if (functions != NULL && !tb_memory_debug_is_complete(functions))
    {
        return false;
    }
    tb_mutex_lock(&tb_memory_debug.lock);
    if (tb_memory_debug.statistics.live_blocks == 0)
    {
        /* The table goes back to the functions that it came from. */
        tb_memory_debug_forget_table();
        tb_memory_debug.underlying = built_in_at_first_use;
        if (functions != NULL)
        {
            tb_memory_debug.underlying = *functions;
        }
        is_taken = true;
    }
    tb_mutex_unlock(&tb_memory_debug.lock);
    return is_taken;
}

bool tb_memory_debug_reset(void)
{
    const tb_memory_debug_statistics nothing = {0};
    bool is_reset = false;

    tb_mutex_lock(&tb_memory_debug.lock);
    if (tb_memory_debug.statistics.live_blocks == 0)
    {
        tb_memory_debug_forget_table();
        tb_memory_debug.statistics = nothing;
        tb_memory_debug.watched = 0;
        tb_memory_debug.freed_count = 0;
        tb_memory_debug.freed_next = 0;
        is_reset = true;
    }
    tb_mutex_unlock(&tb_memory_debug.lock);
    return is_reset;
}

/* ---------------------------------------------------------------------------
 * Blocks and who allocated them.
 * ------------------------------------------------------------------------- */

const char *tb_memory_debug_set_label(const char *label)
{
    const char *const previous = tb_memory_debug_label;

    tb_memory_debug_label = label;
    return previous;
}

tb_memory_debug_site tb_memory_debug_set_site(tb_memory_debug_site site)
{
    tb_memory_debug_site previous = {0};

    TB_ASSERT(site.file != NULL || site.line == 0);
    previous = tb_memory_debug_location;
    tb_memory_debug_location = site;
    return previous;
}

void tb_memory_debug_visit(tb_memory_debug_visitor *visitor, void *context)
{
    TB_ASSERT(visitor != NULL);
    tb_mutex_lock(&tb_memory_debug.lock);
    for (const tb_memory_debug_record *record = tb_memory_debug.oldest; record != NULL;
         record = record->newer)
    {
        const tb_memory_debug_block block = tb_memory_debug_describe(record);

        visitor(&block, context);
    }
    tb_mutex_unlock(&tb_memory_debug.lock);
}

void tb_memory_debug_get_statistics(tb_memory_debug_statistics *statistics)
{
    TB_ASSERT(statistics != NULL);
    tb_mutex_lock(&tb_memory_debug.lock);
    *statistics = tb_memory_debug.statistics;
    tb_mutex_unlock(&tb_memory_debug.lock);
}

void tb_memory_debug_check(void)
{
    tb_memory_debug_finding finding = {0};
    bool is_damaged = false;

    tb_mutex_lock(&tb_memory_debug.lock);
    for (const tb_memory_debug_record *record = tb_memory_debug.oldest;
         record != NULL && !is_damaged; record = record->newer)
    {
        is_damaged = tb_memory_debug_is_damaged(record, &finding);
    }
    tb_mutex_unlock(&tb_memory_debug.lock);
    if (is_damaged)
    {
        tb_memory_debug_fail(&finding);
    }
}

void tb_memory_debug_watch(uint64_t number)
{
    tb_mutex_lock(&tb_memory_debug.lock);
    tb_memory_debug.watched = number;
    tb_mutex_unlock(&tb_memory_debug.lock);
}

/* ---------------------------------------------------------------------------
 * The report.
 * ------------------------------------------------------------------------- */

/* What is allocated under one label. */
typedef struct tb_memory_debug_tally
{
    const char *label;
    size_t blocks;
    size_t bytes;
} tb_memory_debug_tally;

static bool tb_memory_debug_is_same_label(const char *left, const char *right)
{
    if (left == NULL || right == NULL)
    {
        return left == right;
    }
    return tb_str_equal(left, right);
}

/* Adds a block to the tally of its label, which is given a place among the
 * `*count` in use when it has none. A label for which there is no place
 * left is added to `other`. */
static void tb_memory_debug_tally_block(tb_memory_debug_tally *tallies, size_t *count,
                                        tb_memory_debug_tally *other,
                                        const tb_memory_debug_record *record)
{
    tb_memory_debug_tally *tally = other;

    for (size_t index = 0; index < *count && tally == other; index++)
    {
        if (tb_memory_debug_is_same_label(tallies[index].label, record->label))
        {
            tally = &tallies[index];
        }
    }
    if (tally == other && *count < TB_MEMORY_DEBUG_REPORT_LABELS_MAX)
    {
        tally = &tallies[*count];
        tally->label = record->label;
        (*count)++;
    }
    tally->blocks++;
    tally->bytes += record->size;
}

void tb_memory_debug_report(void)
{
    tb_memory_debug_tally tallies[TB_MEMORY_DEBUG_REPORT_LABELS_MAX] = {{0}};
    tb_memory_debug_tally other = {0};
    size_t tally_count = 0;
    size_t listed = 0;

    tb_mutex_lock(&tb_memory_debug.lock);
    (void)tb_fmt_print_error(
        "badger: memory allocated: blocks %zu, bytes %zu; at the most: blocks %zu, bytes %zu; "
        "blocks ever: %llu\n",
        tb_memory_debug.statistics.live_blocks, tb_memory_debug.statistics.live_bytes,
        tb_memory_debug.statistics.peak_blocks, tb_memory_debug.statistics.peak_bytes,
        (unsigned long long)tb_memory_debug.statistics.allocated_blocks);
    for (const tb_memory_debug_record *record = tb_memory_debug.oldest; record != NULL;
         record = record->newer)
    {
        tb_memory_debug_tally_block(tallies, &tally_count, &other, record);
    }
    for (size_t index = 0; index < tally_count; index++)
    {
        (void)tb_fmt_print_error("  label %s: blocks %zu, bytes %zu\n",
                                 tb_memory_debug_label_text(tallies[index].label),
                                 tallies[index].blocks, tallies[index].bytes);
    }
    if (other.blocks > 0)
    {
        (void)tb_fmt_print_error("  other labels: blocks %zu, bytes %zu\n", other.blocks,
                                 other.bytes);
    }
    for (const tb_memory_debug_record *record = tb_memory_debug.oldest;
         record != NULL && listed < TB_MEMORY_DEBUG_REPORT_BLOCKS_MAX; record = record->newer)
    {
        (void)tb_fmt_print_error("  block %llu: bytes %zu, at %p, label %s\n",
                                 (unsigned long long)record->number, record->size,
                                 (const void *)tb_memory_debug_block_of(record),
                                 tb_memory_debug_label_text(record->label));
        if (record->file != NULL)
        {
            (void)tb_fmt_print_error("    allocated at %s:%zu\n", record->file, record->line);
        }
        listed++;
    }
    if (tb_memory_debug.statistics.live_blocks > listed)
    {
        (void)tb_fmt_print_error("  and more blocks: %zu\n",
                                 tb_memory_debug.statistics.live_blocks - listed);
    }
    tb_mutex_unlock(&tb_memory_debug.lock);
    (void)tb_fmt_flush();
}
