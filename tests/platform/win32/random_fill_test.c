/* Encode each chunk's length in its bytes so gaps, overlaps and the last
 * short chunk are visible independently of what the system RNG returns. */
#include "platform/win32/random_fill.h"

#include "tb_test.h"

#include <string.h>

static NTSTATUS WINAPI fill_chunk(BCRYPT_ALG_HANDLE algorithm, PUCHAR buffer, ULONG length,
                                  ULONG flags)
{
    if (algorithm != NULL || flags != BCRYPT_USE_SYSTEM_PREFERRED_RNG || *buffer == 0xFF)
    {
        return (NTSTATUS)-1;
    }
    memset(buffer, (int)length, length);
    return 0;
}

TB_TEST(platform_win32_random, fills_only_the_requested_chunks)
{
    enum
    {
        LIMIT = 4
    };
    unsigned char bytes[16] = {0};
    for (size_t length = 0; length <= sizeof(bytes) - 2; length++)
    {
        memset(bytes, 0, sizeof(bytes));
        TB_TEST_REQUIRE(tb_win32_random_fill(bytes + 1, length, LIMIT, fill_chunk));
        for (size_t index = 0; index < length; index++)
        {
            const size_t remaining = length - (index / LIMIT) * LIMIT;
            TB_TEST_EXPECT_UINT(bytes[index + 1], ==, remaining < LIMIT ? remaining : LIMIT);
        }
        TB_TEST_EXPECT_UINT(bytes[0], ==, 0);
        TB_TEST_EXPECT_UINT(bytes[length + 1], ==, 0);
    }
}

TB_TEST(platform_win32_random, stops_at_a_failed_chunk)
{
    unsigned char bytes[9] = {0};
    bytes[4] = 0xFF;
    TB_TEST_EXPECT(!tb_win32_random_fill(bytes, sizeof(bytes), 4, fill_chunk));
    static const unsigned char expected[] = {4, 4, 4, 4, 0xFF, 0, 0, 0, 0};
    TB_TEST_EXPECT_BYTES(bytes, expected, sizeof(bytes));
    TB_TEST_EXPECT(!tb_win32_random_fill(bytes, 1, 4, NULL));
    TB_TEST_EXPECT(tb_win32_random_fill(NULL, 0, 4, NULL));
}
