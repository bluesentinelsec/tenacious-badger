#define _POSIX_C_SOURCE 200809L
/* The process's arguments on Linux and macOS: the bytes the program was
 * started with, as main received them; and its standard streams sent to a
 * file. */
#include "badger/process.h"

#include "badger/assert.h"

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

bool tb_process_write_output_to(const char *path)
{
    int descriptor = -1;
    bool moved = false;

    TB_ASSERT(path != NULL);
    descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, TB_POSIX_OUTPUT_MODE);
    if (descriptor < 0)
    {
        return false;
    }
    (void)fflush(stdout);
    (void)fflush(stderr);
    /* Both descriptors share the file's position, so lines keep their order.
     * dup2 clears close-on-exec, so a child writes there too. */
    /* coverage-off: duplicating an open descriptor onto 1 or 2 cannot fail */
    if (dup2(descriptor, STDOUT_FILENO) < 0 || dup2(descriptor, STDERR_FILENO) < 0)
    {
        goto cleanup;
    }
    /* coverage-on */
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)setvbuf(stderr, NULL, _IONBF, 0);
    moved = true;
cleanup:
    (void)close(descriptor);
    return moved;
}
