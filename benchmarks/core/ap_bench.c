/* Arbitrary-precision integers: the cost of a new value for every result, on
 * 1024-bit operands, and a modular power of the size used to check primes. */
#include "tb_bench.h"

#include "badger/ap.h"

#include <stddef.h>
#include <stdint.h>

#define AP_CALLS ((size_t)20000)
#define AP_POWER_CALLS ((size_t)200)

typedef struct ap_state
{
    size_t calls;
    size_t power_calls;
    tb_ap *left;     /* 1024 bits */
    tb_ap *right;    /* 512 bits */
    tb_ap *exponent; /* 256 bits */
    tb_ap *modulus;  /* 256 bits */
} ap_state;

static void run_add(void *context)
{
    const ap_state *const state = context;
    int sign = 0;

    for (size_t call = 0; call < state->calls; call++)
    {
        tb_ap *const sum = tb_ap_add(state->left, state->right);

        sign += tb_ap_sign(sum);
        tb_ap_free(sum);
    }
    tb_bench_keep((uint64_t)sign);
}

static void run_multiply(void *context)
{
    const ap_state *const state = context;
    int sign = 0;

    for (size_t call = 0; call < state->calls; call++)
    {
        tb_ap *const product = tb_ap_mul(state->left, state->right);

        sign += tb_ap_sign(product);
        tb_ap_free(product);
    }
    tb_bench_keep((uint64_t)sign);
}

static void run_divide(void *context)
{
    const ap_state *const state = context;
    int sign = 0;

    for (size_t call = 0; call < state->calls; call++)
    {
        tb_ap *const quotient = tb_ap_div_floor(state->left, state->right);

        sign += tb_ap_sign(quotient);
        tb_ap_free(quotient);
    }
    tb_bench_keep((uint64_t)sign);
}

static void run_power(void *context)
{
    const ap_state *const state = context;
    int sign = 0;

    for (size_t call = 0; call < state->power_calls; call++)
    {
        tb_ap *const power = tb_ap_pow(state->left, state->exponent, state->modulus);

        sign += tb_ap_sign(power);
        tb_ap_free(power);
    }
    tb_bench_keep((uint64_t)sign);
}

/* A value of `bits` bits from the benchmark's sequence, with the top bit set. */
static tb_ap *random_value(size_t bits, uint64_t *sequence)
{
    tb_ap *value = tb_ap_from_i64(NULL, 1);

    for (size_t done = 0; done + 64 <= bits; done += 64)
    {
        tb_ap *const shifted = tb_ap_shift_left(value, 64);
        tb_ap *const next = tb_ap_add_i64(shifted, (int64_t)(tb_bench_random(sequence) >> 1));

        tb_ap_free(shifted);
        tb_ap_free(value);
        value = next;
    }
    return value;
}

void tb_bench_ap(tb_bench *bench)
{
    ap_state state = {0};
    uint64_t sequence = 7;
    tb_bench_case add = {.name = "add 1024 and 512 bits into a new value", .run = run_add};
    tb_bench_case multiply = {.name = "multiply 1024 by 512 bits into a new value",
                              .run = run_multiply};
    tb_bench_case divide = {.name = "divide 1024 by 512 bits into a new value", .run = run_divide};
    tb_bench_case power = {.name = "raise to a 256-bit power modulo 256 bits", .run = run_power};

    if (!tb_bench_wants(bench, "ap"))
    {
        return;
    }
    state.calls = tb_bench_size(bench, AP_CALLS);
    state.power_calls = tb_bench_size(bench, AP_POWER_CALLS);
    state.left = random_value(1024, &sequence);
    state.right = random_value(512, &sequence);
    state.exponent = random_value(256, &sequence);
    state.modulus = random_value(256, &sequence);

    add.variant = "badger";
    add.operations = (double)state.calls;
    add.context = &state;
    multiply.variant = "badger";
    multiply.operations = (double)state.calls;
    multiply.context = &state;
    divide.variant = "badger";
    divide.operations = (double)state.calls;
    divide.context = &state;
    power.variant = "badger";
    power.operations = (double)state.power_calls;
    power.context = &state;
    tb_bench_measure(bench, "ap", &add, 1);
    tb_bench_measure(bench, "ap", &multiply, 1);
    tb_bench_measure(bench, "ap", &divide, 1);
    tb_bench_measure(bench, "ap", &power, 1);
    tb_ap_free(state.modulus);
    tb_ap_free(state.exponent);
    tb_ap_free(state.right);
    tb_ap_free(state.left);
}
