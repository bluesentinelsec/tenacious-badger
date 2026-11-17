/** @file jobs.h
 * @brief The job system: a pool of worker threads, and the jobs they run.
 *
 * A job is a function and an argument. The scheduler runs it on one of its
 * worker threads, or on a thread that is waiting for a job and has nothing
 * better to do. A job can be made to wait for others, and can be given one of
 * two priorities.
 *
 * @code
 * tb_jobs *const jobs = tb_jobs_create(NULL);
 * tb_job cull = tb_jobs_run(jobs, cull_objects, &scene);
 * tb_job animate = tb_jobs_run(jobs, animate_skeletons, &scene);
 *
 * // Drawing waits for both.
 * const tb_job needs[] = {cull, animate};
 * const tb_job_description draw = {.function = record_draws,
 *                                  .argument = &scene,
 *                                  .dependencies = needs,
 *                                  .dependency_count = 2};
 *
 * tb_jobs_wait(jobs, tb_jobs_submit(jobs, &draw));
 * tb_jobs_destroy(jobs);
 * @endcode
 *
 * **A handle never dangles.** A tb_job names a job by a slot and a
 * generation. Once the job has finished the handle is simply out of date:
 * waiting for it returns at once, and a job that depends on it does not wait.
 * Nothing has to release a handle, and one may be kept or copied freely.
 *
 * **Waiting helps.** tb_jobs_wait() does not put its thread to sleep while
 * there are jobs to run: it runs them. So a job may wait for the jobs it
 * submitted, and every worker may do so at once, without the pool running
 * out of threads.
 *
 * @par Threading
 * Every function is thread-safe and may be called from inside a job, except
 * tb_jobs_create() and tb_jobs_destroy(), which the owner of the scheduler
 * calls with no other thread using it.
 */
#ifndef TB_JOBS_H
#define TB_JOBS_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief A pool of worker threads and the jobs they run. Opaque. */
typedef struct tb_jobs tb_jobs;

/**
 * @brief The function a job runs.
 * @param argument The pointer given when the job was submitted; may be NULL.
 */
typedef void tb_job_function(void *argument);

/**
 * @brief Names a job that was submitted.
 *
 * Zero-initialized, it names no job: waiting for it returns at once and
 * depending on it does nothing. The fields are private to the scheduler.
 */
typedef struct tb_job
{
    uint32_t slot;       /**< Where the scheduler keeps the job; zero for no job. */
    uint32_t generation; /**< Which of the jobs that have used the slot. */
} tb_job;

/** @brief Which jobs a worker takes first. */
typedef enum tb_job_priority
{
    /** The work of the frame. A worker takes it before any background job. */
    TB_JOB_PRIORITY_FOREGROUND = 0,
    /** Work that may wait: loading, streaming, baking. It runs when no
     * foreground job is ready. A foreground job that depends on a background
     * job waits for it as long as the background job waits. */
    TB_JOB_PRIORITY_BACKGROUND
} tb_job_priority;

/** @brief How a scheduler is made. Zero-initialized, it asks for the defaults. */
typedef struct tb_jobs_options
{
    /** Worker threads to start, or 0 for one for each logical processor that
     * the process may use, less one for the thread that submits and waits,
     * and never fewer than one. See tb_system_get_processors(). */
    size_t worker_count;
    /** Jobs a worker can keep to itself before it shares the rest, for each
     * priority: a power of two, or 0 for 256. Jobs beyond it are not lost;
     * they go to a queue that every worker shares, behind a lock. */
    size_t queue_capacity;
} tb_jobs_options;

/**
 * @brief Start a pool of worker threads.
 * @param options How to make it, or NULL for the defaults. Borrowed for the
 * call.
 * @return The scheduler; never NULL. The caller owns it and releases it with
 * tb_jobs_destroy().
 * @par Errors A `queue_capacity` that is not zero or a power of two is a
 * contract violation. Running out of memory, or a system that cannot start a
 * thread, ends the process as it does everywhere.
 * @par Threading Thread-safe; schedulers are independent of one another.
 */
TB_API tb_jobs *tb_jobs_create(const tb_jobs_options *options);

/**
 * @brief Finish every job, stop the workers, and release the scheduler.
 *
 * Jobs that are still waiting to run are run first, by the workers and by
 * the calling thread, including jobs that those jobs submit. Then the
 * workers are joined.
 *
 * @param jobs The scheduler, or NULL, which does nothing.
 * @par Threading Externally synchronized: no other thread may use the
 * scheduler during or after the call, and it must not be called from a job
 * of this scheduler.
 */
TB_API void tb_jobs_destroy(tb_jobs *jobs);

/**
 * @brief How many worker threads a scheduler has.
 * @param jobs Required non-null.
 * @return The count, at least 1. A thread that waits also runs jobs, so more
 * than this many can run at once.
 * @par Threading Thread-safe.
 */
TB_API size_t tb_jobs_worker_count(const tb_jobs *jobs);

/** @brief A job to submit. Zero-initialize it and set what is needed. */
typedef struct tb_job_description
{
    /** What to run, or NULL for a job that does nothing itself: it finishes
     * when its dependencies have, which makes it a way to wait for many
     * jobs at once. */
    tb_job_function *function;
    /** Passed to `function`; may be NULL. Whatever it points to must stay
     * valid until the job has finished. */
    void *argument;
    /** One of tb_job_priority. */
    tb_job_priority priority;
    /** Jobs that must finish before this one starts; NULL when there are
     * none. Borrowed for the call. A handle that names no job, or a job that
     * has finished already, is not waited for. */
    const tb_job *dependencies;
    /** How many handles `dependencies` holds. */
    size_t dependency_count;
} tb_job_description;

/**
 * @brief Submit a job.
 *
 * The job may start before this function returns, and may have finished by
 * the time the caller looks at the handle.
 *
 * @param jobs Required non-null.
 * @param description Required non-null; borrowed for the call.
 * @return A handle to the job.
 * @par Errors A priority that is not one of tb_job_priority, a non-zero
 * `dependency_count` with NULL `dependencies`, and a dependency that came
 * from another scheduler are contract violations. More than about a million
 * unfinished jobs at once ends the process with a diagnostic.
 * @par Threading Thread-safe; may be called from inside a job.
 */
TB_API tb_job tb_jobs_submit(tb_jobs *jobs, const tb_job_description *description);

/**
 * @brief Submit a foreground job with no dependencies.
 * @param jobs Required non-null.
 * @param function What to run; required non-null.
 * @param argument Passed to `function`; may be NULL.
 * @return A handle to the job.
 * @par Threading Thread-safe; may be called from inside a job.
 */
TB_API tb_job tb_jobs_run(tb_jobs *jobs, tb_job_function *function, void *argument);

/**
 * @brief Whether a job has finished.
 * @param jobs Required non-null; the scheduler the handle came from.
 * @param job A handle. One that names no job counts as finished.
 * @return true once the job's function has returned; then it stays true.
 * @par Threading Thread-safe.
 */
TB_API bool tb_jobs_is_finished(tb_jobs *jobs, tb_job job);

/**
 * @brief Return when a job has finished, running other jobs meanwhile.
 *
 * The calling thread runs jobs that are ready, any of them, until the one it
 * waits for has finished, and sleeps only when there is nothing to run. It
 * may therefore return some time after the job finished, when it was in the
 * middle of another.
 *
 * A job must not wait for itself, or for a job that depends on it: neither
 * can ever finish. That is not checked.
 *
 * @param jobs Required non-null; the scheduler the handle came from.
 * @param job A handle. One that names no job returns at once.
 * @par Threading Thread-safe; may be called from inside a job. Each job that
 * the waiting thread runs is on top of the waiter's stack, so jobs that wait
 * inside jobs that wait use stack for every level.
 */
TB_API void tb_jobs_wait(tb_jobs *jobs, tb_job job);

/**
 * @brief Return when no job is unfinished, running jobs meanwhile.
 *
 * As tb_jobs_wait(), for everything that has been submitted, including what
 * running jobs go on to submit. Other threads may submit more as soon as it
 * returns, or before.
 *
 * @param jobs Required non-null.
 * @par Threading Thread-safe, but not from inside a job, which would wait
 * for itself.
 */
TB_API void tb_jobs_wait_for_all(tb_jobs *jobs);

#endif
