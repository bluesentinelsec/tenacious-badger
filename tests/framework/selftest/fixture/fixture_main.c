/* Entry point of the fixture executable. Given "--with-hooks" as its first
 * argument it installs case hooks that announce themselves, so that
 * RunScenario.cmake can check when hooks run and what they can do. */
#include "tb_test.h"

#include <stdio.h>
#include <string.h>

static void announce_before_case(void *context)
{
    const tb_test_case *const test = tb_test_current_case();
    const char *const label = context;

    printf("hook-before %s.%s (%s)\n", test->suite, test->name, label);
    if (strcmp(test->name, "is_skipped_by_the_before_hook") == 0)
    {
        TB_TEST_SKIP("skipped by the before hook");
    }
}

static void announce_after_case(void *context)
{
    const tb_test_case *const test = tb_test_current_case();
    int has_failed = {0};

    (void)context;
    if (tb_test_case_has_failed())
    {
        has_failed = 1;
    }
    printf("hook-after %s.%s failed=%d\n", test->suite, test->name, has_failed);
    if (strcmp(test->name, "is_failed_by_the_after_hook") == 0)
    {
        tb_test_fail_at(test->file, test->line, "rejected by the after hook");
    }
}

int main(int argc, char **argv)
{
    static char label[] = "fixture context";

    if (argc > 1 && strcmp(argv[1], "--with-hooks") == 0)
    {
        tb_test_set_case_hooks(announce_before_case, announce_after_case, label);
        /* Hide the argument from the framework: the program name takes its place. */
        argv[1] = argv[0];
        return tb_test_main(argc - 1, argv + 1);
    }
    return tb_test_main(argc, argv);
}
