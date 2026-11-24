# Fonts

The fonts the engine library carries (docs/fonts.md): 15 families, 60
TrueType files, in roman and, where the family has one, true italic. The
build compresses them into the library; `TB_EMBED_FONTS` chooses all of
them, the families of the default roles, or none.

The files are kept byte for byte as the author's font pack had them. They
may be redistributed and embedded: 14 families are under the SIL Open Font
License 1.1, and Px437 IBM VGA 8x16 under CC BY-SA 4.0. The full text of
every license is in [licenses/](licenses/), and ships with the engine's
notices, in the SDK and in every exported game, where this file and the
licenses are in `licenses/fonts/` beside `NOTICE`.

| Family | Folder | Styles | License | Upstream |
| --- | --- | --- | --- | --- |
| EB Garamond | `serif/` | 400, 600, 700, and italics | OFL 1.1, [EBGaramond-OFL.txt](licenses/EBGaramond-OFL.txt) | [Google Fonts](https://fonts.google.com/specimen/EB+Garamond), the EB Garamond Project Authors |
| Source Serif 4 | `serif/` | 400, 600, 700, and italics | OFL 1.1, [SourceSerif4-OFL.txt](licenses/SourceSerif4-OFL.txt) | [Google Fonts](https://fonts.google.com/specimen/Source+Serif+4), the Source Serif 4 Project Authors |
| Libre Baskerville | `serif/` | 400, 600, 700, and italics | OFL 1.1, [LibreBaskerville-OFL.txt](licenses/LibreBaskerville-OFL.txt) | [Google Fonts](https://fonts.google.com/specimen/Libre+Baskerville), the Libre Baskerville Project Authors |
| Inter | `sans/` | 400, 600, 700, and italics | OFL 1.1, [Inter-OFL.txt](licenses/Inter-OFL.txt) | [Google Fonts](https://fonts.google.com/specimen/Inter), the Inter Project Authors |
| Roboto | `sans/` | 400, 600, 700, and italics | OFL 1.1, [Roboto-OFL.txt](licenses/Roboto-OFL.txt) | [Google Fonts](https://fonts.google.com/specimen/Roboto), the Roboto Project Authors |
| Source Sans 3 | `sans/` | 400, 600, 700, and italics | OFL 1.1, [SourceSans3-OFL.txt](licenses/SourceSans3-OFL.txt) | [Google Fonts](https://fonts.google.com/specimen/Source+Sans+3), Adobe |
| JetBrains Mono | `mono/` | 400, 600, 700, and italics | OFL 1.1, [JetBrainsMono-OFL.txt](licenses/JetBrainsMono-OFL.txt) | [Google Fonts](https://fonts.google.com/specimen/JetBrains+Mono), the JetBrains Mono Project Authors |
| Fira Code | `mono/` | 400, 600, 700 | OFL 1.1, [FiraCode-OFL.txt](licenses/FiraCode-OFL.txt) | [Google Fonts](https://fonts.google.com/specimen/Fira+Code), the Fira Code Project Authors |
| IBM Plex Mono | `mono/` | 400, 700, and italics | OFL 1.1, [IBMPlexMono-OFL.txt](licenses/IBMPlexMono-OFL.txt) | [Google Fonts](https://fonts.google.com/specimen/IBM+Plex+Mono), IBM Corp. |
| Press Start 2P | `retro/` | 400 | OFL 1.1, [PressStart2P-OFL.txt](licenses/PressStart2P-OFL.txt) | [Google Fonts](https://fonts.google.com/specimen/Press+Start+2P), the Press Start 2P Project Authors |
| VT323 | `retro/` | 400 | OFL 1.1, [VT323-OFL.txt](licenses/VT323-OFL.txt) | [Google Fonts](https://fonts.google.com/specimen/VT323), the VT323 Project Authors |
| Silkscreen | `retro/` | 400, 700 | OFL 1.1, [Silkscreen-OFL.txt](licenses/Silkscreen-OFL.txt) | [Google Fonts](https://fonts.google.com/specimen/Silkscreen), the Silkscreen Project Authors |
| Px437 IBM VGA 8x16 | `retro/` | 400 | CC BY-SA 4.0, [Px437-OldschoolPCFontPack-CC-BY-SA-4.0.txt](licenses/Px437-OldschoolPCFontPack-CC-BY-SA-4.0.txt) | [The Ultimate Oldschool PC Font Pack](https://int10h.org/oldschool-pc-fonts/), by VileR |
| Monocraft | `retro/` | 400, 600, 700, and italics | OFL 1.1, [Monocraft-OFL.txt](licenses/Monocraft-OFL.txt) | [IdreesInc/Monocraft](https://github.com/IdreesInc/Monocraft), Idrees Hassan |

**Static instances.** EB Garamond, Source Serif 4, Libre Baskerville,
Inter, Roboto, Source Sans 3, JetBrains Mono, and Fira Code are variable
fonts upstream; the files here are their static 400, 600, and 700 instances,
with their glyphs and kerning unchanged. The italics are the families' own
italic designs, not slanted romans. Fira Code and the pixel fonts have no
italic.

**Licenses.** The OFL lets the fonts be used, embedded, and redistributed,
with a game or an engine, free or sold, so long as the license travels with
them, the fonts are not sold by themselves, and a modified font does not
take a reserved font name. Px437 IBM VGA 8x16 is © VileR, from The Ultimate
Oldschool PC Font Pack, under CC BY-SA 4.0: it is attributed here and in
the engine's NOTICE, and a modified copy of the font file itself would have
to be shared under the same license. The engine does not modify it.

Three of these files are also in `tests/data/fonts/`, as test data of the
TrueType reader.
