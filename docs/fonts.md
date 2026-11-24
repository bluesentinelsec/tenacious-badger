# Fonts

The `font` module reads TrueType fonts (`.ttf`), draws their glyphs into
pixels, and lays out text. Its code lives in `src/font/`. `badger/font.h`
reads; `badger/font_raster.h` draws: glyphs rasterized into bitmaps of
coverage, and an atlas that packs them into one image for the renderer;
and `badger/font_layout.h` lays out UTF-8 into lines of placed glyphs,
apart from any renderer. Everything is the engine's own C; no font library
is used. The canvas draws text from the atlas
([renderer](renderer.md#2d-the-canvas)); the fonts the engine carries come
later.

## Opening a font

`tb_font_open` takes the bytes of a `.ttf` file, from a file, a package, or
memory, and borrows them: they must outlive the font, which
`tb_font_close` releases. Opening checks every table the font's queries
rely on, so a font that opens answers every query; only a glyph's outline
is checked when it is read, so that one damaged glyph spoils only itself.

```c
tb_font *font = NULL;
tb_font_error error = {0};
if (tb_font_open(bytes, length, NULL, &font, &error) != TB_FONT_OK)
{
    /* error.table is the table at fault, such as "cmap", and error.reason why. */
}
```

A font is immutable once open, and any number of threads may read it at
once. Reading an outline needs an arena; that is as thread-safe as the
arena is, so each thread gives its own.

## What a font tells

All measures are in the font's units, `units_per_em` to the em, with y up
from the baseline. To draw at a size of `pixels` to the em, scale by
`pixels / units_per_em`.

| Function | Gives | From |
| --- | --- | --- |
| `tb_font_get_metrics` | Units per em, glyph count, ascender, descender, line gap, cap and x heights, and the box around every glyph | `head`, `maxp`, `hhea`, `OS/2` |
| `tb_font_get_style` | Family, style, weight, and whether it is italic | `name`, `OS/2`, `head` |
| `tb_font_find_glyph` | The glyph that draws a code point, or 0, the missing glyph | `cmap` |
| `tb_font_get_glyph_metrics` | A glyph's advance and left side bearing | `hmtx` |
| `tb_font_get_kerning` | What to add to the advance of the first of two glyphs | `GPOS` or `kern` |
| `tb_font_get_outline` | A glyph's contours of lines and quadratic curves, and its box | `glyf`, `loca` |

- **Characters.** A Unicode `cmap` subtable of format 12 is used where the
  font has one, for every plane of Unicode; otherwise one of format 4, for
  the Basic Multilingual Plane. A font with neither is unsupported.
- **Names.** The typographic family and subfamily (name IDs 16 and 17) where
  the font has them, which group a family's weights under one name; else the
  family and subfamily (1 and 2). Windows names in US English come first,
  then other Windows names, then Unicode ones, then Mac Roman ones. They are
  decoded to UTF-8; a damaged UTF-16 surrogate becomes U+FFFD.
- **Weight and italic.** `usWeightClass` and the italic and oblique bits of
  `OS/2`'s `fsSelection`; a font without `OS/2` is bold or regular, and
  italic, as `head`'s `macStyle` says.
- **Kerning.** The pair adjustments of the lookups of `GPOS`'s `kern`
  feature, of either format, also through extension lookups. Each lookup
  applies its first subtable that applies to the pair, as shapers do, and
  the lookups' adjustments add up. A font whose `GPOS` has no `kern` feature
  kerns from the first horizontal format 0 subtable of its `kern` table.
  Scripts and languages are not told apart: every `kern` feature's lookups
  are used.
- **Outlines.** TrueType draws with on-curve points and off-curve control
  points; two off-curve points in a row imply an on-curve point halfway
  between them. The reader resolves that: each contour is a start point
  and segments, each a line or a quadratic Bézier curve with its control
  point, and the last ends at the start. Compound glyphs are made of other
  glyphs moved by offsets or by matching points, and scaled or transformed
  by a 2×2 matrix; the outline is the whole of them in place. Contours wind
  as TrueType's do, outer ones clockwise, and fill by the non-zero rule.

What is not read: hinting, the variations of a variable font (its default
instance is read), vertical metrics, glyph substitution, mark positioning
and every other `GPOS` adjustment, and fonts with PostScript outlines
(`OTTO`) or collections of fonts (`ttcf`), which are unsupported.

## Drawing glyphs

`badger/font_raster.h` turns outlines into pixels. A size is a number of
pixels to the em: at `pixels_per_em`, a font unit is
`pixels_per_em / units_per_em` pixels, which `tb_font_scale` gives.
`tb_font_get_line_metrics` scales the ascent, descent, and line gap.

```c
const tb_font_raster_options options = {16.0f, 0.0f, TB_FONT_RASTER_SMOOTH};
tb_font_glyph_image image = {0};
if (tb_font_rasterize_glyph(font, glyph, &options, arena, &image, NULL) == TB_FONT_OK)
{
    /* image.pixels: image.width * image.height bytes of coverage, rows from
     * the top. With the pen at (x, y) on the baseline, y growing down, the
     * bitmap's top-left pixel goes at (x + image.left, y - image.top), and
     * the pen moves on by image.advance. */
}
```

- **The bitmap** is the smallest box of whole pixels around every point of
  the outline, control points included. `tb_font_measure_glyph` gives its
  size and place, and the advance and side bearing in pixels, without
  drawing. A blank glyph, such as a space, has an advance and no bitmap.
- **Subpixel offsets.** `subpixel_x`, from 0 to 1, draws the glyph that far
  right of a whole pixel, so that text can be laid out at fractions of a
  pixel and still be drawn from cached bitmaps of a few offsets.
- **Limits.** Sizes are clamped to 2048 pixels to the em, and a size that is
  not a positive finite number draws nothing. A bitmap wider or taller than
  4096 pixels, or an outline of more than a million lines once its curves
  are cut, is `TB_FONT_TOO_LARGE`, so a damaged glyph cannot run away with
  time or memory.
- **Any outline.** `tb_font_rasterize_outline` draws an outline into a
  bitmap the caller gives, at any scale and offset, clipped to it.

### The smooth mode

The smooth mode gives each pixel the exact area of it inside the outline,
the anti-aliasing of FreeType's "smooth" renderer, font-rs, and
stb_truetype's second rasterizer.

1. Each quadratic curve is cut into lines that stray at most a twentieth
   of a pixel from it: a curve with ends p0 and p1 and control point c,
   cut into n equal steps of its parameter, strays by at most
   |p0 - 2c + p1| / 4n².
2. Each line adds, to every pixel of each row it crosses, the area of the
   pixel to its right within that row, signed by whether the line goes up
   or down. It adds it as differences along the row, so that the area of a
   pixel is the sum of the row's differences up to it: a line writes to
   only the pixels it passes through, and one more.
3. Summing each row gives each pixel's area inside the outline, the
   windings of the contours adding up. Its magnitude, at most 1, is the
   coverage, from 0 to 255.

Rows are summed a band of 32 at a time, so the memory a glyph takes while
it is drawn is 32 rows, whatever its size.

That is the non-zero rule exactly where one contour covers a pixel. Where
two contours wound the same way overlap within a pixel, adding their areas
counts the overlap twice, and the pixel comes out too dark; holes, wound
the other way, subtract correctly. Overlaps are common: Roboto draws a T as
its bar and its stem, and an e as one contour that crosses itself. So the
rasterizer looks for them, as edges that cross or a pixel that winds more
than once, and draws a glyph that has them again at 8 times the size each
way, averaging each 8 by 8 block of subpixels into a pixel. The error is
then confined to the subpixels where the overlap's edge lies: at most 1/16
of a pixel. FreeType does the same for the glyphs that a font marks as
overlapping; marks that fonts often leave out.

### The aliased mode

The aliased mode fills a pixel when its center is inside the outline by the
non-zero rule, and leaves it empty otherwise: each line adds its winding
at the first pixel whose center lies right of where it crosses the row's
center, and a pixel is full when its row's sum up to it is not zero. That
is for pixel fonts at the sizes they were drawn for, whose edges fall on
pixel edges, so that every pixel is wholly in or out: the VGA font
(Px437 IBM VGA 8x16) at 16 pixels to the em, Silkscreen at 8 or 16, Press
Start 2P at 8. At those sizes the smooth mode draws the same pixels.

## The atlas

A `tb_font_atlas` caches rasterized glyphs packed into one single-channel
image, from which the renderer draws text as textured quads.
`tb_font_atlas_get_glyph` finds a glyph there, or draws it there on first
use, and gives its rectangle in the image and how to place it.

```c
tb_font_atlas *atlas = tb_font_atlas_create(&(tb_font_atlas_options){1024, 1024, 1, 4}, NULL);
tb_font_atlas_glyph placed = {0};
const tb_font_raster_options options = {16.0f, pen_x - floorf(pen_x), TB_FONT_RASTER_SMOOTH};
if (tb_font_atlas_get_glyph(atlas, font, glyph, &options, &placed, NULL) == TB_FONT_OK)
{
    /* Draw placed.rect of the atlas's image at (floorf(pen_x) + placed.left,
     * baseline - placed.top), and move the pen on by placed.advance. */
}
```

- **Keys.** A glyph is kept by its font, glyph, size, mode, and subpixel
  bin. The pen's fraction of a pixel is rounded to the nearest of the
  atlas's `subpixel_bins` positions (4 is the usual choice for smooth
  text; 1, the default, draws at whole pixels); a fraction that rounds up
  to the next whole pixel is bin 0 with `left` one more. The atlas keeps a
  font by its address, so reset the atlas before closing a font it holds
  glyphs of.
- **Packing.** Glyphs are packed by a skyline: the top edge of what is
  packed, as level runs from left to right. A glyph goes where its bottom
  would be highest, resting on the runs beneath it. `padding` empty pixels
  (1 by default) surround every glyph, so that sampling one bilinearly never
  reads its neighbour.
- **Changes.** `tb_font_atlas_get_dirty` lists the rectangles of the image
  that changed since `tb_font_atlas_clear_dirty`: what to copy to the GPU's
  texture. Past 64 rectangles, the list becomes the one rectangle around
  them all. A new or cleared atlas lists its whole image.
- **Eviction.** When a glyph does not fit, the atlas clears: every glyph is
  forgotten, the image is cleared, and its generation, from
  `tb_font_atlas_generation`, counts up. Glyphs are then drawn again as they
  are asked for. Clearing everything, rather than evicting the least used
  glyphs, keeps packing simple and the image unfragmented; text uses few
  glyphs at few sizes, so an atlas of the right size rarely fills. A caller
  that has placed glyphs since the generation changed must ask for them
  again, because their rectangles may now hold other glyphs: the renderer
  checks the generation after laying out a frame's text, and lays it out
  again when it changed. A glyph larger than the whole atlas is
  `TB_FONT_TOO_LARGE`, and leaves the atlas as it was.

An atlas is externally synchronized: one thread at a time, as the renderer
lays out text.

The renderer keeps one atlas, and the texture that holds it on the GPU,
updated from its changed rectangles before each frame's 2D is drawn (see
[the renderer](renderer.md#how-it-draws)).

### Not done yet

- **Signed distance fields**, for text that scales and rotates in 3D and
  for outlines and glows, are not made: each size is rasterized.
- **Hinting**, and the LCD's subpixels (ClearType's colour fringes), are not
  used: glyphs are drawn as their outlines are, in grey.

## Laying out text

`tb_font_layout_text` turns UTF-8 into glyphs placed in pixels at a size,
in lines; the canvas draws what it lays out, and an interface measures
with it. It only reads the font, so it is thread-safe, and it allocates
the layout's arrays from an arena the caller gives.

```c
const tb_font_layout_options options = {.wrap_width = 300.0f, .align = TB_FONT_ALIGN_CENTER};
tb_font_layout layout = {0};
tb_font_layout_text(font, TB_TEXT("Hello, world"), 24.0f, &options, arena, &layout);
for (size_t index = 0; index < layout.glyph_count; index++)
{
    const tb_font_layout_glyph *glyph = &layout.glyphs[index];
    const tb_font_layout_line *line = &layout.lines[glyph->line];
    /* draw glyph->glyph with its pen at (line->x + glyph->x, line->baseline) */
}
```

- **Placing.** Each character is one glyph: the font's glyph for it, or
  glyph 0, the missing glyph. The pen moves by each glyph's advance, plus
  the kerning (`tb_font_get_kerning`) between it and the glyph before it on
  its line, scaled to the size and not rounded: the atlas's subpixel bins
  draw glyphs at fractions of a pixel.
- **Characters.** Each byte that does not begin a valid UTF-8 character
  reads as U+FFFD. `\n` ends a line; `\r` takes no place, so `\r\n` ends
  one line; a tab is as wide as four spaces.
- **Breaking lines.** Given `wrap_width`, a line breaks before a word that
  would end past it, after the spaces before that word; the spaces stay on
  the line they end, are not counted in its width, and never break a line
  themselves. A word wider than the whole width breaks between any two of
  its characters. Every line holds at least one character. A text that ends
  with `\n` ends with an empty line, where a caret can stand.
- **Lines.** From baseline to baseline is the font's line height
  (`tb_font_get_line_metrics`) times `line_spacing`. The first baseline is
  the ascent below the box's top. Lines are aligned left, center or right
  across the wrap width, or across the widest line when there is none.
- **Measuring.** `tb_font_measure_text` gives the widest line, the lines'
  height and their number, without allocating: what an interface sizes a
  box by. The layout gives each line's left, top, baseline and width, and
  the box of them all.
- **Carets.** Each placed glyph records the bytes it was read from, its pen
  and its advance, so a text field can find where a caret goes:
  `tb_font_layout_caret` gives the place before the character at a byte
  offset, and `tb_font_layout_hit` the caret nearest a point, by which half
  of a character it is over.
- **Pixel fonts.** With `pixel_size`, the font's own size in pixels (16 for
  the VGA font, 8 for Silkscreen), text is laid out at the whole multiple of
  it nearest the size asked for, at least once, every advance, kerning,
  alignment, ascent and line height rounded to whole pixels, and drawn
  aliased: each of the font's pixels becomes a square of whole pixels.

### Not done

Text is laid out one character to one glyph, left to right. Not done:
shaping (ligatures, and the joining and reordering that Arabic, Devanagari
and other complex scripts need, which is HarfBuzz's work), right-to-left and
bidirectional text, combining marks placed on their bases, falling back to
another font for a character this one lacks, colour emoji, hyphenation,
justification, and line breaking by Unicode's rules (UAX #14) rather than
at spaces.

## Damaged fonts

Fonts come from mods and downloads, so the reader treats their bytes as
hostile. Every offset, count, and length is checked before it is followed,
every array is checked to lie within its table, and every table within the
bytes. What lookups binary-search is checked to be in order. A compound
glyph nests at most 16 deep, which also ends a cycle of compounds, and an
outline holds at most a million points from at most a million components,
so no font can make a query run away with time or memory.

Each failure is `TB_FONT_MALFORMED`, `TB_FONT_TRUNCATED` (a table runs past
the end of the bytes), `TB_FONT_UNSUPPORTED`, or `TB_FONT_TOO_LARGE`, with
the table and a reason in `tb_font_error`.

## Tests

`tests/font/font_test.c` reads three fonts of the font pack, in
`tests/data/fonts/` with their licenses, against values that a separate
reader, written with Python's standard library, found in them: their
metrics and names, every glyph's outline and advance, every code point of
the BMP, and the kerning of every pair of printable ASCII. Fonts made in
memory by `tests/font/font_builder.c` cover what those three do not hold:
`cmap` format 12 and glyph arrays, the `kern` table, extension lookups,
every kind of component transform and placement, and names in each
encoding.

`tests/font/font_damage_test.c` provokes each check the reader makes, and
cuts the three fonts short at every length up to 512 bytes
and at hundreds more, and corrupts them at thousands of places, each time
in memory of exactly the font's length, so that the sanitizer build reports
any read past the end.

`tests/font/font_raster_test.c` checks the rasterizer against what the
outlines are, worked out by other means: that the coverage of a glyph adds
up to the area of its outline, by Green's theorem, and that every pixel of
every printable character of Roboto at three sizes matches a reference
rasterizer in the test that shares nothing with the engine's (it samples
16 heights a row, and counts windings along each). Outlines made by hand
check exact coverage of known shapes, the non-zero rule, clipping, and
degenerate and oversized outlines. The aliased VGA font and Silkscreen
must match, pixel for pixel, the cells that `tools/font_pixel_goldens.py`,
a separate reader in Python, samples from their outlines.
`tests/font/font_layout_test.c` checks layout against the font's own
measures: pens and widths from `tb_font_get_glyph_metrics` and
`tb_font_get_kerning`, lines broken at spaces, inside long words and at
`\n`, alignment, spacing, measuring that matches the layout, carets and
hits, U+FFFD for bad UTF-8, empty text, and pixel fonts on whole pixels.
`tests/render/text_d3d12_test.c` draws text on D3D12 on WARP: a string's ink
lies in the box its layout and glyph measures give, the VGA font's pixels
are exactly its aliased bitmap at 16 and 32 pixels, and glyphs drawn before
the atlas clears within a frame are still drawn right.
`tests/font/font_atlas_test.c` checks that packed glyphs never overlap with
their padding, over thousands of random glyphs and sizes, that the atlas
reports what changed, and that it clears and counts its generation when
full.
