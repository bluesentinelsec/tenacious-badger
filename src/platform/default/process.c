#include "badger/process.h"

#include "not_implemented.h"

/* Standard C gives main its arguments, but says nothing of their encoding,
 * which is each system's own. */

bool tb_process_get_arguments(int count, char **values, tb_arguments *arguments)
{
    (void)count;
    (void)values;
    (void)arguments;
    tb_default_not_implemented("platform.process", "tb_process_get_arguments");
}

void tb_process_release_arguments(tb_arguments *arguments)
{
    (void)arguments;
    tb_default_not_implemented("platform.process", "tb_process_release_arguments");
}

bool tb_process_write_output_to(const char *path)
{
    (void)path;
    tb_default_not_implemented("platform.process", "tb_process_write_output_to");
}
