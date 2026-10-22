# Sharp MZ ASCII, display code, standard ASCII

Sharp MZ machines use three distinct encodings. When reading text from
memory or from `emulator://video/text_dump`, you must know which one
you are looking at.

| Encoding | Where it appears | Range |
|----------|------------------|-------|
| **Sharp MZ ASCII** | Input to ROM print routines (`@MSG`, `@PRNTC`, `@RST18`), MZF filenames, CMT data | 0x00..0xFF |
| **Display code** | Byte stored in text VRAM (`0xD000..0xD3FF` in MZ-700 / MZ-800 700-compat) - index into CG-ROM | 0x00..0xFF |
| **Standard ASCII** | PC tools, CP/M, NIPOS, MRS | 0x00..0x7F |

The three are related by the ROM routine `@?ADCN` (Sharp ASCII -> display
code). For the inverse direction (display -> Sharp ASCII) there is
`@?DACN`.

Authoritative source:
`mz800-knowledge/public/reference/agent/conventions/sharpmz-ascii.md`.

## Text VRAM layout (MZ-700 / 700-compat)

| Region | Size | Contents |
|--------|------|----------|
| `0xD000..0xD3E7` | 1000 bytes | Display code, 40 columns x 25 rows, row-major. First byte = top-left cell. |
| `0xD3E8..0xD7FF` | unused | (=  outside the visible 40x25 grid) |
| `0xD800..0xDBE7` | 1000 bytes | Attribute byte per cell (= colour / inverse / PCG selector, depends on platform) |
| `0xDBE8..0xDFFF` | unused | |

`emulator://video/text_dump` returns 1000 bytes of display code + 1000
bytes of attribute in `chars_b64` and `attributes_b64` (base64) when
available.

## Display code -> standard ASCII (key ranges)

| Display range | Glyph(s) |
|---------------|----------|
| `0x00` | space (Sharp ASCII `0x20` -> display `0x00`) |
| `0x01..0x1A` | `A`..`Z` (uppercase letters, **not** at ASCII positions) |
| `0x20..0x29` | digits `0`..`9` (Sharp ASCII `0x30..0x39`) |
| `0x61..0x69` | `!`, `"`, `#`, `$`, `%`, `&`, `'`, `(`, `)` (Sharp ASCII `0x21..0x29`) |
| `0x80..` | lowercase + diacritics + graphics (rearranged) |
| `0xC0..0xC6` | DPCT control glyphs - SCROLL / CDOWN / CUP / CRIGHT / CLEFT / HOME / CLS |
| `0xF0` | NK (no key / non-printable placeholder) |

Note: the table above is partial. The full 256-entry mapping is
defined by the ROM routine `@?ADCN` (Sharp ASCII -> display code) at
ROM address `0x0BB9`, and its inverse `@?DACN` at `0x0BCE`. For the
complete table see the source document above (= ROM-derived).

## Sharp MZ ASCII -> standard ASCII (lowercase letters subset)

For 0x20..0x5D Sharp MZ ASCII is **identical** to standard ASCII
(space through `]`). Outside that range the mapping is sparse - the
notable lowercase mapping is in the 0x80..0xC0 region:

| Sharp | ASCII | Sharp | ASCII | Sharp | ASCII | Sharp | ASCII |
|-------|-------|-------|-------|-------|-------|-------|-------|
| `0x80` | `}` | `0x9D` | `r` | `0xA9` | `k` | `0xB7` | `o` |
| `0x8B` | `^` | `0x9E` | `p` | `0xAA` | `f` | `0xB8` | `l` |
| `0x90` | `_` | `0x9F` | `c` | `0xAB` | `v` | `0xBD` | `y` |
| `0x92` | `e` | `0xA0` | `q` | `0xAF` | `j` | `0xBE` | `{` |
| `0x94` | `~` | `0xA1` | `a` | `0xB0` | `n` | `0xC0` | `\|` |
| `0x96` | `t` | `0xA2` | `z` | `0xB3` | `m` |  |  |
| `0x97` | `g` | `0xA3` | `w` |  |  |  |  |
| `0x98` | `h` | `0xA4` | `s` |  |  |  |  |
| `0x9A` | `b` | `0xA5` | `u` |  |  |  |  |
| `0x9B` | `x` | `0xA6` | `i` |  |  |  |  |
| `0x9C` | `d` |  |  |  |  |  |  |

This table is enough to decode MZF filenames and Sharp ASCII text
buffers down to printable standard ASCII; for the full 256-entry
mapping (including graphics symbols, arrows, card suits, diacritics)
see the conventions reference above.

## Inverse video / attribute byte

The attribute byte at `0xD800 + cell_offset` controls colour and other
flags per cell. Layout depends on the platform:

- **MZ-700 / MZ-1500 / MZ-800 in 700-compat**: 4-bit foreground + 4-bit
  background colour (one common interpretation); bit 7 of the attribute
  selects PCG set in MZ-700 mode `[unverified for MZ-800 700-compat -
  consult GDG documentation]`.
- **MZ-800 native graphics mode**: text VRAM region is not used as text
  - `emulator://video/text_dump` returns `available=false`.

Consult `emulator://periph/gdg` and the GDG documentation in
`docs/{cz,en}/debugger/gdg-panel.md` for the live attribute byte
interpretation per active mode.

## Decoding `emulator://video/text_dump` (Python sketch)

```python
import base64, json

dump = json.loads(read_resource("emulator://video/text_dump"))
if not dump.get("available"):
    raise RuntimeError(dump.get("reason"))

chars = base64.b64decode(dump["chars_b64"])
# chars[i] is the display code at row i//40, column i%40.

# Display code -> Sharp MZ ASCII -> standard ASCII (two-step).
# For a quick text dump of letters / digits use the inverse @?DACN
# mapping. For a complete mapping use the table from
# mz800-knowledge/public/reference/agent/conventions/sharpmz-ascii.md.
```

For full UTF-8 conversion (including arrows, card suits, German /
Czech diacritic glyphs) consult the same convention document - it
lists Unicode equivalents for the graphic Sharp MZ ASCII codes
(e.g. `0x5E` -> U+2191 "up arrow", `0xFF` -> U+03C0 "pi").

## Quick lookup: MZF filename encoding

MZF headers store the filename in **Sharp MZ ASCII** (16 bytes). A
filename like `"hello"` in standard ASCII must be converted byte by
byte: `h=0x68 -> 0x98`, `e=0x65 -> 0x92`, `l=0x6C -> 0xB8`, ...

Forgetting to convert produces "garbage" graphics symbols when the ROM
monitor prints the MZF filename through `@MSG`. The `bin2mzf` tool
performs this conversion automatically with `--charset eu` (default).

## Related

- `emulator://video/text_dump` - live text VRAM dump (display code).
- `emulator://docs/memory_layout` - where the text VRAM region lives.
- `mz800-knowledge/public/reference/agent/conventions/sharpmz-ascii.md` - full reference (Sharp MZ ASCII <-> ASCII <-> display code).
- ROM routine `@?ADCN` at `0x0BB9`, `@?DACN` at `0x0BCE`.
