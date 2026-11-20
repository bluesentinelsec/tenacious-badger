/* sigaltstack and the SA_ flags past POSIX's base are not in the strict C17
 * headers of each system. */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "badger/crash.h"

#include "../shared/crash_text.h"
#include "badger/fatal.h"
#include "badger/stack.h"
#include "crash_context.h"

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The stack a report is written on, for the thread that called
 * tb_crash_catch(), so that running out of its own is reported too. */
#define TB_POSIX_CRASH_STACK_SIZE ((size_t)64 * 1024)

/* Frames of a capture in the handler that are not the crashed code's: the
 * handler itself and the system's trampoline that called it. */
#define TB_POSIX_CRASH_HANDLER_FRAMES ((size_t)2)

typedef struct tb_posix_crash_signal
{
    int number;
    const char *name;   /* What happened, in words. */
    const char *symbol; /* The signal's name. */
    bool has_address;   /* Whether si_addr says where. */
} tb_posix_crash_signal;

static const tb_posix_crash_signal tb_posix_crash_signals[] = {
    {SIGSEGV, "segmentation fault", "SIGSEGV", true},
    {SIGBUS, "bus error", "SIGBUS", true},
    {SIGILL, "illegal instruction", "SIGILL", true},
    {SIGFPE, "arithmetic error", "SIGFPE", true},
    {SIGABRT, "abort", "SIGABRT", false},
};

static _Alignas(16) unsigned char tb_posix_crash_stack[TB_POSIX_CRASH_STACK_SIZE];

static const tb_posix_crash_signal *tb_posix_crash_find(int number)
{
    for (size_t index = 0;
         index < sizeof(tb_posix_crash_signals) / sizeof(tb_posix_crash_signals[0]); index++)
    {
        if (tb_posix_crash_signals[index].number == number)
        {
            return &tb_posix_crash_signals[index];
        }
    }
    return NULL;
}

/* The crashed code's stack, from a capture made in the handler. The
 * capture holds the handler and the trampoline that called it, then the
 * crashed code, starting at the instruction that raised the signal. Where
 * the system's unwinding leaves that instruction out, it is put back from
 * the signal's context. */
static size_t tb_posix_crash_frames(void *const *captured, size_t captured_count, void *instruction,
                                    void **frames)
{
    size_t first = TB_POSIX_CRASH_HANDLER_FRAMES;
    bool is_found = false;
    size_t count = 0;

    for (size_t index = 0; instruction != NULL && index < captured_count; index++)
    {
        if (captured[index] == instruction)
        {
            first = index;
            is_found = true;
            break;
        }
    }
    if (instruction != NULL && !is_found)
    {
        frames[count] = instruction;
        count++;
    }
    for (size_t index = first; index < captured_count && count < TB_FATAL_FRAMES_MAX; index++)
    {
        frames[count] = captured[index];
        count++;
    }
    return count;
}

static void tb_posix_crash_handler(int number, siginfo_t *information, void *context)
{
    const tb_posix_crash_signal *const raised = tb_posix_crash_find(number);
    char message[TB_FATAL_MESSAGE_CAPACITY] = {0};
    tb_crash_text line = {message, sizeof(message), 0};
    void *captured[TB_FATAL_FRAMES_MAX + TB_POSIX_CRASH_HANDLER_FRAMES] = {0};
    void *frames[TB_FATAL_FRAMES_MAX] = {0};
    size_t captured_count = 0;
    size_t frame_count = 0;

    captured_count = tb_stack_capture(captured, sizeof(captured) / sizeof(captured[0]), 0);
    tb_crash_text_add(&line, "crash: ");
    if (raised == NULL)
    {
        tb_crash_text_add(&line, "signal ");
        tb_crash_text_add_integer(&line, number);
    }
    else
    {
        tb_crash_text_add(&line, raised->name);
        tb_crash_text_add(&line, " (");
        tb_crash_text_add(&line, raised->symbol);
        tb_crash_text_add(&line, ")");
        if (raised->has_address)
        {
            tb_crash_text_add(&line, " at address ");
            tb_crash_text_add_address(&line, (uintptr_t)information->si_addr);
        }
    }
    frame_count = tb_posix_crash_frames(captured, captured_count,
                                        tb_posix_crash_instruction(context), frames);
    tb_fatal_with_stack(message, frames, frame_count);
}

void tb_crash_report_abort(int signal_number)
{
    void *frames[TB_FATAL_FRAMES_MAX] = {0};
    size_t frame_count = 0;

    (void)signal_number;
    /* The stack runs from the system's trampoline through abort() to its
     * caller; this function is left out. */
    frame_count = tb_stack_capture(frames, TB_FATAL_FRAMES_MAX, 1);
    tb_fatal_with_stack("crash: abort (SIGABRT)", frames, frame_count);
}

void tb_crash_catch(void)
{
    stack_t stack = {0};
    struct sigaction action = {0};

    stack.ss_sp = tb_posix_crash_stack;
    stack.ss_size = sizeof(tb_posix_crash_stack);
    if (sigaltstack(&stack, NULL) != 0)
    {
        tb_fatal_message("crash: the system refused a stack for reports of crashes");
    }
    action.sa_sigaction = tb_posix_crash_handler;
    /* The handler runs once: a crash while reporting one ends the process
     * as the system would have. */
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND | SA_NODEFER;
    (void)sigemptyset(&action.sa_mask);
    for (size_t index = 0;
         index < sizeof(tb_posix_crash_signals) / sizeof(tb_posix_crash_signals[0]); index++)
    {
        if (sigaction(tb_posix_crash_signals[index].number, &action, NULL) != 0)
        {
            tb_fatal_message("crash: the system refused a handler for crashes");
        }
    }
}
