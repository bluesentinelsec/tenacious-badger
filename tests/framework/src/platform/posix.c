/* SIGTRAP is a POSIX signal; request the POSIX API from the strict C17 headers. */
#define _POSIX_C_SOURCE 200809L

#include "../platform.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

static const int tb_test_fatal_signals[] = {SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL};
static struct sigaction tb_test_saved_actions[5] = {0};
static bool tb_test_installed_actions[5] = {0};
static const char *tb_test_crash_message = NULL;
static size_t tb_test_crash_length = {0};

static void tb_test_crash_signal(int number)
{
    struct sigaction action = {0};
    size_t sent = {0};
    int saved_errno = {0};
    saved_errno = errno;
    /* Only async-signal-safe operations; formatting happened before the case.
     * A broken stderr must not turn the original fault into a different one. */
    while (sent < tb_test_crash_length)
    {
        const ssize_t count =
            write(STDERR_FILENO, tb_test_crash_message + sent, tb_test_crash_length - sent);
        if (count > 0)
        {
            sent += (size_t)count;
        }
        else if (count < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            break;
        }
    }
    action.sa_handler = SIG_DFL;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(number, &action, NULL);
    (void)raise(number);
    (void)sigaddset(&action.sa_mask, number);
    (void)sigprocmask(SIG_UNBLOCK, &action.sa_mask, NULL);
    errno = saved_errno;
    /* The signal is blocked during its handler. Returning delivers the queued
     * signal with its default action, retaining signal status and core dumps. */
}
void tb_test_platform_crash_begin(const char *message, size_t length)
{
    struct sigaction action = {0};
    tb_test_crash_message = message;
    tb_test_crash_length = length;
    action.sa_handler = tb_test_crash_signal;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaddset(&action.sa_mask, SIGPIPE);
    for (size_t index = {0}; index < 5; index++)
    {
        tb_test_installed_actions[index] = false;
        if (sigaction(tb_test_fatal_signals[index], NULL, &tb_test_saved_actions[index]) == 0 &&
            tb_test_saved_actions[index].sa_handler == SIG_DFL)
        {
            tb_test_installed_actions[index] =
                sigaction(tb_test_fatal_signals[index], &action, NULL) == 0;
        }
    }
}
void tb_test_platform_crash_end(void)
{
    for (size_t index = {0}; index < 5; index++)
    {
        struct sigaction current = {0};
        if (tb_test_installed_actions[index] &&
            sigaction(tb_test_fatal_signals[index], NULL, &current) == 0 &&
            current.sa_handler == tb_test_crash_signal)
        {
            (void)sigaction(tb_test_fatal_signals[index], &tb_test_saved_actions[index], NULL);
        }
    }
    tb_test_crash_message = NULL;
    tb_test_crash_length = 0;
}

void tb_test_platform_disable_error_dialogs(void)
{
    /* POSIX systems report crashes through signals and exit statuses only. */
}

void tb_test_platform_debug_break(void)
{
    (void)raise(SIGTRAP);
}

void tb_test_platform_print_location(const char *file, int line)
{
    /* GCC and Clang diagnostic form: editors and IDEs link `file:line`. */
    printf("%s:%d", file, line);
}
