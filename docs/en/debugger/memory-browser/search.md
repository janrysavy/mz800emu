# Memory Browser - Search engine + Pattern Builder + Char Inserter

The search engine in Memory Browser allows searching for byte
sequences, text, word values, regular expressions or expression
results across a single region or all regions of the current
architecture. For convenient pattern construction there is the
**Pattern Builder** (dual editable HEX + ASCII fields) and the
**Char Inserter** (special character palette).

The search is one-shot. To find a variable whose value you do not know
(lives, score, a timer) by watching how it changes, use
[Memory Search](../memory-search.md).

## 1. Opening the search panel

| Path | What happens |
|---|---|
| **Key** `Ctrl+F` | Toggle the search panel (= extends the main window with search rows) |
| **Button** `Search` in toolbar row 3 | Opens the panel |
| **Key** `Esc` | Closes the panel |

The search panel is a part of the Memory Browser window, not a separate
window. After closing, the settings are preserved in cfgmain (= per
instance).

## 2. Search types

The **Search type** dropdown has 7 + 1 variants. The recommended type
for most use is the first one (`Bytes (HEX + ASCII)`); the others are
either specialized or kept for backward compatibility.

| # | Type | Description |
|---|-----|-------|
| 1 | **Bytes (HEX + ASCII)** | V4.1+ Pattern Builder, two synchronized editable fields (HEX + ASCII). Recommended default. |
| 2 | **Byte sequence (hex, legacy)** | V0 - single hex field `AA BB CC`. Kept for compatibility. |
| 3 | **ASCII (current encoding, legacy)** | V0 - single text field, compiled via the current encoding. |
| 4 | **Word LE (16-bit)** | A literal like `0x1234` is encoded as little-endian bytes `34 12`. |
| 5 | **Word BE (16-bit)** | A literal `0x1234` is encoded as big-endian bytes `12 34`. |
| 6 | **Masked (AA ?? BB)** | Hex sequence with `??` as wildcard for any byte. |
| 7 | **Regex (POSIX ERE)** | `std::regex_extended` over raw region bytes. |
| 8 | **Expression (per byte)** | For each byte the expression `bp_expr` is evaluated (`Value = byte`, `Address = offset`). Match occurs when the result is non-zero. |

## 3. Pattern Builder (Bytes type)

The most modern UX (V4.1+). Two synchronized editable fields:

```
Search type: [ Bytes (HEX + ASCII) v ]
HEX:    [56 C1 DA 65 6E C9    ]  ASCII: [Vazeni    ]  [+]  [H]
Scope: [Current region v]  [ ] Case sens.  [Find Next] [Prev] [All] [Cancel] [Close]
```

- **HEX field** (left): free-form hex `56 C1 DA 65 6E C9`. Spaces and
  commas are optional. Tooltip: `HEX bytes: AA BB CC (spaces optional)`.
- **ASCII field** (right): rendered from the byte buffer via the
  current encoding (e.g. KOI8-CS -> `Vazeni`, Raw -> dots for non-ASCII).
  The field is editable - edits walk UTF-8 codepoints, for each char
  `utf8_to_byte(current_encoding)` is called and the resulting byte
  overwrites the byte buffer. Tooltip: `ASCII view (current encoding).
  Type to update HEX. Use [+] for special chars.`
- **[+] Insert character** opens the Char Inserter window in CALLBACK
  mode (= clicking a cell in the palette appends a byte to the pattern
  buffer, not to memory).
- **[H] History** opens a dropdown with the last 8 patterns.

### HEX <-> ASCII synchronization

- Edit in the HEX field -> ASCII scratch buffer is regenerated per
  frame (= only when the ASCII field is **not** focused).
- Edit in the ASCII field -> re-parse via encoding, update both the
  byte buffer and the HEX field.
- Focus is detected via ImGui `GetActiveID()` so the HEX-driven
  regeneration does not override an in-progress ASCII edit.

The canonical pattern storage = hex string in `search_pattern[]`. Only
that is persisted in cfgmain.

## 4. Encoding-aware case fold

The **Case sens** checkbox enables case-sensitive comparison. When
unchecked, search uses an encoding-aware tolower table - working for
**all encodings** where a case pair exists:

| Encoding | Case fold coverage |
|----------|---------------------|
| **Raw** | Latin `A-Z` (0x41-0x5A -> 0x61-0x7A) |
| **SharpMZ EU** | Latin + EU national chars (per SharpMZ EU charset table) |
| **SharpMZ JP** | Latin + JP national chars |
| **KOI8-CS** | Latin + full Czech/Slovak diacritics (A->a 0xE1->0xC1, Z->z 0xFA->0xDA, C->c, R->r, S->s, U->u, T->t etc.) |
| **CG variants** (CG1/CG2/UTF8) | Aliased to canonical `_ASCII` counterpart (identical byte layout, only different render) |

Implementation: per-encoding lazy-init 256B table. Per-byte algorithm
= `byte -> utf8 -> codepoint -> g_unichar_tolower -> utf8 -> byte` via
GLib.

**Case fold example**: SAPO-P scroll text contains `Vazeni` in KOI8-CS
as bytes `56 C1 DA 65 6E C9`. Pattern `VAZENI` (all uppercase) in
encoding KOI8-CS with **Case sens** off also finds lowercase `Vazeni`
(KOI8-CS uppercase bytes `0xE1/0xFA/0xE9` fold to lowercase
`0xC1/0xDA/0xC9`).

## 5. Char Inserter window

Singleton window with a palette of special characters (V1.5+). Three tabs:

| Tab | Encoding | Glyphs |
|-----|----------|-------|
| **ASCII EU** | SharpMZ EU CG1 | MZ CG-ROM bitmaps from `mzglyphs.ttf`, PUA `U+E100-E1FF` |
| **ASCII JP** | SharpMZ JP CG1 | MZ CG-ROM bitmaps from `mzglyphs.ttf`, PUA `U+E300-E3FF` |
| **KOI8-CS** | KOI8-CS | Standard UTF-8 via default font (Czech diacritics) |

Per-tab layout: 16x16 grid (= 256 bytes `0x00..0xFF`), per cell
glyph + tooltip `0xNN - <utf8 glyph>`.

### Opening and targets

| Path | Target | What clicking a cell does |
|---|---|---|
| **Hex view context menu** -> `Insert character...` | `MB_CURSOR` mode - cursor pos of the source MB instance | Writes the byte to memory, moves cursor, pushes undo |
| **Search panel** -> `[+]` next to the ASCII field | `CALLBACK` mode - search pattern buffer | Appends the byte to the `search_pattern` hex string |
| **Menu Debugger -> Char Inserter** | Default MB instance (#0 main) | Same as context menu (`MB_CURSOR` mode) |

The status row at the top of the window shows the current target:

- `MB_CURSOR` mode: `Target: MB main @ 0xC100  Region: ...  [Edit ON]`
- `CALLBACK` mode: `Target: Search pattern` (`TextColored` blue)

## 6. Search scope

Dropdown next to the search type:

| Value | Meaning |
|---------|--------|
| **Current region** | Searches only in the active MB instance region |
| **All regions** | Iterates over all regions of the current architecture |

## 7. Find modes

| Button / key | Action |
|---|---|
| **Find Next** / `Enter` / `F3` | Jump to next match from `cursor + 1`, no wraparound |
| **Find Prev** / `Shift+F3` | Jump to previous match |
| **Find All** | Collects results into a panel below the search (max 256 hits). Click on a result = jump to address. |

## 8. Progress + Cancel

For large regions (= 16 MB Ramdisk) the search runs **chunked** -
256 KB / frame = approx. 15 MB/s at 60 FPS = the 16 MB Ramdisk takes
roughly 1 s. The UI stays responsive (= no stutter).

During progress, a **progress bar** in % is shown in the search status
row. The **Cancel** button aborts the search (state goes to `CANCELED`).

## 9. Search history

The **H** button next to the pattern field opens a dropdown with the
last 8 patterns (most recent first). Clicking a history item sets it
into the pattern field and also restores the corresponding
`search_type`.

## 10. Persistence

Search settings per MB instance in `cfgmain`:

| Key | Meaning |
|------|--------|
| `search_type` | Active type (default `BYTES`) |
| `search_pattern` | Last pattern string |
| `search_scope` | `Current region` / `All regions` |
| `search_case_sensitive` | bool |
| `search_history` | 8 last patterns |

## 11. Examples

### Find a byte sequence

1. Search type: **Bytes (HEX + ASCII)**
2. HEX field: `C3 00 01` (= `JP 0x0100`, Z80 jump instruction)
3. **Find Next** -> jumps to the first occurrence

### Find the text `napsat` in the SAPO-P scroll

1. Encoding: **KOI8-CS** (= top toolbar Encoding dropdown)
2. Search type: **Bytes (HEX + ASCII)**
3. ASCII field: `napsat` -> HEX automatically updates to
   `6E 61 70 73 61 74`
4. **Find Next** -> cursor jumps to the occurrence in SAPO-P scroll RAM

### Case insensitive Czech search

1. Encoding: **KOI8-CS**
2. Type: **Bytes (HEX + ASCII)**, **Case sens** off
3. Pattern: `VAZENI` -> also finds `Vazeni`, `vazeni`, `VAZENI` etc.

### Insert a Kanji from the palette into the search pattern

1. Encoding: **SharpMZ JP CG1**
2. Search type: **Bytes (HEX + ASCII)**, cursor in HEX or ASCII field
3. Click **[+]** -> opens Char Inserter, automatically switches to the
   `ASCII JP` tab
4. Click on a Kanji glyph -> byte appended to the HEX field
   `search_pattern`
5. **Find Next** -> finds the occurrence in memory

## Related

- [hex-view](hex-view.md) - core hex viewer + cursor + edit
- [edit-tools](edit-tools.md) - edit features (undo, fill, annotations)
- [Memory Map](../memory-map.md), [Watch](../watch.md)
