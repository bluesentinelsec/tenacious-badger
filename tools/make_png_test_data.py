"""Make the PNG files that tests/media/bitmap_png_test.c reads, with
encoders independent of the engine's: this script's own, which compresses
with Python's zlib, and Windows' two, WIC (through WPF's PngBitmapEncoder)
and GDI+ (through System.Drawing), driven by Windows PowerShell.

    python tools/make_png_test_data.py [--powershell PATH]

Every image is the same pattern, which the test computes too: the sample of
channel c at (x, y) is pattern(x, y, c), cut to the bit depth. This script
writes every color type at every bit depth, interlaced and not, each row
filtered by a different one of the five filters, in the manner of PngSuite:

    py-c{color}-d{depth}-i{interlace}.png   the 13 by 11 pattern
    py-c{color}-d{depth}-trns.png           with a tRNS chunk
    py-tiny-{w}x{h}-i1.png                  interlaced images whose passes are empty
    py-chunks.png                           ancillary chunks of every kind, and IDAT
                                            chunks of 1 byte and of none
    py-apng.png                             an animated PNG: its default image is read
    py-level0.png                           stored blocks, no compression

WPF writes wic-*.png (Bgra32, Bgr24, Gray8, Gray16, Rgba64 and Indexed8, and
an interlaced Bgra32), and GDI+ writes gdiplus-argb.png. Their pixels are
the pattern at 8 or 16 bits. This is run at development time on Windows; the
files are committed, so the tests need none of the encoders.
"""
import argparse
import os
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

WIDTH = 13
HEIGHT = 11

SAMPLES = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
DEPTHS = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}

PASSES = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2),
          (0, 1, 1, 2)]  # first column, first row, columns apart, rows apart


def pattern(x, y, c, depth):
    """The sample of channel c at (x, y). Keep in step with the test."""
    value = (x * 4111 + y * 7919 + c * 1237 + x * y * 31 + (x ^ y) * 263) & 0xFFFF
    return value >> (16 - depth)


def palette_color(index):
    """Entry `index` of the palette of a palette image. Keep in step with the test."""
    return ((index * 71 + 5) & 255, (index * 113 + 40) & 255, (index * 29 + 7) & 255)


def palette_alpha(index):
    """The tRNS alpha of entry `index`, for the first half of the entries."""
    return (index * 97 + 3) & 255


def chunk(name, data):
    body = name + data
    return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)


def pack_row(samples, depth):
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b''.join(struct.pack('>H', s) for s in samples)
    out = bytearray()
    per_byte = 8 // depth
    for start in range(0, len(samples), per_byte):
        byte = 0
        group = samples[start:start + per_byte]
        for n, sample in enumerate(group):
            byte |= sample << (8 - depth * (n + 1))
        out.append(byte)
    return bytes(out)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filter_row(kind, row, above, back):
    out = bytearray([kind])
    for i, value in enumerate(row):
        a = row[i - back] if i >= back else 0
        b = above[i]
        c = above[i - back] if i >= back else 0
        predicted = [0, a, b, (a + b) // 2, paeth(a, b, c)][kind]
        out.append((value - predicted) & 255)
    return bytes(out)


def image_data(color, depth, width, height, interlaced, sample_of):
    """Filtered rows of every pass; the filter changes from row to row."""
    channels = SAMPLES[color]
    back = max(1, channels * depth // 8)
    passes = PASSES if interlaced else [(0, 0, 1, 1)]
    raw = bytearray()
    row_number = 0
    for x0, y0, dx, dy in passes:
        xs = range(x0, width, dx)
        ys = range(y0, height, dy)
        if len(xs) == 0 or len(ys) == 0:
            continue
        above = bytes(len(pack_row([0] * len(xs) * channels, depth)))
        for y in ys:
            samples = [sample_of(x, y, c) for x in xs for c in range(channels)]
            row = pack_row(samples, depth)
            raw += filter_row(row_number % 5, row, above, back)
            above = row
            row_number += 1
    return bytes(raw)


def png(color, depth, width, height, interlaced, extra_before=b'', idat_split=None, level=9,
        sample_of=None, after_idat=b''):
    if sample_of is None:
        def sample_of(x, y, c):
            return pattern(x, y, c, depth)
    header = struct.pack('>IIBBBBB', width, height, depth, color, 0, 0, 1 if interlaced else 0)
    compressed = zlib.compress(image_data(color, depth, width, height, interlaced, sample_of),
                               level)
    pieces = [compressed]
    if idat_split:
        pieces = [compressed[i:i + idat_split] for i in range(0, len(compressed), idat_split)]
    out = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + extra_before
    for piece in pieces:
        out += chunk(b'IDAT', piece)
    return out + after_idat + chunk(b'IEND', b'')


def palette_chunks(depth, transparent):
    count = 1 << depth
    plte = b''.join(bytes(palette_color(i)) for i in range(count))
    out = chunk(b'PLTE', plte)
    if transparent:
        out += chunk(b'tRNS', bytes(palette_alpha(i) for i in range(count // 2 + 1)))
    return out


def key_chunk(color, depth):
    """tRNS of gray or RGB: the color of the pixel at (1, 0) is transparent."""
    if color == 0:
        return chunk(b'tRNS', struct.pack('>H', pattern(1, 0, 0, depth)))
    return chunk(b'tRNS', b''.join(struct.pack('>H', pattern(1, 0, c, depth)) for c in range(3)))


def write_own(out):
    for color, depths in DEPTHS.items():
        for depth in depths:
            for interlaced in (0, 1):
                extra = palette_chunks(depth, False) if color == 3 else b''
                name = 'py-c%d-d%d-i%d.png' % (color, depth, interlaced)
                (out / name).write_bytes(png(color, depth, WIDTH, HEIGHT, interlaced, extra))
            if color in (0, 2):
                extra = key_chunk(color, depth)
            elif color == 3:
                extra = palette_chunks(depth, True)
            else:
                continue
            name = 'py-c%d-d%d-trns.png' % (color, depth)
            (out / name).write_bytes(png(color, depth, WIDTH, HEIGHT, 0, extra))
    for width, height in ((1, 1), (2, 3), (3, 1), (1, 5), (5, 5)):
        name = 'py-tiny-%dx%d-i1.png' % (width, height)
        (out / name).write_bytes(png(6, 8, width, height, 1))
    ancillary = (chunk(b'gAMA', struct.pack('>I', 45455)) + chunk(b'sRGB', b'\x00') +
                 chunk(b'cHRM', bytes(32)) + chunk(b'iCCP', b'profile\x00\x00' + zlib.compress(b'')) +
                 chunk(b'tEXt', b'Comment\x00made by make_png_test_data.py') +
                 chunk(b'pHYs', struct.pack('>IIB', 2835, 2835, 1)) + chunk(b'prVt', b'private'))
    (out / 'py-chunks.png').write_bytes(
        png(6, 8, WIDTH, HEIGHT, 0, ancillary + chunk(b'IDAT', b''), idat_split=1,
            after_idat=chunk(b'tIME', bytes(7))))
    # An animated PNG whose default image is its first frame; the second
    # frame, in fdAT, is not read.
    actl = chunk(b'acTL', struct.pack('>II', 2, 0))
    fctl = chunk(b'fcTL', struct.pack('>IIIIIHHBB', 0, WIDTH, HEIGHT, 0, 0, 1, 10, 0, 0))
    second = zlib.compress(bytes(HEIGHT * (WIDTH * 4 + 1)))
    fdat = (chunk(b'fcTL', struct.pack('>IIIIIHHBB', 1, WIDTH, HEIGHT, 0, 0, 1, 10, 0, 0)) +
            chunk(b'fdAT', struct.pack('>I', 2) + second))
    (out / 'py-apng.png').write_bytes(png(6, 8, WIDTH, HEIGHT, 0, actl + fctl, after_idat=fdat))
    (out / 'py-level0.png').write_bytes(png(2, 8, WIDTH, HEIGHT, 0, level=0))


WIC_SCRIPT = r'''
Add-Type -AssemblyName PresentationCore
Add-Type -AssemblyName System.Drawing
$out = $args[0]
$w = 13; $h = 11
function Pattern($x, $y, $c) { return ($x * 4111 + $y * 7919 + $c * 1237 + $x * $y * 31 + ($x -bxor $y) * 263) -band 0xFFFF }
function Save($name, $format, $bytesPerPixel, $fill, $interlace, $palette) {
    $stride = $w * $bytesPerPixel
    $pixels = New-Object byte[] ($stride * $h)
    for ($y = 0; $y -lt $h; $y++) { for ($x = 0; $x -lt $w; $x++) { & $fill $pixels ($y * $stride + $x * $bytesPerPixel) $x $y } }
    $source = [System.Windows.Media.Imaging.BitmapSource]::Create($w, $h, 96, 96, $format, $palette, $pixels, $stride)
    $encoder = New-Object System.Windows.Media.Imaging.PngBitmapEncoder
    if ($interlace) { $encoder.Interlace = [System.Windows.Media.Imaging.PngInterlaceOption]::On }
    else { $encoder.Interlace = [System.Windows.Media.Imaging.PngInterlaceOption]::Off }
    $encoder.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($source))
    $stream = [System.IO.File]::Create((Join-Path $out $name))
    $encoder.Save($stream)
    $stream.Close()
}
$F = [System.Windows.Media.PixelFormats]
$bgra = { param($p, $i, $x, $y) $p[$i] = (Pattern $x $y 2) -shr 8; $p[$i+1] = (Pattern $x $y 1) -shr 8; $p[$i+2] = (Pattern $x $y 0) -shr 8; $p[$i+3] = (Pattern $x $y 3) -shr 8 }
$bgr = { param($p, $i, $x, $y) $p[$i] = (Pattern $x $y 2) -shr 8; $p[$i+1] = (Pattern $x $y 1) -shr 8; $p[$i+2] = (Pattern $x $y 0) -shr 8 }
$gray8 = { param($p, $i, $x, $y) $p[$i] = (Pattern $x $y 0) -shr 8 }
$gray16 = { param($p, $i, $x, $y) $v = Pattern $x $y 0; $p[$i] = $v -band 255; $p[$i+1] = $v -shr 8 }
$rgba64 = { param($p, $i, $x, $y) for ($c = 0; $c -lt 4; $c++) { $v = Pattern $x $y $c; $p[$i + 2*$c] = $v -band 255; $p[$i + 2*$c + 1] = $v -shr 8 } }
$index = { param($p, $i, $x, $y) $p[$i] = (Pattern $x $y 0) -shr 8 }
Save 'wic-bgra32.png' $F::Bgra32 4 $bgra $false $null
Save 'wic-bgra32-i1.png' $F::Bgra32 4 $bgra $true $null
Save 'wic-bgr24.png' $F::Bgr24 3 $bgr $false $null
Save 'wic-gray8.png' $F::Gray8 1 $gray8 $false $null
Save 'wic-gray16.png' $F::Gray16 2 $gray16 $false $null
Save 'wic-rgba64.png' $F::Rgba64 8 $rgba64 $false $null
$colors = New-Object 'System.Collections.Generic.List[System.Windows.Media.Color]'
for ($k = 0; $k -lt 256; $k++) { $colors.Add([System.Windows.Media.Color]::FromRgb((($k * 71 + 5) -band 255), (($k * 113 + 40) -band 255), (($k * 29 + 7) -band 255))) }
$palette = New-Object System.Windows.Media.Imaging.BitmapPalette -ArgumentList (,$colors)
Save 'wic-indexed8.png' $F::Indexed8 1 $index $false $palette
$bitmap = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
for ($y = 0; $y -lt $h; $y++) { for ($x = 0; $x -lt $w; $x++) {
    $bitmap.SetPixel($x, $y, [System.Drawing.Color]::FromArgb((Pattern $x $y 3) -shr 8, (Pattern $x $y 0) -shr 8, (Pattern $x $y 1) -shr 8, (Pattern $x $y 2) -shr 8)) } }
$bitmap.Save((Join-Path $out 'gdiplus-argb.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
'''


def write_windows(out, powershell):
    with tempfile.TemporaryDirectory() as temporary:
        script = Path(temporary) / 'encode.ps1'
        script.write_text(WIC_SCRIPT, encoding='utf-8')
        subprocess.run([powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                        str(script), str(out)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--powershell', default='powershell.exe',
                        help='Windows PowerShell, for WIC and GDI+; "none" to skip them')
    args = parser.parse_args()
    out = Path(__file__).resolve().parent.parent / 'tests' / 'data' / 'png'
    out.mkdir(parents=True, exist_ok=True)
    write_own(out)
    if args.powershell != 'none':
        write_windows(out, args.powershell)
    print('wrote %d files to %s' % (len(os.listdir(out)), out))


if __name__ == '__main__':
    main()
