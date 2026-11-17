/* Touches memory that an arena has released or never handed out. Under
 * AddressSanitizer each case must end the process with a use-after-poison
 * report, because the arena tells the sanitizer which of its bytes are in use.
 * ExpectArenaPoison.cmake runs each case as a child process of a sanitizer
 * build and checks how it ends; they are not registered with CTest directly. */
#include "badger/arena.h"

#include "badger/fmt.h"
#include "tb_test.h"

enum
{
    PROBE_CHUNK_SIZE = 1024,
    PROBE_OBJECT_SIZE = 64
};

/* Says that the case reached the access under test, so a process that ends
 * earlier for another reason is not mistaken for a report. */
static void announce_access(void)
{
    (void)tb_fmt_print("before-invalid-access\n");
    (void)tb_fmt_flush();
}

TB_TEST(arena_probe, read_after_reset)
{
    tb_arena *const arena = tb_arena_create(PROBE_CHUNK_SIZE);
    unsigned char *const object = tb_arena_alloc(arena, PROBE_OBJECT_SIZE, 8);

    object[0] = 7;
    tb_arena_reset(arena);
    announce_access();
    (void)tb_fmt_print("after-invalid-access: read %d\n", object[0]);
    tb_arena_destroy(arena);
}

TB_TEST(arena_probe, write_after_rewind)
{
    tb_arena *const arena = tb_arena_create(PROBE_CHUNK_SIZE);
    unsigned char *const kept = tb_arena_alloc(arena, PROBE_OBJECT_SIZE, 8);
    const tb_arena_mark mark = tb_arena_get_mark(arena);
    unsigned char *const released = tb_arena_alloc(arena, PROBE_OBJECT_SIZE, 8);

    kept[0] = 1;
    released[0] = 2;
    tb_arena_rewind(arena, mark);
    /* What was allocated before the mark is still in use. */
    kept[PROBE_OBJECT_SIZE - 1] = 3;
    announce_access();
    released[0] = 4;
    (void)tb_fmt_print("after-invalid-access\n");
    tb_arena_destroy(arena);
}

TB_TEST(arena_probe, read_after_reset_of_a_fixed_arena)
{
    static unsigned char buffer[PROBE_CHUNK_SIZE];
    tb_arena *const arena = tb_arena_create_fixed(buffer, sizeof(buffer));
    unsigned char *const object = tb_arena_alloc(arena, PROBE_OBJECT_SIZE, 8);

    object[0] = 7;
    tb_arena_reset(arena);
    announce_access();
    (void)tb_fmt_print("after-invalid-access: read %d\n", object[0]);
    tb_arena_destroy(arena);
}

TB_TEST(arena_probe, write_past_the_last_object)
{
    tb_arena *const arena = tb_arena_create(PROBE_CHUNK_SIZE);
    unsigned char *const object = tb_arena_alloc(arena, PROBE_OBJECT_SIZE, 8);

    object[PROBE_OBJECT_SIZE - 1] = 1;
    announce_access();
    /* The chunk has room here, but no object owns it. */
    object[PROBE_OBJECT_SIZE] = 2;
    (void)tb_fmt_print("after-invalid-access\n");
    tb_arena_destroy(arena);
}

TB_TEST(arena_probe, read_after_reset_of_a_reserved_arena)
{
    tb_arena *const arena = tb_arena_create_reserved(0);
    unsigned char *const object = tb_arena_alloc(arena, PROBE_OBJECT_SIZE, 8);
    object[0] = 7;
    tb_arena_reset(arena);
    announce_access();
    (void)tb_fmt_print("after-invalid-access: read %d\n", object[0]);
    tb_arena_destroy(arena);
}

TB_TEST(arena_probe, write_after_rewind_of_a_reserved_arena)
{
    tb_arena *const arena = tb_arena_create_reserved(0);
    const tb_arena_mark mark = tb_arena_get_mark(arena);
    unsigned char *const object = tb_arena_alloc(arena, PROBE_OBJECT_SIZE, 8);
    tb_arena_rewind(arena, mark);
    announce_access();
    object[0] = 7;
    (void)tb_fmt_print("after-invalid-access\n");
    tb_arena_destroy(arena);
}

TB_TEST(arena_probe, valid_use_is_not_reported)
{
    static unsigned char buffer[PROBE_CHUNK_SIZE];
    tb_arena *const arenas[] = {tb_arena_create(PROBE_CHUNK_SIZE),
                                tb_arena_create_fixed(buffer, sizeof(buffer)),
                                tb_arena_create_reserved(0)};

    /* The same steps as the cases above, touching only what is in use. A
     * report here would mean the arena poisons memory it has handed out. */
    for (size_t index = 0; index < TB_TEST_COUNT_OF(arenas); index++)
    {
        tb_arena *const arena = arenas[index];
        unsigned char *const kept = tb_arena_alloc(arena, PROBE_OBJECT_SIZE, 8);
        const tb_arena_mark mark = tb_arena_get_mark(arena);
        unsigned char *released = tb_arena_alloc(arena, PROBE_OBJECT_SIZE, 1);

        released[PROBE_OBJECT_SIZE - 1] = 1;
        tb_arena_rewind(arena, mark);
        kept[0] = 2;
        kept[PROBE_OBJECT_SIZE - 1] = 3;
        released = tb_arena_alloc(arena, PROBE_OBJECT_SIZE, 1);
        released[0] = 4;
        tb_arena_reset(arena);
        released = tb_arena_alloc(arena, PROBE_OBJECT_SIZE, 8);
        released[PROBE_OBJECT_SIZE - 1] = 5;
        tb_arena_destroy(arena);
    }
    /* Once its arena is destroyed, a fixed buffer is ordinary memory again. */
    buffer[0] = 6;
    buffer[sizeof(buffer) - 1] = 7;
    (void)tb_fmt_print("valid-use-completed\n");
}
