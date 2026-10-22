# Memory Browser - Hex view

Core hex viewer + editor with 10 encodings, navigation, edit modes and
per-byte metadata (recently edited badge, PC/SP marker, symbol overlay,
annotation indicator, bookmark marker).

## Toolbar

3-row window header:

### Row 1 - File ops + Edit master + Region

```
[Load BIN] [Save BIN] | [Edit: OFF/ON] | Region: v
```

- **Load BIN** - loads a binary file and writes it to the region from
  cursor pos (disabled when Edit OFF / RO region)
- **Save BIN** - From/Size/Whole region dialog for exporting bytes to
  .bin (disabled if the region is empty / disconnected)
- **Edit: OFF/ON** - master toggle, red BG when ON. For RO regions
  (ROM, FLASH) it is force-disabled. F2 does the same.
- **Region v** - dropdown of all available regions (banking-aware,
  per-arch). See [layers-regions](layers-regions.md) for details.

### Row 2 - View options

```
Encoding v | Bytes/row v | [x] ASCII | [x] PC [x] SP | Origin
```

- **Encoding v** - 10 encodings for the ASCII column (see below)
- **Bytes/row v** - 8 / 16 / 32 (default 16)
- **ASCII** - toggle the ASCII column to the right of hex
- **PC** / **SP** - highlight rows containing Z80 PC / SP (Logical Z80
  region only)
- **Origin** - toggle per-row physical origin label (see
  [layers-regions](layers-regions.md))

### Row 3 - Navigation + Search

```
HEX:____ DEC:____ [Goto] [Search] < > Page: N of: M  0xXXXX
```

- **HEX:** input - hex address for goto (e.g. `C000` or `c000`, the
  `0x` prefix is not required)
- **DEC:** input - decimal address
- **Goto** - jump to the entered address (Ctrl+G focuses the HEX input)
- **Search** - toggle the search panel (see [search](search.md))
- **< >** - paging (page = 16 rows * bytes_per_row), keys PgUp/PgDn
- **Page: N of: M  0xXXXX** - status of current page + current cursor addr

## Hex table

Per-row layout (default 16 B/row):

```
| AAAA: | XX XX XX XX | XX XX XX XX | XX XX XX XX | XX XX XX XX  | ASCII... |
```

- **Address column** - adaptive hex width (4 digits for < 64 KB, 6 for
  < 16 MB, 8 for larger). Address = offset within the region.
- **Mark column** (1 char) - indicator character: '\*' = bookmark, 'P'
  = Z80 PC, 'S' = Z80 SP, '!' = freeze (= see Layers panel for details)
- **HEX column** - bytes split into groups of 4 by a `|` separator for
  readability. Per-byte custom BG color from the Layers panel (CDL X/R/W,
  Heatmap, etc.).
- **ASCII column** - per byte UTF-8 glyph according to the active
  encoding. Non-printable = `.`. Per-byte BG + "recently edited" yellow
  underline.

Render: ImGuiListClipper virtualization - smooth scrolling even for
the 16 MB Ramdisk. Per-row 32 B read through the backend (= chunk per
draw call).

## Encodings (10)

| ID | Encoding | Description |
|----|----------|-------|
| 0 | **Raw** | Printable ASCII 0x20-0x7E, otherwise `.` |
| 1 | Sharp MZ ASCII (EU) | Sharp MZ EU charset -> ASCII (1B per glyph) |
| 2 | Sharp MZ ASCII (JP) | Sharp MZ JP charset -> ASCII |
| 3 | Sharp MZ UTF-8 (EU) | Sharp MZ EU -> UTF-8 (multi-byte glyphs with diacritics) |
| 4 | Sharp MZ UTF-8 (JP) | Sharp MZ JP -> UTF-8 (incl. katakana) |
| 5 | Sharp MZ EU CG1 | MZ ASCII -> video code -> mzglyphs EU bank 1 (= live emu display look) |
| 6 | Sharp MZ EU CG2 | MZ ASCII -> video code -> mzglyphs EU bank 2 (= graphics chars) |
| 7 | Sharp MZ JP CG1 | ditto JP bank 1 |
| 8 | Sharp MZ JP CG2 | ditto JP bank 2 (= katakana / hiragana glyphs) |
| 9 | **KOI8-CS (Czech/Slovak)** | 8-bit encoding in the Sharp MZ CP/M ecosystem, full Czech diacritics |

CG variants (5-8) render **actual MZ CG-ROM bitmaps** via the
`mzglyphs.ttf` font in the PUA range U+E100-E4FF - i.e. you see what
would be displayed on the MZ screen in text mode.

KOI8-CS (9) - uses the full Cousine font (= contains Latin Extended-A
for c, d, e, z, s, r, u, n, ...). The SAPO-P scroll text is in KOI8-CS.

Changing the encoding **does not recompute the input across encodings**
(= silently aborts an active edit).

## HEX editing

In Edit mode + HEX sub-mode (default after F2):

- Cursor position in the hex column, half-byte nibble position
- Keys **0-9** / **A-F** write a nibble (high -> low -> next byte)
- Keypad **0-9** keys too
- Yellow underline (= "recently edited" badge) appears per edited byte
- No modifiers (Ctrl/Alt/Super = skip, collides with shortcuts)

Write-through via backend `write_bytes` - for VRAM regions it
immediately reflects in the video output. Per-byte undo push (max 10
levels per region).

## ASCII editing

Tab switches to ASCII sub-mode (= label "Mode: ASCII (Tab to switch)"
in the bottom bar, red caret under the glyph).

- Accepts **raw printable 7-bit ASCII 0x20..0x7E** directly from the
  keyboard (= letters, digits, punctuation, space)
- Non-ASCII chars (Czech diacritics, Kanji, MZ CG glyphs) **silent skip**
- For special characters use the **Char Inserter** window (see
  [search](search.md))

## Click in hex / ASCII column

- **LMB in the HEX column** -> cursor jumps to the clicked byte +
  edit_mode = HEX
- **LMB in the ASCII column** -> cursor jump + edit_mode = ASCII
- **RMB in either** -> context menu (bookmark, freeze, annotation,
  fill, undo/redo, insert character, show in Memory Map, PCG editor
  for PCG region, Who wrote this byte? - see
  [RAM Write Log](../ram-write-log.md))

## Keyboard navigation

| Key | Action |
|---------|------|
| **Arrows L/R** | cursor one byte left / right |
| **Arrows U/D** | cursor one row (= bytes_per_row) |
| **PgUp / PgDn** | scroll 16 rows + move cursor |
| **Home / End** | start / end of region |
| **Ctrl+Home/End** | ditto |
| **Ctrl+G** | focus the HEX goto input |
| **F2** | Edit toggle |
| **Tab** | HEX <-> ASCII sub-mode (in Edit) |
| **Esc** | Edit OFF |

## PC / SP markers

Only for the **Logical Z80 region** (= the 16-bit Z80 addr only makes
sense there). When enabled:
- **'P' marker** in the mark column on the Z80 PC row
- **'S' marker** on the Z80 SP row
- Whole row highlighted dimly (= does not overwrite the BG layers)

For non-Logical regions (RAM, ROM, VRAM, ...) the markers are always
OFF (= cursor.addr is an offset in the region, not a Z80 addr).

## Cursor info in the bottom bar

```
Cursor: 0x0000C005 = 0x6F 'o' (KOI8-CS)
```

- Address offset 8-digit (with zero-padding)
- Byte value hex
- ASCII glyph according to the active encoding
- Encoding label

When Edit ON, additionally:
```
... || Mode: HEX (Tab to switch)
```

## Recently edited badge

Yellow underline beneath the HEX byte and ASCII glyph for bytes edited
in the current session. Storage per instance, max 1024 entries FIFO.
Auto-cleared on region switch (= new session). Manual clear via the
**Clear edited** button in row 1.

**Auto-unmark after undo to original**: every record holds
`original_byte` (= value BEFORE the first edit in the session). After
undo (or manual edit back to the original value) the entry is removed
and the underline disappears.

## Symbol overlay

In the ASCII column, only for the Logical Z80 region + Symbols layer
ON. If the row contains a sym_db symbol, its name is prepended as a
yellow label with truncation. Hover tooltip with full name + kind +
comment.

## Annotation indicator

In the bottom bar below cursor info, if the byte under the cursor has
an annotation:

```
[#] Annotation: <text>
```

(see [edit-tools](edit-tools.md) for annotation details)

In V6.1 additionally a **hover tooltip** above the annotated byte (HEX
and ASCII columns) - ImGui tooltip with a color swatch + addr + text.
Implemented via manual `IsMouseHoveringRect` (hex cells are DrawList
renders, not ImGui items for IsItemHovered).

## Bookmark marker

`'*'` in the mark column if the byte has a sym_db entry with
`kind=BOOKMARK`. LMB click behaves the same as another hex cell. RMB
context menu Add/Remove bookmark.

## Related

- [layers-regions](layers-regions.md) - Layers panel (CDL/Heatmap/Delta/Frozen/Symbols), Regions tree, Multi-instance
- [search](search.md) - Search engine + Pattern Builder + Char Inserter
- [edit-tools](edit-tools.md) - Edit features (undo/redo, fill, annotations, freeze)
- [diff-pcg](diff-pcg.md) - Memory Diff window + PCG glyph editor
