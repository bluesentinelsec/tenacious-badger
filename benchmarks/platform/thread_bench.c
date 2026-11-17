/* What a lock costs: a mutex that nobody else wants, the same mutex with
 * eight threads after it, and a turn passed between two threads through a
 * condition variable. */
#include "tb_bench.h"

#include "badger/thread.h"

#include <stddef.h>
#include <stdint.h>

/* Threads that want one mutex at the same time. */
#define THREAD_CONTENDERS ((size_t)8)

#define THREAD_LOCKS_ALONE ((size_t)2000000)
#define THREAD_LOCKS_CONTENDED ((size_t)200000)
#define THREAD_TURNS ((size_t)100000)

typedef struct lock_state
{
    tb_mutex lock;  /* Guards `count`. */
    uint64_t count; /* Raised once under every lock. */
    size_t locks;   /* Times each thread takes the mutex. */
} lock_state;

static void reset_lock(void *context)
{
    lock_state *const state = context;

    state->count = 0;
}

static void run_lock(void *context)
{
    lock_state *const state = context;

    for (size_t round = 0; round < state->locks; round++)
    {
        tb_mutex_lock(&state->lock);
        state->count++;
        tb_mutex_unlock(&state->lock);
    }
}

static void keep_lock(void *context)
{
    const lock_state *const state = context;

    tb_bench_keep(state->count);
}

/* A turn that two threads pass back and forth: each waits until the turn is
 * its own, and gives it to the other. Every pass wakes a sleeping thread, so
 * this is the cost of handing work from one thread to another. */
typedef struct turn_state
{
    tb_mutex lock;         /* Guards `turn`. */
    tb_condition has_turn; /* Woken when `turn` changes. */
    size_t turn;           /* The number of the thread that may go. */
    size_t turns;          /* Times each thread takes its turn. */
} turn_state;

static void reset_turn(void *context)
{
    turn_state *const state = context;

    state->turn = 0;
}

static void run_turns(void *context)
{
    turn_state *const state = context;
    const size_t own = tb_bench_thread_number();
    const size_t other = 1 - own;

    tb_mutex_lock(&state->lock);
    for (size_t round = 0; round < state->turns; round++)
    {
        while (state->turn != own)
        {
            tb_condition_wait(&state->has_turn, &state->lock);
        }
        state->turn = other;
        tb_condition_wake_one(&state->has_turn);
    }
    tb_mutex_unlock(&state->lock);
    tb_bench_keep(state->turns);
}

void tb_bench_thread(tb_bench *bench)
{
    lock_state alone = {0};
    lock_state contended = {0};
    turn_state turns = {0};
    tb_bench_case locks[2] = {
        {.name = "lock and unlock a mutex nobody else wants",
         .variant = "badger",
         .setup = reset_lock,
         .run = run_lock,
         .teardown = keep_lock},
        {.name = "lock and unlock a mutex on 8 threads at once",
         .variant = "badger",
         .setup = reset_lock,
         .run = run_lock,
         .teardown = keep_lock,
         .threads = THREAD_CONTENDERS},
    };
    tb_bench_case pass = {.name = "pass a turn to another thread and get it back",
                          .variant = "badger",
                          .setup = reset_turn,
                          .run = run_turns,
                          .threads = 2};

    alone.locks = tb_bench_size(bench, THREAD_LOCKS_ALONE);
    contended.locks = tb_bench_size(bench, THREAD_LOCKS_CONTENDED);
    turns.turns = tb_bench_size(bench, THREAD_TURNS);
    locks[0].operations = (double)alone.locks;
    locks[0].context = &alone;
    locks[1].operations = (double)contended.locks;
    locks[1].context = &contended;
    pass.operations = (double)turns.turns;
    pass.context = &turns;
    tb_bench_measure(bench, "thread", locks, 2);
    tb_bench_measure(bench, "thread", &pass, 1);
}
