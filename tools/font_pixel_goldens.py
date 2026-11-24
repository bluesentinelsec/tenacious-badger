"""Print the pixels of pixel fonts' glyphs, sampled at pixel centers.

    python tools/font_pixel_goldens.py FONT.ttf PIXELS_PER_EM CELL_WIDTH CELL_HEIGHT ASCENT TEXT

For each character of TEXT, prints the cell the glyph is drawn in, CELL_WIDTH
pixels wide from the pen and CELL_HEIGHT tall, its top row ASCENT pixels above
the baseline, as rows of '#' and '.': '#' where the pixel's center lies inside
the outline by the non-zero rule.

The tests of the rasterizer (tests/font/font_raster_test.c) expect these cells
of the aliased mode. This is a separate reader of the font, with Python's
standard library only, so that the expectation does not come from the code it
checks: it reads `head`, `loca`, `glyf`, and a format 4 `cmap`, and handles
simple glyphs only, which the pixel fonts' letters are.
"""
import struct
import sys


def tables(data):
    count = struct.unpack_from('>H', data, 4)[0]
    found = {}
    for index in range(count):
        tag, _, offset, length = struct.unpack_from('>4sIII', data, 12 + 16 * index)
        found[tag.decode('latin-1')] = data[offset:offset + length]
    return found


def glyph_of(cmap, code):
    count = struct.unpack_from('>H', cmap, 2)[0]
    for index in range(count):
        platform, encoding, offset = struct.unpack_from('>HHI', cmap, 4 + 8 * index)
        if (platform, encoding) not in ((3, 1), (0, 3), (0, 4), (3, 10)):
            continue
        if struct.unpack_from('>H', cmap, offset)[0] != 4:
            continue
        sub = cmap[offset:]
        segments = struct.unpack_from('>H', sub, 6)[0] // 2
        ends = struct.unpack_from('>%dH' % segments, sub, 14)
        starts = struct.unpack_from('>%dH' % segments, sub, 16 + 2 * segments)
        deltas = struct.unpack_from('>%dh' % segments, sub, 16 + 4 * segments)
        range_base = 16 + 6 * segments
        offsets = struct.unpack_from('>%dH' % segments, sub, range_base)
        for segment in range(segments):
            if starts[segment] <= code <= ends[segment]:
                if offsets[segment] == 0:
                    return (code + deltas[segment]) & 0xFFFF
                at = range_base + 2 * segment + offsets[segment] + 2 * (code - starts[segment])
                glyph = struct.unpack_from('>H', sub, at)[0]
                return (glyph + deltas[segment]) & 0xFFFF if glyph else 0
        return 0
    raise SystemExit('no format 4 Unicode cmap')


def contours_of(font, glyph):
    """The contours of a simple glyph, as lists of (x, y, on_curve)."""
    long_offsets = struct.unpack_from('>h', font['head'], 50)[0] == 1
    if long_offsets:
        start, end = struct.unpack_from('>II', font['loca'], 4 * glyph)
    else:
        start, end = (2 * value for value in struct.unpack_from('>HH', font['loca'], 2 * glyph))
    data = font['glyf'][start:end]
    if not data:
        return []
    contour_count = struct.unpack_from('>h', data, 0)[0]
    if contour_count < 0:
        raise SystemExit('glyph %d is compound; this reader reads simple glyphs only' % glyph)
    ends = struct.unpack_from('>%dH' % contour_count, data, 10)
    point_count = ends[-1] + 1
    at = 10 + 2 * contour_count
    instructions = struct.unpack_from('>H', data, at)[0]
    at += 2 + instructions
    flags = []
    while len(flags) < point_count:
        flag = data[at]
        at += 1
        flags.append(flag)
        if flag & 8:
            repeat = data[at]
            at += 1
            flags.extend([flag] * repeat)
    coordinates = []
    for short_bit, same_bit in ((2, 16), (4, 32)):
        value = 0
        values = []
        for flag in flags[:point_count]:
            if flag & short_bit:
                delta = data[at]
                at += 1
                value += delta if flag & same_bit else -delta
            elif not flag & same_bit:
                value += struct.unpack_from('>h', data, at)[0]
                at += 2
            values.append(value)
        coordinates.append(values)
    contours = []
    first = 0
    for last in ends:
        contours.append([(coordinates[0][i], coordinates[1][i], bool(flags[i] & 1))
                         for i in range(first, last + 1)])
        first = last + 1
    return contours


def edges_of(contours, steps=16):
    """Line edges of the contours, with quadratic curves cut into short lines."""
    edges = []
    for points in contours:
        # Make every implied on-curve point explicit, then start on one.
        full = []
        for index, point in enumerate(points):
            after = points[(index + 1) % len(points)]
            full.append(point)
            if not point[2] and not after[2]:
                full.append(((point[0] + after[0]) / 2, (point[1] + after[1]) / 2, True))
        first_on = next(i for i, point in enumerate(full) if point[2])
        full = full[first_on:] + full[:first_on]
        current = full[0]
        index = 1
        while index <= len(full):
            point = full[index % len(full)]
            if point[2]:
                edges.append((current[0], current[1], point[0], point[1]))
                current = point
                index += 1
                continue
            end = full[(index + 1) % len(full)]
            previous = current
            for step in range(1, steps + 1):
                t = step / steps
                x = (1 - t) ** 2 * current[0] + 2 * (1 - t) * t * point[0] + t * t * end[0]
                y = (1 - t) ** 2 * current[1] + 2 * (1 - t) * t * point[1] + t * t * end[1]
                edges.append((previous[0], previous[1], x, y))
                previous = (x, y)
            current = end
            index += 2
    return edges


def winding(edges, x, y):
    total = 0
    for x0, y0, x1, y1 in edges:
        if (y0 <= y < y1) or (y1 <= y < y0):
            crossing = x0 + (y - y0) * (x1 - x0) / (y1 - y0)
            if crossing < x:
                total += 1 if y1 > y0 else -1
    return total


def main():
    path, pixels, width, height, ascent, text = sys.argv[1:7]
    data = open(path, 'rb').read()
    font = tables(data)
    units = struct.unpack_from('>H', font['head'], 18)[0]
    scale = float(pixels) / units
    for character in text:
        edges = edges_of(contours_of(font, glyph_of(font['cmap'], ord(character))))
        print('%r at %s pixels per em:' % (character, pixels))
        for row in range(int(height)):
            # The center of the row, in font units, y up from the baseline.
            y = (int(ascent) - row - 0.5) / scale
            line = ''
            for column in range(int(width)):
                x = (column + 0.5) / scale
                line += '#' if winding(edges, x, y) != 0 else '.'
            print('    "%s",' % line)


if __name__ == '__main__':
    main()
