/* Checksums over 1 MiB: CRC-32 by slicing-by-8, beside the classic loop of
 * one table lookup per byte that it replaces, and Adler-32. */
#include "tb_bench.h"

#include "badger/checksum.h"
#include "badger/memory.h"

#include <stddef.h>
#include <stdint.h>

#define CHECKSUM_SIZE ((size_t)1 << 20)

typedef struct checksum_state
{
    unsigned char *bytes;
    size_t size;
    uint32_t table[256]; /* For the byte-at-a-time loop. */
} checksum_state;

static void run_crc32(void *context)
{
    const checksum_state *const state = context;
    tb_bench_keep(tb_crc32(state->bytes, state->size));
}

static void run_crc32_bytewise(void *context)
{
    const checksum_state *const state = context;
    uint32_t value = 0xFFFFFFFFu;
    for (size_t index = 0; index < state->size; index++)
    {
        value = (value >> 8) ^ state->table[(value ^ state->bytes[index]) & 0xFFu];
    }
    tb_bench_keep(~value);
}

static void run_adler32(void *context)
{
    const checksum_state *const state = context;
    tb_bench_keep(tb_adler32(state->bytes, state->size));
}

void tb_bench_checksum(tb_bench *bench)
{
    checksum_state state = {0};
    uint64_t random = 1;
    tb_bench_case crc[] = {
        {.name = "CRC-32 of 1 MiB", .variant = "tb_crc32", .run = run_crc32},
        {.name = "CRC-32 of 1 MiB", .variant = "a lookup per byte", .run = run_crc32_bytewise},
    };
    tb_bench_case adler = {
        .name = "Adler-32 of 1 MiB", .variant = "tb_adler32", .run = run_adler32};
    if (!tb_bench_wants(bench, "checksum"))
    {
        return;
    }
    state.size = tb_bench_size(bench, CHECKSUM_SIZE);
    state.bytes = tb_malloc(state.size);
    for (size_t index = 0; index < state.size; index++)
    {
        state.bytes[index] = (unsigned char)tb_bench_random(&random);
    }
    for (uint32_t index = 0; index < 256; index++)
    {
        uint32_t value = index;
        for (int bit = 0; bit < 8; bit++)
        {
            value = (value & 1u) != 0 ? (value >> 1) ^ 0xEDB88320u : value >> 1;
        }
        state.table[index] = value;
    }
    /* Operations are bytes, so the numbers are nanoseconds per byte. */
    for (size_t index = 0; index < 2; index++)
    {
        crc[index].context = &state;
        crc[index].operations = (double)state.size;
    }
    adler.context = &state;
    adler.operations = (double)state.size;
    tb_bench_measure(bench, "checksum", crc, 2);
    tb_bench_measure(bench, "checksum", &adler, 1);
    tb_free(state.bytes);
}
