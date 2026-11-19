/* Prints the keys that tb_add_package_key compiled in, one per line in
 * hexadecimal, for tests/ExpectPackageKey.cmake to compare with the key file
 * and the password, and to look for in this executable's bytes. */
#include "badger/fmt.h"
#include "tb_probe_file_key.h"
#include "tb_probe_password_key.h"

#include <stddef.h>
#include <stdint.h>

static void print_key(void (*function)(uint8_t key[32]))
{
    uint8_t key[32] = {0};
    function(key);
    for (size_t index = 0; index < sizeof(key); index++)
    {
        (void)tb_fmt_print("%02x", (unsigned)key[index]);
    }
    (void)tb_fmt_print("\n");
}

int main(void)
{
    print_key(tb_probe_file_key);
    print_key(tb_probe_password_key);
    return 0;
}
