# Fonts

Three fonts of the font pack, copied unchanged, for the tests of the
TrueType reader in `tests/font/`. They are test data under their own
licenses, beside them; none of them is built into the engine.

| File | Family | License |
| --- | --- | --- |
| `Px437_IBM_VGA_8x16.ttf` | Px437 IBM VGA 8x16, of the Oldschool PC Font Pack by VileR | CC BY-SA 4.0: [Px437-OldschoolPCFontPack-CC-BY-SA-4.0.txt](Px437-OldschoolPCFontPack-CC-BY-SA-4.0.txt) |
| `Roboto-Regular.ttf` | Roboto, by the Roboto Project Authors | SIL Open Font License 1.1: [Roboto-OFL.txt](Roboto-OFL.txt) |
| `Silkscreen-Regular.ttf` | Silkscreen, by the Silkscreen Project Authors | SIL Open Font License 1.1: [Silkscreen-OFL.txt](Silkscreen-OFL.txt) |

They were chosen for what they hold between them: a bitmap-style font of
straight lines only, a pixel font with compound glyphs, and a text font of
quadratic curves, compound glyphs, and pair kerning in both formats of
GPOS. The values the tests expect of them were found by a separate reader
written with Python's standard library, which parses the same tables by
hand.
