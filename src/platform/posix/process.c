#define _POSIX_C_SOURCE 200809L
/* The process's arguments on Linux and macOS: the bytes the program was
 * started with, as main received them; and its standard streams sent to a
 * file. */
#include "badger/process.h"

#include "badger/assert.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

/* rw-r--r--, less the user's umask: a log anyone may read. */
#define TB_POSIX_OUTPUT_MODE 0644

bool tb_process_get_arguments(int count, char **values, tb_arguments *arguments)
{
    TB_ASSERT(count >= 1 && values != NULL);
    TB_ASSERT(arguments != NULL);
    arguments->count = (size_t)count;
    arguments->values = (const char *const *)values;
    arguments->storage = NULL;
    return true;
}

void tb_process_release_arguments(tb_arguments *arguments)
{
    TB_ASSERT(arguments != NULL);
    arguments->count = 0;
    arguments->values = NULL;
    arguments->storage = NULL;
}

static bool tb_posix_move_output(int file, int target, int flags)
{
    int result = -1;
    do
    {
        result = dup2(file, target);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
    {
        return false;
    }
    /* The file may already be the target when the launcher closed that
     * descriptor. dup2 then preserves close-on-exec. Set the flags on the
     * installed descriptor so a child inherits exactly the intended streams. */
    const int installed = result;
    do
    {
        result = fcntl(installed, F_SETFD, flags);
    } while (result < 0 && errno == EINTR);
    return result >= 0;
}

bool tb_process_write_output_to(const char *path)
{
    int descriptor = -1;
    int saved_output = -1;
    int output_flags = 0;
    bool moved = false;

    TB_ASSERT(path != NULL);
    /* Keep descriptors 0 through 2 free if the launcher left them closed. */
    saved_output = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (saved_output >= 0)
    {
        output_flags = fcntl(STDOUT_FILENO, F_GETFD);
        if (output_flags < 0)
        {
            goto cleanup;
        }
    }
    else if (errno != EBADF)
    {
        return false;
    }
    descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, TB_POSIX_OUTPUT_MODE);
    if (descriptor < 0 || !tb_posix_move_output(descriptor, STDOUT_FILENO, 0))
    {
        goto cleanup;
    }
    if (!tb_posix_move_output(descriptor, STDERR_FILENO, 0))
    {
        if (saved_output >= 0)
        {
            (void)tb_posix_move_output(saved_output, STDOUT_FILENO, output_flags);
        }
        else if (descriptor != STDOUT_FILENO)
        {
            (void)close(STDOUT_FILENO);
        }
        goto cleanup;
    }
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)setvbuf(stderr, NULL, _IONBF, 0);
    moved = true;
cleanup:
    if (descriptor >= 0 && (!moved || (descriptor != STDOUT_FILENO && descriptor != STDERR_FILENO)))
    {
        (void)close(descriptor);
    }
    if (saved_output >= 0)
    {
        (void)close(saved_output);
    }
    return moved;
}
