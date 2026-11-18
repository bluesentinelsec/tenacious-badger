"""Write tests/core/decimal_vectors.h: floating-point text to test against.

    python tests/tools/make_decimal_vectors.py

Python converts between float and text with David Gay's correctly rounded
algorithms, which share no code with the engine's. Its repr is the shortest
text that reads back as the same value, its % operator formats %e, %f, and %g
as C specifies with exact rounding, ties to even, float.hex writes the exact
hexadecimal form, and float() reads text into the nearest value. This script
draws values and texts from a fixed seed, with many at the places where
conversions are hard, and records what Python makes of them. Run it only to
change the vectors; the output is checked in, and the build never runs it.

The output is formatted with the clang-format on PATH, or the one that the
CLANG_FORMAT environment variable names, as the format check expects.
"""
from decimal import Decimal, getcontext
import math
import os
from pathlib import Path
import random
import struct
import subprocess

OUTPUT = Path(__file__).resolve().parent.parent / 'core' / 'decimal_vectors.h'
SEED = 49
getcontext().prec = 2000

RANDOM_SHORTEST = 500
RANDOM_FORMATS = 1200
RANDOM_PARSE = 500
RANDOM_HALFWAY = 300


def bits_of(value):
    return struct.unpack('<Q', struct.pack('<d', value))[0]


def value_of(bits):
    return struct.unpack('<d', struct.pack('<Q', bits))[0]


def c_string(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def special_values():
    values = [0.0, -0.0, math.inf, -math.inf, math.nan, 5e-324, -5e-324,
              value_of(0x000FFFFFFFFFFFFF), value_of(0x0010000000000000),
              value_of(0x7FEFFFFFFFFFFFFF), 1.0, -1.0, 0.1, 0.2, 0.3, 1 / 3, 2 / 3,
              9007199254740992.0, 9007199254740993.0, 1e15, 1e16, 1e17, 9999999999999998.0,
              0.0001, 0.00001, 0.00009999999999999999, 123456789012345678.0, 1e22, 1e23,
              5e-310, 2.2250738585072014e-308, 1.7976931348623157e308, 0.5, 1.5, 2.5, 0.125,
              0.375, 100.0, 1234.5, 4.35, 2.675, 1e-7, 1.5e-7, 299792458.0, 6.02214076e23]
    for exponent in range(-1074, 1024, 37):
        values.append(math.ldexp(1.0, exponent))
    for exponent in range(-323, 309, 13):
        values.append(float('1e%d' % exponent))
    return values


def random_double(rng):
    """Bits drawn evenly, so every exponent is as likely as every other."""
    while True:
        value = value_of(rng.getrandbits(64))
        if math.isfinite(value):
            return value


def random_nice_double(rng):
    """A short decimal, as people write them, which shortest output must give back."""
    digits = rng.randrange(1, 10 ** rng.randrange(1, 17))
    return float('%de%d' % (digits, rng.randrange(-30, 30)))


def shortest_rows(rng):
    values = special_values()
    values += [random_double(rng) for _ in range(RANDOM_SHORTEST)]
    values += [random_nice_double(rng) for _ in range(RANDOM_SHORTEST // 2)]
    return [(bits_of(value), repr(value)) for value in values]


FORMAT_TEMPLATES = ['%e', '%f', '%g', '%E', '%F', '%G', '%#g', '%#.0f', '%#.0e', '%+e', '% f',
                    '%015.3f', '%-12.2e|', '%+010.4g', '%#G', '%12g', '%-+9.1f|', '% 09.2e',
                    '%010f', '%.0g', '%#.0g', '%.1g']


def random_format(rng):
    if rng.random() < 0.4:
        return rng.choice(FORMAT_TEMPLATES)
    flags = ''.join(flag for flag in '-+ 0#' if rng.random() < 0.15)
    width = str(rng.randrange(1, 30)) if rng.random() < 0.3 else ''
    precision = '.%d' % rng.choice([0, 1, 2, 3, 5, 10, 15, 16, 17, 20, 30, 40]) \
        if rng.random() < 0.8 else ''
    return '%' + flags + width + precision + rng.choice('eEfFgG')


def pads_with_zeros(template):
    """Whether a conversion has the 0 flag: a 0 among the flags after the %."""
    for character in template[1:]:
        if character not in '-+ #0':
            return False
        if character == '0':
            return True
    return False


def format_rows(rng):
    rows = []
    for value in special_values():
        for template in ('%e', '%f', '%g', '%.17g', '%.3e', '%.20f', '%#.0f', '%015.2F'):
            rows.append((template, value))
    for _ in range(RANDOM_FORMATS):
        value = random_double(rng) if rng.random() < 0.5 else random_nice_double(rng)
        if rng.random() < 0.2:
            value = round(value, rng.randrange(0, 4)) if abs(value) < 1e15 else value
        rows.append((random_format(rng), value))
    # Halfway cases, which round to even.
    for value in (0.5, 1.5, 2.5, 0.125, 0.375, 1e23, 2.5e-5, 1.25, 1.375):
        for template in ('%.0f', '%.1f', '%.2f', '%.0e', '%.1e', '%.2e', '%.1g', '%.2g'):
            rows.append((template, value))
    # The whole exact value of the extremes.
    rows.append(('%.1074f', 5e-324))
    rows.append(('%.760e', 5e-324))
    rows.append(('%.0f', 1.7976931348623157e308))
    rows.append(('%.1100e', 2.2250738585072014e-308))
    # Python pads an infinity or a NaN with zeros under the 0 flag, which C
    # forbids (C11 7.21.6.1): those are tested by hand instead.
    rows = [(template, value) for template, value in rows
            if math.isfinite(value) or not pads_with_zeros(template)]
    return [(template, bits_of(value), template % value) for template, value in rows]


def hex_text(value):
    """float.hex, in the shortest form that C's %a writes: no zeros at the end
    of the fraction, and no point when there is no fraction."""
    if math.isnan(value):
        return 'nan'
    if math.isinf(value):
        return '-inf' if value < 0 else 'inf'
    text = value.hex()
    mantissa, exponent = text.split('p')
    if '.' in mantissa:
        mantissa = mantissa.rstrip('0').rstrip('.')
    return mantissa + 'p' + exponent


def hex_rows(rng):
    values = special_values() + [random_double(rng) for _ in range(200)]
    return [(bits_of(value), hex_text(value)) for value in values]


def halfway_text(rng):
    """Text at, just below, or just above the exact midpoint between a double
    and the next one up: the hardest inputs to round."""
    value = abs(random_double(rng)) if rng.random() < 0.8 else abs(random_nice_double(rng))
    above = math.nextafter(value, math.inf)
    if not math.isfinite(above):
        return repr(value)
    middle = (Decimal(value) + Decimal(above)) / 2
    text = format(middle, 'e')
    mantissa, exponent = text.split('e')
    choice = rng.randrange(3)
    if choice == 0:
        return text
    digits = mantissa.replace('.', '')
    last = int(digits[-1])
    step = 1 if choice == 1 else -1
    if last + step < 0 or last + step > 9:
        digits = digits + ('1' if step == 1 else '')
        if step == -1:
            return text
    else:
        digits = digits[:-1] + str(last + step)
    return digits[0] + '.' + digits[1:] + 'e' + exponent


def random_text(rng):
    integer = ''.join(rng.choice('0123456789') for _ in range(rng.randrange(0, 25)))
    fraction = ''.join(rng.choice('0123456789') for _ in range(rng.randrange(0, 25)))
    if not integer and not fraction:
        integer = '0'
    text = integer
    if fraction or rng.random() < 0.3:
        text += '.' + fraction
    if rng.random() < 0.6:
        text += rng.choice('eE') + rng.choice(['', '+', '-']) + str(rng.randrange(0, 330))
    if rng.random() < 0.3:
        text = rng.choice('+-') + text
    return text


def parse_rows(rng):
    texts = ['0', '-0', '+0', '0.0', '.5', '5.', '1e0', '1E+2', '1e-2', '00012.3400',
             'inf', '-inf', 'INF', 'Infinity', '-iNfInItY', 'nan', 'NaN', '-nan',
             '9007199254740993', '9007199254740993.0000000000000000000001',
             '2.4703282292062327e-324', '2.4703282292062328e-324', '4.9406564584124654e-324',
             '1.7976931348623157e308', '1.7976931348623158e308', '1.7976931348623159e308',
             '1e400', '-1e400', '1e-400', '-1e-400', '1e999999999999999999999',
             '1e-999999999999999999999', '0e999999999999', '2.2250738585072011e-308',
             '2.2250738585072012e-308', '0.1', '0.30000000000000004', '123456789012345678901234567890',
             '0.' + '0' * 400 + '1e400', '1' + '0' * 400 + 'e-400',
             '0x1p0', '0x1.8p1', '-0X1.FFFFFFFFFFFFFP+1023', '0x1p-1074', '0x1p-1075',
             '0x1.0000000000001p-1075', '0x0.0000000000001p-1022', '0x1.fffffffffffff8p1023',
             '0x1.fffffffffffff7ffp1023', '0x.8', '0xA', '0x10.8p-4', '0x1.00000000000008p0',
             '0x1.00000000000008000001p0', '0x1.00000000000018p0', '0x0p0', '0x1p99999999999']
    # The midpoint between 1 and the next double, with a long tail of digits.
    middle = (Decimal(1) + Decimal(math.nextafter(1.0, 2.0))) / 2
    texts.append(format(middle, 'f'))
    texts.append(format(middle, 'f') + '0' * 1000)
    texts.append(format(middle, 'f') + '0' * 1000 + '1')
    texts += [halfway_text(rng) for _ in range(RANDOM_HALFWAY)]
    texts += [random_text(rng) for _ in range(RANDOM_PARSE)]
    texts += [random_double(rng).hex() for _ in range(50)]
    rows = []
    for text in texts:
        if text.lower().lstrip('+-').startswith('0x'):
            negative = text.startswith('-')
            body = text.lstrip('+-')
            try:
                value = float.fromhex(body)
            except OverflowError:
                # Python raises where IEEE 754 rounding gives infinity.
                value = math.inf
            value = -value if negative else value
        else:
            value = float(text)
        rows.append((text, bits_of(value)))
    return rows


def literal_lines(text, indent):
    pieces = [text[start:start + 76] for start in range(0, len(text), 76)] or ['']
    return '\n'.join(indent + c_string(piece) for piece in pieces)


def main():
    rng = random.Random(SEED)
    out = [
        '/* Floating-point text for decimal_test.c, computed by Python.',
        ' * Generated by tests/tools/make_decimal_vectors.py; change that script, not this file. */',
        '#ifndef TB_DECIMAL_VECTORS_H',
        '#define TB_DECIMAL_VECTORS_H',
        '',
        '#include <stdint.h>',
        '',
        '/* A double, by its bits, and its text. */',
        'typedef struct decimal_text_vector',
        '{',
        '    uint64_t bits;',
        '    const char *text;',
        '} decimal_text_vector;',
        '',
        '/* A format, a double, and what the format makes of it. */',
        'typedef struct decimal_format_vector',
        '{',
        '    const char *format;',
        '    uint64_t bits;',
        '    const char *text;',
        '} decimal_format_vector;',
        '',
        '/* Text, and the bits of the double it reads as. */',
        'typedef struct decimal_parse_vector',
        '{',
        '    const char *text;',
        '    uint64_t bits;',
        '} decimal_parse_vector;',
        '',
        '/* Python\'s repr: the shortest text that reads back as the value. */',
        'static const decimal_text_vector decimal_shortest_vectors[] = {',
    ]
    for bits, text in shortest_rows(rng):
        out.append(f'    {{0x{bits:016X}u, {c_string(text)}}},')
    out += ['};', '', '/* %a without a precision. */',
            'static const decimal_text_vector decimal_hex_vectors[] = {']
    for bits, text in hex_rows(rng):
        out.append(f'    {{0x{bits:016X}u, {c_string(text)}}},')
    out += ['};', '', 'static const decimal_format_vector decimal_format_vectors[] = {']
    for template, bits, text in format_rows(rng):
        out.append(f'    {{{c_string(template)}, 0x{bits:016X}u,')
        out.append(literal_lines(text, '     ') + '},')
    out += ['};', '', 'static const decimal_parse_vector decimal_parse_vectors[] = {']
    for text, bits in parse_rows(rng):
        out.append('    {')
        out.append(literal_lines(text, '        ') + ',')
        out.append(f'        0x{bits:016X}u}},')
    out += ['};', '', '#endif', '']
    OUTPUT.write_bytes('\n'.join(out).encode())
    subprocess.run([os.environ.get('CLANG_FORMAT', 'clang-format'), '-i', str(OUTPUT)],
                   check=True)


if __name__ == '__main__':
    main()
