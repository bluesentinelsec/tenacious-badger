/* Random bytes from BCryptGenRandom, loaded from the system's bcrypt.dll the
 * first time they are asked for, so neither the engine nor a program that
 * links it statically needs bcrypt.lib. */
#include "badger/random.h"

#include "badger/assert.h"
#include "badger/library.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <bcrypt.h>

typedef NTSTATUS WINAPI tb_win32_gen_random(BCRYPT_ALG_HANDLE algorithm, PUCHAR buffer,
                                            ULONG length, ULONG flags);

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

bool tb_random_bytes(void *bytes, size_t length)
{
    unsigned char *output = bytes;
    TB_ASSERT(bytes != NULL || length == 0);
    (void)InitOnceExecuteOnce(&tb_win32_random_once, tb_win32_random_load, NULL, NULL);
    /* coverage-off: as above */
    if (tb_win32_random_function == NULL)
    {
        return false;
    }
    /* coverage-on */
    while (length > 0)
    {
        /* The function takes a 32-bit length. */
        const ULONG piece = length > (size_t)0x40000000 ? 0x40000000u : (ULONG)length;
        const NTSTATUS status =
            tb_win32_random_function(NULL, output, piece, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        /* coverage-off: the system's generator does not fail */
        if (!BCRYPT_SUCCESS(status))
        {
            return false;
        }
        /* coverage-on */
        output += piece;
        length -= piece;
    }
    return true;
}
