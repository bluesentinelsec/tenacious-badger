# Fonts

The `font` module reads TrueType fonts (`.ttf`). Its code lives in
`src/font/`, and its header is `badger/font.h`. Everything is the engine's
own C; no font library is used. It reads; drawing glyphs into pixels, and
the fonts the engine carries, come later.

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
