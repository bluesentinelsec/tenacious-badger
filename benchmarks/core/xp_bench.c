/* Extended-precision arithmetic at the sizes that big integers and exact
 * floating-point conversion use: addition, multiplication, long division,
 * and conversion to decimal text. */
#include "tb_bench.h"

#include "badger/xp.h"

#include <stddef.h>
#include <stdint.h>

/* 32 digits are 1024 bits: about the size of the exact value of a double
 * during its conversion to decimal text, and of a large factorial. */
#define XP_DIGITS ((size_t)32)
#define XP_CALLS ((size_t)20000)
#define XP_TEXT_SIZE ((size_t)(2 * XP_DIGITS * TB_XP_DIGIT_BITS))

typedef struct xp_state
{
    size_t calls;
    tb_xp_digit left[2 * XP_DIGITS];
    tb_xp_digit right[XP_DIGITS];
    tb_xp_digit result[3 * XP_DIGITS];
    tb_xp_digit remainder[XP_DIGITS];
    tb_xp_digit scratch[TB_XP_DIV_SCRATCH(2 * XP_DIGITS, XP_DIGITS)];
    char text[XP_TEXT_SIZE];
} xp_state;

static void run_add(void *context)
{
    xp_state *const state = context;
    unsigned carry = 0;

    for (size_t call = 0; call < state->calls; call++)
    {
        carry = tb_xp_add(XP_DIGITS, state->result, state->left, state->right, carry);
    }
    tb_bench_keep(state->result[0] + carry);
}

static void run_multiply(void *context)
{
    xp_state *const state = context;

    for (size_t call = 0; call < state->calls; call++)
    {
        tb_xp_mul(state->result, XP_DIGITS, state->left, XP_DIGITS, state->right);
    }
    tb_bench_keep(state->result[XP_DIGITS]);
}

static void run_divide(void *context)
{
    xp_state *const state = context;

    for (size_t call = 0; call < state->calls; call++)
    {
        tb_xp_div(2 * XP_DIGITS, state->result, state->left, XP_DIGITS, state->right,
                  state->remainder, state->scratch);
    }
    tb_bench_keep(state->remainder[0]);
}

static void run_to_decimal(void *context)
{
    xp_state *const state = context;
    size_t required = 0;

    for (size_t call = 0; call < state->calls; call++)
    {
        (void)tb_xp_to_string(XP_DIGITS, state->left, 10, state->scratch, state->text,
                              sizeof(state->text), &required);
    }
    tb_bench_keep(required);
}

void tb_bench_xp(tb_bench *bench)
{
    static xp_state state = {0};
    uint64_t sequence = 5;
    tb_bench_case add = {.name = "add two numbers of 32 digits", .run = run_add};
    tb_bench_case multiply = {.name = "multiply two numbers of 32 digits", .run = run_multiply};
    tb_bench_case divide = {.name = "divide 64 digits by 32", .run = run_divide};
    tb_bench_case to_decimal = {.name = "write 32 digits as decimal text", .run = run_to_decimal};

    if (!tb_bench_wants(bench, "xp"))
    {
        return;
    }
    state.calls = tb_bench_size(bench, XP_CALLS);
    for (size_t index = 0; index < 2 * XP_DIGITS; index++)
    {
        state.left[index] = (tb_xp_digit)tb_bench_random(&sequence);
    }
    for (size_t index = 0; index < XP_DIGITS; index++)
    {
        state.right[index] = (tb_xp_digit)tb_bench_random(&sequence);
    }
    /* A divisor with its top digit set, so that its length is exact. */
    state.right[XP_DIGITS - 1] |= 1;

    add.variant = "badger";
    add.operations = (double)state.calls;
    add.context = &state;
    multiply.variant = "badger";
    multiply.operations = (double)state.calls;
    multiply.context = &state;
    divide.variant = "badger";
    divide.operations = (double)state.calls;
    divide.context = &state;
    to_decimal.variant = "badger";
    to_decimal.operations = (double)state.calls;
    to_decimal.context = &state;
    tb_bench_measure(bench, "xp", &add, 1);
    tb_bench_measure(bench, "xp", &multiply, 1);
    tb_bench_measure(bench, "xp", &divide, 1);
    tb_bench_measure(bench, "xp", &to_decimal, 1);
}
