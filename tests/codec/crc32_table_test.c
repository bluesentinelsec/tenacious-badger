/* The committed CRC-32 tables (src/codec/crc32_table.h) match their
 * definition, so a hand edit or a stale generator is caught: table 0 is the
 * byte-at-a-time table of the polynomial, and table k advances an entry of
 * table k - 1 past one more zero byte. */
#include "codec/crc32_table.h"
#include "tb_test.h"

static uint32_t crc_of_byte(uint32_t value)
{
    for (int bit = {0}; bit < 8; bit++)
    {
        const uint32_t mask = 0u - (value & 1u);
        value = (value >> 1) ^ (0xEDB88320u & mask);
    }
    return value;
}

TB_TEST(codec_crc32_table, every_entry_matches_its_definition)
{
    for (uint32_t index = {0}; index < 256; index++)
    {
        TB_TEST_CONTEXT("entry %u", (unsigned)index);
        TB_TEST_REQUIRE_HEX(tb_crc32_table[0][index], ==, crc_of_byte(index));
        for (size_t table = 1; table < 8; table++)
        {
            const uint32_t previous = tb_crc32_table[table - 1][index];
            TB_TEST_REQUIRE_HEX(tb_crc32_table[table][index], ==,
                                (previous >> 8) ^ tb_crc32_table[0][previous & 0xFFu]);
        }
    }
}
