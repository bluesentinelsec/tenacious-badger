"""Write tests/codec/deflate_vectors.h: streams made by zlib, for the decoder.

The issue asks that the engine decode what the reference implementation
writes. Python's zlib module is zlib itself, so this script compresses
deterministic inputs with it, at several levels and strategies and in every
framing, and stores the compressed bytes as C arrays. The inputs are not
stored: tests/codec/inflate_test.c makes them again with the same generator
(sample_bytes there, sample_bytes here) and compares.

This runs at development time only; the build and the tests need no Python.
Run from the repository root: python tools/generate_deflate_vectors.py, then
format the sources (build.bat fmt), which rejoins the short last row of each array.
"""

import struct
import zlib
from pathlib import Path

# Kinds of input, made identically by sample_bytes in inflate_test.c.
TEXT = 0       # Numbered lines of English: compressible, with short matches.
RANDOM = 1     # A linear congruential generator: incompressible.
ZEROS = 2      # One byte repeated: the longest matches.
FAR_MATCH = 3  # 300 random bytes, text, then the same 300 bytes ~32000 back.

FAR_PREFIX = 300
FAR_GAP = 31700


def lcg_bytes(length, seed):
    state = seed
    out = bytearray()
    for _ in range(length):
        state = (state * 1103515245 + 12345) & 0xFFFFFFFF
        out.append((state >> 16) & 0xFF)
    return bytes(out)


def text_bytes(length):
    out = bytearray()
    line = 0
    while len(out) < length:
        out += b'line %05d: the quick brown fox jumps over the lazy dog\n' % line
        line += 1
    return bytes(out[:length])


def sample_bytes(kind, length):
    if kind == TEXT:
        return text_bytes(length)
    if kind == RANDOM:
        return lcg_bytes(length, 12345)
    if kind == ZEROS:
        return bytes(length)
    prefix = lcg_bytes(FAR_PREFIX, 777)
    whole = prefix + text_bytes(FAR_GAP) + prefix
    assert len(whole) == length
    return whole


def deflate(data, level, strategy=zlib.Z_DEFAULT_STRATEGY, wbits=-15, flushes=()):
    """Compress, with optional flushes at given offsets."""
    compressor = zlib.compressobj(level, zlib.DEFLATED, wbits, 9, strategy)
    out = bytearray()
    start = 0
    for offset, mode in flushes:
        out += compressor.compress(data[start:offset])
        out += compressor.flush(mode)
        start = offset
    out += compressor.compress(data[start:])
    out += compressor.flush(zlib.Z_FINISH)
    return bytes(out)


def gzip_member(data, level, name=None, comment=None, extra=None, header_crc=False):
    """A gzip member written field by field, so every optional part is covered."""
    flags = 0
    if header_crc:
        flags |= 0x02
    if extra is not None:
        flags |= 0x04
    if name is not None:
        flags |= 0x08
    if comment is not None:
        flags |= 0x10
    header = bytearray(b'\x1f\x8b\x08' + bytes([flags]) + struct.pack('<I', 0) + b'\x00\xff')
    if extra is not None:
        header += struct.pack('<H', len(extra)) + extra
    if name is not None:
        header += name + b'\x00'
    if comment is not None:
        header += comment + b'\x00'
    if header_crc:
        header += struct.pack('<H', zlib.crc32(bytes(header)) & 0xFFFF)
    trailer = struct.pack('<II', zlib.crc32(data), len(data) & 0xFFFFFFFF)
    return bytes(header) + deflate(data, level) + trailer


RAW, ZLIB, GZIP = 'TB_DEFLATE_RAW', 'TB_DEFLATE_ZLIB', 'TB_DEFLATE_GZIP'
FAR_LENGTH = 2 * FAR_PREFIX + FAR_GAP


def vectors():
    """(name, format, kind, length, members, compressed); members > 1 is concatenated gzip."""
    text = sample_bytes(TEXT, 20000)
    rows = []
    for level in (1, 6, 9):
        rows.append((f'zlib_text_level{level}', ZLIB, TEXT, 20000, 1,
                     zlib.compress(text, level)))
    rows.append(('raw_text_level6', RAW, TEXT, 20000, 1, deflate(text, 6)))
    rows.append(('raw_far_match_level9', RAW, FAR_MATCH, FAR_LENGTH, 1,
                 deflate(sample_bytes(FAR_MATCH, FAR_LENGTH), 9)))
    rows.append(('zlib_random_stored', ZLIB, RANDOM, 3000, 1,
                 zlib.compress(sample_bytes(RANDOM, 3000), 6)))
    rows.append(('zlib_text_level0', ZLIB, TEXT, 3000, 1, zlib.compress(text[:3000], 0)))
    rows.append(('raw_text_fixed', RAW, TEXT, 4000, 1,
                 deflate(text[:4000], 6, zlib.Z_FIXED)))
    rows.append(('raw_text_huffman_only', RAW, TEXT, 4000, 1,
                 deflate(text[:4000], 6, zlib.Z_HUFFMAN_ONLY)))
    rows.append(('raw_zeros_rle', RAW, ZEROS, 50000, 1,
                 deflate(sample_bytes(ZEROS, 50000), 6, zlib.Z_RLE)))
    rows.append(('zlib_zeros_level9', ZLIB, ZEROS, 100000, 1,
                 zlib.compress(sample_bytes(ZEROS, 100000), 9)))
    rows.append(('raw_text_flushes', RAW, TEXT, 10000, 1,
                 deflate(text[:10000], 6, flushes=((1000, zlib.Z_SYNC_FLUSH),
                                                   (5000, zlib.Z_FULL_FLUSH),
                                                   (7000, zlib.Z_SYNC_FLUSH)))))
    for framing, name in ((RAW, 'raw'), (ZLIB, 'zlib')):
        wbits = -15 if framing == RAW else 15
        rows.append((f'{name}_empty', framing, TEXT, 0, 1, deflate(b'', 6, wbits=wbits)))
    rows.append(('gzip_empty', GZIP, TEXT, 0, 1, gzip_member(b'', 6)))
    rows.append(('gzip_text_plain', GZIP, TEXT, 5000, 1, gzip_member(text[:5000], 6)))
    rows.append(('gzip_text_every_header_field', GZIP, TEXT, 5000, 1,
                 gzip_member(text[:5000], 9, name=b'sample.txt', comment=b'made for tests',
                             extra=b'XY\x04\x00data', header_crc=True)))
    # Two members of 2500 bytes each decode to the first 2500 bytes twice; the
    # test checks that against the sample, so the halves are the same text.
    rows.append(('gzip_two_members', GZIP, TEXT, 2500, 2,
                 gzip_member(text[:2500], 6) + gzip_member(text[:2500], 1, name=b'b')))
    return rows


def render(rows):
    lines = [
        '/* Generated by tools/generate_deflate_vectors.py from Python\'s zlib; do',
        ' * not edit by hand. Streams made by the reference implementation, for',
        ' * tests/codec/inflate_test.c to decode. Each decodes to sample_bytes(kind,',
        ' * length), repeated `members` times. */',
        '#ifndef TB_DEFLATE_VECTORS_H',
        '#define TB_DEFLATE_VECTORS_H',
        '',
        '#include "badger/deflate.h"',
        '',
        '#include <stddef.h>',
        '',
    ]
    for name, _, _, _, _, data in rows:
        lines.append(f'static const unsigned char vector_{name}[{len(data)}] = {{')
        for start in range(0, len(data), 16):
            chunk = ', '.join(f'0x{byte:02X}' for byte in data[start:start + 16])
            lines.append(f'    {chunk},')
        lines.append('};')
        lines.append('')
    lines += [
        'typedef struct deflate_vector',
        '{',
        '    const char *name;',
        '    tb_deflate_format format;',
        '    int kind;',
        '    size_t length;',
        '    size_t members;',
        '    const unsigned char *data;',
        '    size_t size;',
        '} deflate_vector;',
        '',
        'static const deflate_vector deflate_vectors[] = {',
    ]
    for name, framing, kind, length, members, data in rows:
        lines.append(f'    {{"{name}", {framing}, {kind}, {length}, {members}, vector_{name}, '
                     f'sizeof(vector_{name})}},')
    lines += ['};', '', '#endif', '']
    return '\n'.join(lines)


def main():
    rows = vectors()
    for name, framing, kind, length, members, data in rows:
        wbits = {RAW: -15, ZLIB: 15, GZIP: 31}[framing]
        decoder = zlib.decompressobj(wbits)
        plain = decoder.decompress(data)
        while decoder.unused_data and framing == GZIP:
            rest = decoder.unused_data
            decoder = zlib.decompressobj(wbits)
            plain += decoder.decompress(rest)
        assert plain == sample_bytes(kind, length) * members, name
    target = Path(__file__).resolve().parent.parent / 'tests' / 'codec' / 'deflate_vectors.h'
    target.write_bytes(render(rows).encode('ascii'))
    total = sum(len(row[5]) for row in rows)
    print(f'{len(rows)} vectors, {total} compressed bytes')


if __name__ == '__main__':
    main()
