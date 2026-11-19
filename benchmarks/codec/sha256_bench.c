/* SHA-256 over 1 MiB, and HMAC-SHA256 over 64 bytes: what hashing content
 * costs, and what deriving one key costs. */
#include "tb_bench.h"

#include "badger/memory.h"
#include "badger/sha256.h"

#include <stddef.h>
#include <stdint.h>

#define SHA256_SIZE ((size_t)1 << 20)

typedef struct sha256_bench_state
{
    unsigned char *bytes;
    size_t size;
} sha256_bench_state;

static void run_sha256(void *context)
{
    const sha256_bench_state *const state = context;
    uint8_t digest[TB_SHA256_SIZE] = {0};
    tb_sha256(state->bytes, state->size, digest);
    tb_bench_keep(digest[0]);
}

static void run_hmac(void *context)
{
    const sha256_bench_state *const state = context;
    uint8_t mac[TB_SHA256_SIZE] = {0};
    tb_hmac_sha256(state->bytes, 32, state->bytes + 32, 64, mac);
    tb_bench_keep(mac[0]);
}

void tb_bench_sha256(tb_bench *bench)
{
    sha256_bench_state state = {0};
    uint64_t random = 1;
    tb_bench_case whole = {.name = "SHA-256 of 1 MiB", .variant = "tb_sha256", .run = run_sha256};
    tb_bench_case derive = {
        .name = "HMAC-SHA256 of 64 bytes", .variant = "tb_hmac_sha256", .run = run_hmac};
    if (!tb_bench_wants(bench, "sha256"))
    {
        return;
    }
    state.size = tb_bench_size(bench, SHA256_SIZE);
    state.bytes = tb_malloc(state.size < 96 ? 96 : state.size);
    for (size_t index = 0; index < (state.size < 96 ? 96 : state.size); index++)
    {
        state.bytes[index] = (unsigned char)tb_bench_random(&random);
    }
    /* Operations are bytes for the hash, so its number is nanoseconds per
     * byte; and calls for the HMAC. */
    whole.context = &state;
    whole.operations = (double)state.size;
    derive.context = &state;
    derive.operations = 1.0;
    tb_bench_measure(bench, "sha256", &whole, 1);
    tb_bench_measure(bench, "sha256", &derive, 1);
    tb_free(state.bytes);
}
