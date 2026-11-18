# Arithmetic

Integers wider than the machine's. Each layer has one public header under
`include/badger/`, which is its API reference.

| Layer | Header | Use it for |
| --- | --- | --- |
| [Extended precision](#extended-precision) | `xp.h` | Unsigned integers of a fixed number of digits, in arrays the caller supplies. Allocates nothing. |
| [Arbitrary precision](#arbitrary-precision) | `ap.h` | Signed integers that grow as needed. Each value is an object from an allocator. Built on `xp.h`. |

The design follows the `XP` and `AP` interfaces of David Hanson's *C Interfaces and
Implementations*, adapted to the engine's conventions.

**Not for cryptography.** The time an operation takes depends on the values
of its operands, and memory is not cleared after use.

## Extended precision

`xp.h` computes with unsigned integers of any fixed length. A number of `n`
digits is an array of `n` `tb_xp_digit` values, and the caller owns every
array: the functions read and write the arrays they are given, keep no state,
and allocate nothing.

```c
/* 2^64 - 1, squared, in decimal. */
tb_xp_digit x[2] = {0};
tb_xp_digit square[4] = {0};
tb_xp_digit scratch[4] = {0};
char text[48] = {0};
size_t required = 0;

(void)tb_xp_from_u64(2, x, UINT64_MAX);
tb_xp_mul(square, 2, x, 2, x);
(void)tb_xp_to_string(4, square, 10, scratch, text, sizeof(text), &required);
/* text is "340282366920938463426481119284349108225" */
```

### Digits

A digit is a 32-bit unsigned integer, and digits are stored least significant
first: `x[0]` is the units digit in base 2^32. Products and quotients of two
digits are computed in 64 bits, which standard C provides on every platform;
no compiler intrinsic or 128-bit type is used.

Base 2^32 does a quarter of the digit operations that base 2^8 does for
addition and a sixteenth for multiplication and division, which grow with the
square of the number of digits.

Every length is at least one digit. `tb_xp_length` gives the significant
length, without leading zero digits, and returns 1 for zero so that its result
is always a valid length to pass on.

### Carries, borrows, and widths

An operation that can produce a wider result than its operands writes the low
digits and returns what did not fit:

| Function | Computes | Returns |
| --- | --- | --- |
| `tb_xp_add` | `z = x + y + carry` | the carry out, 0 or 1 |
| `tb_xp_sub` | `z = x - y - borrow`, modulo 2^(32n) | the borrow, 1 when the difference is negative |
| `tb_xp_neg` | `z = ~x + carry`; with a carry of 1, `-x` modulo 2^(32n) | the carry out |
| `tb_xp_sum`, `tb_xp_diff` | `z = x + y` and `z = x - y` for a single digit `y` | the carry or the borrow |
| `tb_xp_product` | `z = x * y` for a single digit `y` | the digit carried beyond `z` |
| `tb_xp_quotient` | `z = x / y` for a single digit `y` | the remainder |
| `tb_xp_mul` | `z = x * y` into `n + m` digits | nothing; the product always fits |
| `tb_xp_div` | `q = x / y` and `r = x % y` | nothing; both always fit |
| `tb_xp_shift_left`, `tb_xp_shift_right` | shifts by any count; bits shifted out are lost | nothing |

Passing a carry or a borrow into the next call chains numbers held in pieces.

### Division

`tb_xp_div` is long division after Knuth's Algorithm D (*The Art of Computer
Programming*, volume 2, section 4.3.1). It estimates each digit of the quotient
from the leading digits of the remainder, corrects the estimate, and adds back
the divisor in the rare case that the estimate is still one too large. The
tests reach every one of these steps with operands built to need them.

Division needs working space: `TB_XP_DIV_SCRATCH(n, m)` digits, which the
caller supplies like every other array. Dividing by zero is a contract
violation.

### Which arrays may be the same

| Function | The output may be the same array as |
| --- | --- |
| `tb_xp_add`, `tb_xp_sub` | `x`, `y`, or both |
| `tb_xp_neg`, `tb_xp_sum`, `tb_xp_diff`, `tb_xp_product`, `tb_xp_quotient`, shifts | `x` |
| `tb_xp_mul` | nothing; `x` and `y` may be the same array, to square |
| `tb_xp_div` | nothing; `q`, `r`, and the scratch digits are all apart |
| `tb_xp_to_string` | the scratch digits must be apart from `x` |

"The same array" means the same address and length. Arrays that overlap only
in part are always a contract violation. Builds with assertions check the
arguments that must be apart.

### Text

`tb_xp_from_string` reads digits in any base from 2 to 36, letters in either
case for the digits from 10 up. The text is digits and nothing else: no sign,
no prefix such as `0x`, and no space. It fails with `TB_XP_INVALID` for empty
text or any byte that is not a digit of the base, leaving the number as it
was, and with `TB_XP_TOO_LARGE` for a value that needs more digits than given.

`tb_xp_to_string` writes small letters and no leading zeros, and zero as `0`.
`tb_xp_string_size(n, base)` is a buffer size that holds the text of any
number of `n` digits. A smaller buffer is measured first, and when it cannot
hold the text the function returns `TB_XP_BUFFER_TOO_SMALL` and the size
required, and writes nothing.

Both conversions work several text digits at a time: as many as the largest
power of the base that fits in one digit, such as nine decimal digits.

## Arbitrary precision

`ap.h` has signed integers of any size. A `tb_ap` holds as many digits as its
value needs and a sign, in one block from an allocator.

```c
tb_ap *const base = tb_ap_from_i64(NULL, 3);
tb_ap *const exponent = tb_ap_from_i64(NULL, 1000);
tb_ap *const modulus = tb_ap_from_i64(NULL, 1000000007);
tb_ap *const power = tb_ap_pow(base, exponent, modulus); /* 3^1000 mod 1000000007 */

tb_ap_free(power);
tb_ap_free(modulus);
tb_ap_free(exponent);
tb_ap_free(base);
```

### Values never change

Every operation leaves its operands as they are and returns a new value, which
the caller owns and releases with `tb_ap_free`. A value can therefore be an
operand of any number of operations, both operands of one, and read by any
number of threads at once. A value made from a native integer or text uses the
allocator it is given; the result of an operation, and any working space it
needs for a moment, uses the allocator of its first operand. A computation in
an arena can drop every intermediate value at once by resetting the arena.

Each value has exactly one representation: zero is positive, and no value
has leading zero digits.

### Division rounds down

| | `tb_ap_div_floor`, `tb_ap_mod_floor` | C's `/` and `%` |
| --- | --- | --- |
| 7 and 2 | 3, 1 | 3, 1 |
| -7 and 2 | -4, 1 | -3, -1 |
| 7 and -2 | -4, -1 | -3, 1 |
| -7 and -2 | 3, -1 | 3, -1 |

The quotient rounds toward negative infinity, as in the book and in Python, and
the remainder has the sign of the divisor. A remainder modulo a positive
number is never negative, which is what modular arithmetic, hashing, and
wrapping an index need. `tb_ap_shift_right` rounds the same way, as an
arithmetic shift does: -1 shifted right is still -1. The names of the
functions carry `floor` so that the choice is visible where they are called.

### Native operands

`tb_ap_add_i64`, `tb_ap_sub_i64`, `tb_ap_mul_i64`, `tb_ap_div_floor_i64`,
`tb_ap_mod_floor_i64`, and `tb_ap_compare_i64` take an `int64_t` as the second
operand without making a value of it. `tb_ap_mod_floor_i64` returns a native
integer, since the remainder always fits.

`tb_ap_to_i64` and `tb_ap_to_u64` return `false` for a value outside the
range of the type, and leave the output as it was; they never wrap.

### Text and formatting

`tb_ap_from_string` reads an optional `+` or `-` and digits in a base from 2
to 36; `tb_ap_to_string` writes a `-` for a negative value and no leading
zeros. `tb_ap_format` is a conversion for the formatter: register it under a
letter, and it writes a value as `%d` writes an integer, flags, width, and
precision included.

```c
(void)tb_fmt_register('Z', tb_ap_format);
tb_fmt_print("%Z\n", power);
```

### Running out of memory

As everywhere in the engine, running out of memory ends the process through
the memory failure handler; no operation returns NULL. A result too large for
memory, such as a power whose exponent came from outside, therefore ends the
process too: bound such operands before computing with them. The tests refuse
each allocation of each operation in turn and check that every refusal
reaches the handler.
