# Arithmetic

Integers wider than the machine's. Each layer has one public header under
`include/badger/`, which is its API reference.

| Layer | Header | Use it for |
| --- | --- | --- |
| [Extended precision](#extended-precision) | `xp.h` | Unsigned integers of a fixed number of digits, in arrays the caller supplies. Allocates nothing. |

The design follows the `XP` interface of David Hanson's *C Interfaces and
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
