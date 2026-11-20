/* The process's arguments on Linux and macOS: the bytes the program was
 * started with, as main received them. */
#include "badger/process.h"

#include "badger/assert.h"

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
