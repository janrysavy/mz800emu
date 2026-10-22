# Memory Browser

Memory Browser is a hex viewer + editor + memory search engine for the
Sharp MZ-800 / MZ-700 / MZ-1500 emulator. It exposes the **full palette
of memory regions** (Logical Z80, RAM, ROM, CG-ROM, VRAM planes, MZ-700
char/attr, CG-RAM, PCG, Memext, Ramdisk), allows editing in HEX and
ASCII, tracks CDL / heatmap / annotations, searches for byte and text
patterns, performs fill, freeze, compares snapshots, edits PCG glyphs
and inserts special characters from a palette (Char Inserter).

5 independent instances (Memory Browser main + #2..#5) - each with its
own state (region, encoding, cursor, layers, ...) persisting across
restart.

## Opening

- **Alt+E** - global shortcut for the main window (toggle).
- **Menu Debugger -> Memory Browser** - main window.
- **Menu Debugger -> Other memory browsers -> Memory Browser #N** -
  secondary instance.
- **Iconbar MEM button** in the main debugger window.

## Layout

```
+-- Memory Browser ###mb_main ------------------------------------+
| [Load BIN] [Save BIN] | [Edit: OFF/ON] | Region: v              | <- toolbar row 1
| Encoding v | Bytes/row v | [x] ASCII | [x] PC [x] SP | Origin   | <- toolbar row 2
| HEX:____ DEC:____ [Goto] [Search] < > Page: N of: M  0xXXXX     | <- toolbar row 3
+----+--+----------------------------------+----------------------+
| Re |  | (hex table - ScrollY clipper)    | Layers              | <- 3-panel layout
| gi |  | C000: C3 11 00 C9 ...   ...      |  [ ] CDL Code (X)   |
| on |  | C010: 21 50 12 11 ...   !P...    |  [ ] CDL Data Read  |
| s  |  | ...                              |  [ ] CDL Data Write |
| tr |  |                                  |  [ ] Heatmap        |
| ee |  |                                  |  [ ] Snapshot Delta |
+----+--+----------------------------------+----------------------+
| Size: 65536 B | R/W | mapped | Z80 base 0x0000 ||  Cursor: ... | <- bottom bar
| DMD: MZ-800 mode (0x02)              ||  Fill: done (= status) |
+-----------------------------------------------------------------+
```

3-panel layout - **Regions tree** on the left (collapsible), **Hex view**
in the middle (always), **Layers panel** on the right (collapsible).
Splitters resize, panel widths persisted.

## Feature matrix

| Phase | Feature | Detail | Sub-page |
|------|--------|--------|-------------|
| V0 | Hex view core | 16/32 B/row, ListClipper, 10 encodings, edit HEX+ASCII, Goto, PC/SP marker | [hex-view](hex-view.md) |
| V0 | Region palette | 21 region kinds, banking-aware, per-arch | [layers-regions](layers-regions.md) |
| V1 | Layers panel | CDL X/R/W, Heatmap, Snapshot Delta, Frozen, Symbols overlay | [layers-regions](layers-regions.md) |
| V1 | Bookmarks | sym_db kind=BOOKMARK, marker in hex view | [layers-regions](layers-regions.md) |
| V1 | Freeze bytes | O(1) hot path "lock byte" | [edit-tools](edit-tools.md) |
| V2 | Regions tree | Per-arch hierarchical tree + cross-window "Show in Map" | [layers-regions](layers-regions.md) |
| V3 | Multi-instance | 5 windows (main + #2..#5) with per-instance persist | [layers-regions](layers-regions.md) |
| V4 | Search engine | 7 types: bytes/ASCII/word LE/BE/masked/regex/expression | [search](search.md) |
| V4.1+ | Pattern Builder | Bytes type with dual HEX+ASCII synced fields + Char Inserter | [search](search.md) |
| V4.1+ | Encoding-aware fold | KOI8-CS/SharpMZ Czech case insensitive search | [search](search.md) |
| V5 | Memory Diff | Standalone side-by-side hex comparison window | [diff-pcg](diff-pcg.md) |
| V6 | Pattern fill | 6 modes + undo/redo stack 10 levels | [edit-tools](edit-tools.md) |
| V6 | Annotations | Text + color tag, hover tooltip, persist | [edit-tools](edit-tools.md) |
| V6 | PCG glyph editor | 8x8 bitmap editor for MZ-1500 PCG banks | [diff-pcg](diff-pcg.md) |
| V1.5+ | ASCII edit dispatch | Raw 7-bit + Char Inserter for special chars | [hex-view](hex-view.md) |
| V1.5+ | Char Inserter | 3-tab palette (SharpMZ EU/JP, KOI8-CS), 16x16 grid | [search](search.md) |
| V6.1 | Annotation tooltip | Mouse hover over annotated byte -> tooltip | [edit-tools](edit-tools.md) |

## Hotkeys

| Key | Action |
|---------|------|
| **Alt+E** | Toggle main Memory Browser window |
| **F2** | Edit toggle (per instance) |
| **Tab** | HEX <-> ASCII edit mode (inside Edit) |
| **Esc** | Edit OFF / close Search panel |
| **Ctrl+F** | Open Search panel |
| **F3** / **Shift+F3** | Find Next / Find Prev |
| **Ctrl+G** | Focus Goto address |
| **Ctrl+Z** / **Ctrl+Y** | Undo / Redo (per region) |
| **Arrows** | Move cursor by bytes / row |
| **PgUp** / **PgDn** | Move by 16 rows |
| **Home** / **End** | Start / end of region |
| **Ctrl+Home** / **Ctrl+End** | Same as Home/End |
| **Alt+Shift+I** | Toggle Char Inserter window |

## Sub-pages

Detailed description of features in dedicated sub-pages:

- **[hex-view.md](hex-view.md)** - Core hex viewer,
  encoding, HEX and ASCII editing, navigation, Goto, edited badge
- **[layers-regions.md](layers-regions.md)** - Layers
  panel (CDL/Heatmap/Snapshot/Frozen/Symbols), Regions tree per-arch,
  banking status, Bookmarks, Multi-instance
- **[search.md](search.md)** - Search engine with 7
  types, Pattern Builder (Bytes HEX+ASCII), encoding-aware case fold,
  Char Inserter palette
- **[edit-tools.md](edit-tools.md)** - Edit tools:
  undo/redo stack, Pattern fill (6 modes), Annotations with color tag
  + hover tooltip, Freeze bytes
- **[diff-pcg.md](diff-pcg.md)** - Memory Diff window
  (side-by-side snapshot comparison), PCG glyph editor (MZ-1500)

## Persistence

State is stored in cfgmain sections per instance:
- `[MEMBROWSER_WINDOW_MAIN]` - main window (instance 0)
- `[MEMBROWSER_WINDOW_2]` to `[MEMBROWSER_WINDOW_5]` - secondary windows

Per-instance keys:
- `encoding` - active encoding (default Sharp MZ ASCII EU)
- `region_kind` + `region_sub_id` - last selected region (stable key,
  not raw region_id)
- `cursor_addr` - last cursor position
- `bytes_per_row` - 8 / 16 / 32 (default 16)
- `ascii_column_visible`, `show_pc_marker`, `show_sp_marker` - bool flags
- `layer_*` - 6 layer toggles
- `regions_panel_open`, `regions_panel_width` - sidebar state
- `layers_panel_open`, `layers_panel_width`
- `search_type`, `search_pattern`, `search_scope`, `search_case_sensitive`
- `search_history` (8 last patterns)
- `show_origin_labels` - per-row physical origin in Logical view

Annotations + Freeze bytes have their own file persistence:
- `membrowser_annotations.txt` in the working directory
- Freeze bytes in cfgmain `[MB_FREEZE]`

## CLI flag

- `--memory-browser` - force open the main window at startup
  (= bypass the persisted show flag from imgui.ini)

## Related panels

- [Memory Map](memory-map.md) - banking + memext per 4 KB page
- [Memory Heatmap](memory-heatmap.md) - access heatmap (= more general
  than MB Layers heatmap)
- [Watch](watch.md) - user-defined memory watches
- [Bookmarks](bookmarks.md) - bookmark editor (= shared store)
- [Symbols](symbols.md) - symbol browser (= shared sym_db)
- [RAM Write Log](../ram-write-log.md) - who wrote a byte (also "Who wrote this byte?" in the context menu)
- [Memory Search](../memory-search.md) - find a variable (lives, score, timer) by narrowing candidates
