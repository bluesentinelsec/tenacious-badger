/* Random bytes from BCryptGenRandom, loaded from the system's bcrypt.dll the
 * first time they are asked for, so neither the engine nor a program that
 * links it statically needs bcrypt.lib. */
#include "badger/random.h"

#include "badger/assert.h"
#include "badger/library.h"

#include "random_fill.h"

static INIT_ONCE tb_win32_random_once = INIT_ONCE_STATIC_INIT;
static tb_win32_gen_random *tb_win32_random_function;

/* The library stays loaded for the life of the process. */
static BOOL CALLBACK tb_win32_random_load(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    tb_library *library = NULL;
    tb_library_function function = NULL;
    (void)once;
    (void)parameter;
    (void)context;
    /* coverage-off: the library is part of every Windows the engine supports */
    if (tb_library_open("bcrypt.dll", TB_LIBRARY_SEARCH_SYSTEM, &library, NULL) != TB_LIBRARY_OK)
    {
        return TRUE;
    }
    if (tb_library_find_function(library, "BCryptGenRandom", &function, NULL) != TB_LIBRARY_OK)
    {
        tb_library_close(library);
        return TRUE;
    }
    /* coverage-on */
    tb_win32_random_function = (tb_win32_gen_random *)function;
    return TRUE;
}

bool tb_win32_random_fill(unsigned char *output, size_t length, ULONG chunk_limit,
                          tb_win32_gen_random *generate)
{
    TB_ASSERT(chunk_limit > 0);
    while (length > 0)
    {
        const ULONG piece = length > chunk_limit ? chunk_limit : (ULONG)length;
        if (generate == NULL ||
            !BCRYPT_SUCCESS(generate(NULL, output, piece, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        {
            return false;
        }
        output += piece;
        length -= piece;
    }
    return true;
}

bool tb_random_bytes(void *bytes, size_t length)
{
    TB_ASSERT(bytes != NULL || length == 0);
    if (length == 0)
    {
        return true;
    }
    (void)InitOnceExecuteOnce(&tb_win32_random_once, tb_win32_random_load, NULL, NULL);
    return tb_win32_random_fill(bytes, length, 0x40000000u, tb_win32_random_function);
}
