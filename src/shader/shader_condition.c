/* The expressions of `#if` and `#elif`: signed 64-bit integer arithmetic,
 * with every overflow, division by zero and bad shift an error. */
#include "shader_internal.h"

#include "badger/fmt.h"

#include <string.h>

/* Parentheses and unary operators nested in one another. */
#define MAX_NESTING ((size_t)256)

typedef struct evaluator
{
    const tb_shader_token *tokens;
    size_t count;
    size_t position;
    tb_shader_location directive;
    tb_shader_error_function *error;
    void *context;
    size_t nesting;
    bool failed;
} evaluator;

/* The token at the position, or NULL at the end. */
static const tb_shader_token *current(const evaluator *state)
{
    return state->position < state->count ? &state->tokens[state->position] : NULL;
}

static tb_shader_location here(const evaluator *state)
{
    const tb_shader_token *const token = current(state);
    return token != NULL ? token->location : state->directive;
}

/* Report the first error; later ones follow from it. */
static void fail(evaluator *state, tb_shader_location location, const char *message)
{
    if (!state->failed)
    {
        state->error(state->context, location, message);
    }
    state->failed = true;
}

/* Fail with a message about the current token. */
static void fail_at_token(evaluator *state, const char *what)
{
    const tb_shader_token *const token = current(state);
    if (token == NULL)
    {
        fail(state, state->directive, "the expression ends too soon");
        return;
    }
    char message[160] = {0};
    (void)tb_fmt_buffer(message, sizeof(message), "%s '%.*s'", what,
                        (int)(token->length > 64 ? 64 : token->length), token->text);
    fail(state, token->location, message);
}

static bool accept(evaluator *state, const char *spelling)
{
    const tb_shader_token *const token = current(state);
    if (token != NULL && tb_shader_token_is(token, spelling))
    {
        state->position++;
        return true;
    }
    return false;
}

static int64_t parse_conditional(evaluator *state, bool evaluated);

/* -------------------------------------------------------------------------
 * Numbers.
 * ------------------------------------------------------------------------- */

static int digit_value(char byte)
{
    if (byte >= '0' && byte <= '9')
    {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f')
    {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F')
    {
        return byte - 'A' + 10;
    }
    return -1;
}

static bool is_integer_suffix(const char *text, size_t length)
{
    for (size_t index = 0; index < length; index++)
    {
        const char byte = text[index];
        if (byte != 'u' && byte != 'U' && byte != 'l' && byte != 'L')
        {
            return false;
        }
    }
    return true;
}

/* An integer literal: decimal, `0x` hexadecimal, `0b` binary or `0` octal,
 * with an optional suffix of `u` and `l`. */
static int64_t parse_number(evaluator *state, const tb_shader_token *token)
{
    const char *text = token->text;
    size_t length = token->length;
    unsigned base = 10;
    if (length > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        base = 16;
        text += 2;
        length -= 2;
    }
    else if (length > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B'))
    {
        base = 2;
        text += 2;
        length -= 2;
    }
    else if (length > 1 && text[0] == '0')
    {
        base = 8;
    }
    uint64_t value = 0;
    size_t index = 0;
    while (index < length)
    {
        const int digit = digit_value(text[index]);
        if (digit < 0 || (unsigned)digit >= base)
        {
            break;
        }
        if (value > ((uint64_t)INT64_MAX - (uint64_t)digit) / base)
        {
            fail(state, token->location, "the number is too large for #if");
            return 0;
        }
        value = value * base + (uint64_t)digit;
        index++;
    }
    const bool has_digits = index > 0;
    if (!has_digits || !is_integer_suffix(text + index, length - index))
    {
        const bool floating = memchr(token->text, '.', token->length) != NULL ||
                              (base == 10 && (memchr(token->text, 'e', token->length) != NULL ||
                                              memchr(token->text, 'E', token->length) != NULL));
        fail(state, token->location,
             floating ? "#if takes integers, not floating-point numbers"
                      : "the number is not an integer #if can read");
        return 0;
    }
    return (int64_t)value;
}

/* -------------------------------------------------------------------------
 * Checked arithmetic.
 * ------------------------------------------------------------------------- */

static bool add_overflows(int64_t a, int64_t b)
{
    return (b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b);
}

static bool subtract_overflows(int64_t a, int64_t b)
{
    return (b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b);
}

static bool multiply_overflows(int64_t a, int64_t b)
{
    if (a == 0 || b == 0)
    {
        return false;
    }
    if (a > 0)
    {
        return b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a;
    }
    return b > 0 ? a < INT64_MIN / b : a < INT64_MAX / b;
}

/* Apply a binary operator other than `&&`, `||` and `?:`. */
static int64_t apply(evaluator *state, const tb_shader_token *op, int64_t a, int64_t b,
                     bool evaluated)
{
    const tb_shader_location location = op->location;
    const char *problem = NULL;
    int64_t result = 0;
    if (tb_shader_token_is(op, "*"))
    {
        problem = multiply_overflows(a, b) ? "the multiplication overflows" : NULL;
        result = problem == NULL ? a * b : 0;
    }
    else if (tb_shader_token_is(op, "/") || tb_shader_token_is(op, "%"))
    {
        const bool divide = tb_shader_token_is(op, "/");
        if (b == 0)
        {
            problem = "division by zero";
        }
        else if (a == INT64_MIN && b == -1)
        {
            problem = "the division overflows";
        }
        else
        {
            result = divide ? a / b : a % b;
        }
    }
    else if (tb_shader_token_is(op, "+"))
    {
        problem = add_overflows(a, b) ? "the addition overflows" : NULL;
        result = problem == NULL ? a + b : 0;
    }
    else if (tb_shader_token_is(op, "-"))
    {
        problem = subtract_overflows(a, b) ? "the subtraction overflows" : NULL;
        result = problem == NULL ? a - b : 0;
    }
    else if (tb_shader_token_is(op, "<<") || tb_shader_token_is(op, ">>"))
    {
        const bool left = tb_shader_token_is(op, "<<");
        if (b < 0 || b > 63)
        {
            problem = "the shift count is not from 0 to 63";
        }
        else if (left && (a < 0 || a > (INT64_MAX >> b)))
        {
            problem = "the left shift overflows";
        }
        else if (left)
        {
            result = (int64_t)((uint64_t)a << b);
        }
        else
        {
            /* An arithmetic shift, which C leaves to the compiler for a negative value. */
            result = a >= 0 ? a >> b : ~((~a) >> b);
        }
    }
    else if (tb_shader_token_is(op, "<"))
    {
        result = a < b;
    }
    else if (tb_shader_token_is(op, ">"))
    {
        result = a > b;
    }
    else if (tb_shader_token_is(op, "<="))
    {
        result = a <= b;
    }
    else if (tb_shader_token_is(op, ">="))
    {
        result = a >= b;
    }
    else if (tb_shader_token_is(op, "=="))
    {
        result = a == b;
    }
    else if (tb_shader_token_is(op, "!="))
    {
        result = a != b;
    }
    else if (tb_shader_token_is(op, "&"))
    {
        result = a & b;
    }
    else if (tb_shader_token_is(op, "^"))
    {
        result = a ^ b;
    }
    else
    {
        result = a | b;
    }
    if (problem != NULL && evaluated)
    {
        fail(state, location, problem);
    }
    return result;
}

/* -------------------------------------------------------------------------
 * The grammar: C's, from `?:` down.
 * ------------------------------------------------------------------------- */

/* The precedence of a binary operator other than `&&` and `||`, or 0. */
static int precedence(const tb_shader_token *token)
{
    static const struct
    {
        const char *spelling;
        int level;
    } levels[] = {
        {"|", 1},  {"^", 2},  {"&", 3},  {"==", 4}, {"!=", 4}, {"<", 5}, {">", 5}, {"<=", 5},
        {">=", 5}, {"<<", 6}, {">>", 6}, {"+", 7},  {"-", 7},  {"*", 8}, {"/", 8}, {"%", 8},
    };
    if (token == NULL)
    {
        return 0;
    }
    for (size_t index = 0; index < sizeof(levels) / sizeof(levels[0]); index++)
    {
        if (tb_shader_token_is(token, levels[index].spelling))
        {
            return levels[index].level;
        }
    }
    return 0;
}

static bool enter(evaluator *state)
{
    if (state->nesting == MAX_NESTING)
    {
        fail(state, here(state), "the expression nests too deeply");
        return false;
    }
    state->nesting++;
    return true;
}

static int64_t parse_unary(evaluator *state, bool evaluated);

static int64_t parse_primary(evaluator *state, bool evaluated)
{
    const tb_shader_token *const token = current(state);
    if (token == NULL)
    {
        fail_at_token(state, "expected a value");
        return 0;
    }
    if (accept(state, "("))
    {
        const int64_t value = parse_conditional(state, evaluated);
        if (!accept(state, ")"))
        {
            fail_at_token(state, "expected ')', not");
        }
        return value;
    }
    if (token->kind == TB_SHADER_TOKEN_NUMBER)
    {
        state->position++;
        return parse_number(state, token);
    }
    if (token->kind == TB_SHADER_TOKEN_IDENTIFIER)
    {
        /* A name that is not a macro is 0, as in C, and `true` is 1, as in HLSL. */
        state->position++;
        return token->length == 4 && memcmp(token->text, "true", 4) == 0;
    }
    fail_at_token(state, "expected a value, not");
    return 0;
}

static int64_t parse_unary(evaluator *state, bool evaluated)
{
    const tb_shader_token *const op = current(state);
    const bool is_unary =
        op != NULL && (tb_shader_token_is(op, "+") || tb_shader_token_is(op, "-") ||
                       tb_shader_token_is(op, "~") || tb_shader_token_is(op, "!"));
    if (!is_unary)
    {
        return parse_primary(state, evaluated);
    }
    if (!enter(state))
    {
        return 0;
    }
    state->position++;
    const int64_t value = parse_unary(state, evaluated);
    state->nesting--;
    if (tb_shader_token_is(op, "-"))
    {
        if (value == INT64_MIN)
        {
            if (evaluated)
            {
                fail(state, op->location, "the negation overflows");
            }
            return 0;
        }
        return -value;
    }
    if (tb_shader_token_is(op, "~"))
    {
        return ~value;
    }
    if (tb_shader_token_is(op, "!"))
    {
        return !value;
    }
    return value;
}

/* Binary operators of `minimum` precedence and above, by precedence climbing. */
static int64_t parse_binary(evaluator *state, int minimum, bool evaluated)
{
    int64_t left = parse_unary(state, evaluated);
    for (;;)
    {
        const tb_shader_token *const op = current(state);
        const int level = precedence(op);
        if (level == 0 || level < minimum || state->failed)
        {
            return left;
        }
        state->position++;
        const int64_t right = parse_binary(state, level + 1, evaluated);
        left = apply(state, op, left, right, evaluated);
    }
}

static int64_t parse_and(evaluator *state, bool evaluated)
{
    int64_t value = parse_binary(state, 1, evaluated);
    while (accept(state, "&&"))
    {
        const bool right_evaluated = evaluated && value != 0;
        const int64_t right = parse_binary(state, 1, right_evaluated);
        value = value != 0 && right != 0;
    }
    return value;
}

static int64_t parse_or(evaluator *state, bool evaluated)
{
    int64_t value = parse_and(state, evaluated);
    while (accept(state, "||"))
    {
        const bool right_evaluated = evaluated && value == 0;
        const int64_t right = parse_and(state, right_evaluated);
        value = value != 0 || right != 0;
    }
    return value;
}

static int64_t parse_conditional(evaluator *state, bool evaluated)
{
    if (!enter(state))
    {
        return 0;
    }
    const int64_t condition = parse_or(state, evaluated);
    int64_t value = condition;
    if (accept(state, "?"))
    {
        const int64_t chosen = parse_conditional(state, evaluated && condition != 0);
        if (!accept(state, ":"))
        {
            fail_at_token(state, "expected ':', not");
        }
        const int64_t other = parse_conditional(state, evaluated && condition == 0);
        value = condition != 0 ? chosen : other;
    }
    state->nesting--;
    return value;
}

bool tb_shader_evaluate_condition(const tb_shader_token *tokens, size_t count,
                                  tb_shader_location directive, tb_shader_error_function *error,
                                  void *context, int64_t *value)
{
    evaluator state = {.tokens = tokens,
                       .count = count,
                       .position = 0,
                       .directive = directive,
                       .error = error,
                       .context = context,
                       .nesting = 0,
                       .failed = false};
    const int64_t result = parse_conditional(&state, true);
    if (!state.failed && state.position < count)
    {
        fail_at_token(&state, "unexpected");
    }
    if (state.failed)
    {
        return false;
    }
    *value = result;
    return true;
}
