/* Child-process checks of fatal diagnostics and leak attribution. */
#include "badger/fmt.h"
#include "badger/memory_debug.h"
#include "tb_test.h"

static void install_checking(void)
{
#ifndef TB_MEMORY_PROBE_RAW
    tb_memory_functions functions = {0};

    tb_memory_debug_get_functions(&functions);
    TB_TEST_REQUIRE(tb_memory_set_functions(&functions));
#endif
    (void)tb_memory_debug_set_label("probe");
    (void)tb_memory_debug_set_site((tb_memory_debug_site){"allocation.c", 42});
}

TB_TEST(memory_debug_probe, overrun)
{
    unsigned char *block = NULL;
    size_t offset = 0;

    install_checking();
    block = tb_malloc(8);
    offset = tb_memory_allocation_count() + 7;
    block[offset] = 0;
    tb_free(block);
}

TB_TEST(memory_debug_probe, underrun)
{
    unsigned char *block = NULL;
    size_t offset = 0;

    install_checking();
    block = tb_malloc(8);
    offset = tb_memory_allocation_count();
    block -= offset;
    block[0] = 0;
    block += offset;
    tb_free(block);
}

static void returning_handler(const tb_memory_debug_finding *finding, void *context)
{
    (void)finding;
    (void)context;
    (void)tb_fmt_print_error("returning-handler-called\n");
}

TB_TEST(memory_debug_probe, returning_handler)
{
    unsigned char *block = NULL;

    install_checking();
    tb_memory_debug_set_handler(returning_handler, NULL);
    block = tb_malloc(8);
    block[8] = 0;
    tb_free(block);
}

TB_TEST(memory_debug_probe, report)
{
    void *blocks[40] = {0};
    char labels[40][32] = {{0}};

    install_checking();
    for (size_t index = 0; index < 40; index++)
    {
        (void)tb_fmt_buffer(labels[index], sizeof(labels[index]), "module-%zu", index);
        (void)tb_memory_debug_set_label(labels[index]);
        blocks[index] = tb_malloc(index + 1);
    }
    tb_memory_debug_report();
    for (size_t index = 0; index < 40; index++)
    {
        tb_free(blocks[index]);
    }
    (void)tb_memory_debug_set_label(NULL);
    TB_TEST_REQUIRE(tb_memory_debug_reset());
    tb_memory_debug_report();
}
