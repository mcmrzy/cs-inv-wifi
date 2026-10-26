# Native display font source

`unifont-16.0.04.hex.gz` is the unmodified GNU Unifont 16.0.04 hexadecimal
bitmap source. Original license notices are in `../font-license/`.
The Unifont subset is used under SIL Open Font License 1.1.

`ark-pixel-16px-proportional-latin.bdf` is the unmodified Ark Pixel Font 16px
proportional Latin build (`2026.09.01` release, the last 16px build) by TakWolf,
used under OFL-1.1 (`ark-pixel-OFL-1.1.txt`). It supplies the ASCII/Latin and
digit glyphs; the CJK glyphs still come from Unifont because Ark's 16px CJK
build is deprecated and incomplete.

From the project root, run:

```text
node tests/generate_gfx_native.cjs
```

The generator reads these local sources by default. `DIRECT_UNIFONT_PATH` and
`DIRECT_ARK_LATIN_PATH` override the paths, and `DIRECT_LATIN_FONT=ark16|unifont`
selects the Latin source (see the `LATIN_FONT` switch at the top of
`tests/generate_gfx_native.cjs`). It includes printable ASCII and the Unicode
characters used in `main/direct` C, header, and include sources, so regenerate
after changing Chinese translations. Only the emitted subset is compiled into
firmware.

ASCII/Latin is proportional inside a fixed 16-row cell (baseline row 14, matching
Unifont) so the existing layout keeps its vertical rhythm and mixed Latin/CJK
lines align; Chinese is native 16 by 16 pixels. Large digits are independently
authored monochrome 12 by 24 pixel segment glyphs. No glyph is resampled,
smoothed, or generated through an operating system font renderer.
