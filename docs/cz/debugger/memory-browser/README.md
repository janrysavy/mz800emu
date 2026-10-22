# Memory Browser

Memory Browser je hex viewer + editor + search engine paměti pro Sharp
MZ-800 / MZ-700 / MZ-1500 emulátor. Zobrazuje **plnou paletu paměťových
regionů** (Logical Z80, RAM, ROM, CG-ROM, VRAM plane, MZ-700 char/attr,
CG-RAM, PCG, Memext, Ramdisk), umí editovat HEX i ASCII, sledovat CDL /
heatmapu / annotations, vyhledávat byte i text patterns, fill, freeze,
porovnávat snapshoty, ediotvat PCG glyfy a vkládat speciální znaky přes
paletu (Char Inserter).

5 nezávislých instancí (Memory Browser main + #2..#5) - každá s vlastním
stavem (region, encoding, cursor, layers, ...) přetrvávajícím přes restart.

## Otevření

- **Alt+E** - globální zkratka pro main okno (toggle).
- **Menu Debugger -> Memory Browser** - main okno.
- **Menu Debugger -> Other memory browsers -> Memory Browser #N** -
  sekundární instance.
- **Iconbar tlačítko MEM** v hlavním okně debuggeru.

## Layout

```
+-- Memory Browser ###mb_main ------------------------------------+
| [Load BIN] [Save BIN] | [Edit: OFF/ON] | Region: ▼              | <- toolbar row 1
| Encoding ▼ | Bytes/row ▼ | ☑ ASCII | ☑ PC ☑ SP | Origin        | <- toolbar row 2
| HEX:____ DEC:____ [Goto] [Search] < > Page: N of: M  0xXXXX     | <- toolbar row 3
+----+--+----------------------------------+----------------------+
| Re |  | (hex tabulka - ScrollY clipper)  | Layers              | <- 3-panel layout
| gi |  | C000: C3 11 00 C9 ...   ...      |  ☐ CDL Code (X)     |
| on |  | C010: 21 50 12 11 ...   !P...    |  ☐ CDL Data Read    |
| s  |  | ...                              |  ☐ CDL Data Write   |
| tr |  |                                  |  ☐ Heatmap          |
| ee |  |                                  |  ☐ Snapshot Δ       |
+----+--+----------------------------------+----------------------+
| Size: 65536 B | R/W | mapped | Z80 base 0x0000 ||  Cursor: ... | <- bottom bar
| DMD: MZ-800 mode (0x02)              ||  Fill: done (= status) |
+-----------------------------------------------------------------+
```

3-panel layout - **Regions tree** vlevo (kolaps), **Hex view** uprostřed
(vždy), **Layers panel** vpravo (kolaps). Splittery resize, panel widths
persisted.

## Feature matrix

| Fáze | Funkce | Detail | Sub-stránka |
|------|--------|--------|-------------|
| V0 | Hex view core | 16/32 B/row, ListClipper, 10 encodingů, edit HEX+ASCII, Goto, PC/SP marker | [hex-view](hex-view.md) |
| V0 | Region paleta | 21 region kindů, banking-aware, per-arch | [layers-regions](layers-regions.md) |
| V1 | Layers panel | CDL X/R/W, Heatmap, Snapshot Δ, Frozen, Symbols overlay | [layers-regions](layers-regions.md) |
| V1 | Bookmarks | sym_db kind=BOOKMARK, marker v hex view | [layers-regions](layers-regions.md) |
| V1 | Freeze bytes | O(1) hot path "lock byte" | [edit-tools](edit-tools.md) |
| V2 | Regions tree | Per-arch hierarchický strom + cross-window "Show in Map" | [layers-regions](layers-regions.md) |
| V3 | Multi-instance | 5 oken (main + #2..#5) s per-instance persist | [layers-regions](layers-regions.md) |
| V4 | Search engine | 7 typů: bytes/ASCII/word LE/BE/masked/regex/expression | [search](search.md) |
| V4.1+ | Pattern Builder | Bytes type s dual HEX+ASCII synced fields + Char Inserter | [search](search.md) |
| V4.1+ | Encoding-aware fold | KOI8-CS/SharpMZ Czech case insensitive search | [search](search.md) |
| V5 | Memory Diff | Samostatné okno side-by-side hex porovnání | [diff-pcg](diff-pcg.md) |
| V6 | Pattern fill | 6 modes + undo/redo stack 10 levels | [edit-tools](edit-tools.md) |
| V6 | Annotations | Text + color tag, hover tooltip, persist | [edit-tools](edit-tools.md) |
| V6 | PCG glyph editor | 8×8 bitmap editor pro MZ-1500 PCG banks | [diff-pcg](diff-pcg.md) |
| V1.5+ | ASCII edit dispatch | Raw 7-bit + Char Inserter pro speciální chars | [hex-view](hex-view.md) |
| V1.5+ | Char Inserter | 3-tab paleta (SharpMZ EU/JP, KOI8-CS), 16×16 grid | [search](search.md) |
| V6.1 | Annotation tooltip | Hover myší nad annotated byte → tooltip | [edit-tools](edit-tools.md) |

## Klávesy

| Klávesa | Akce |
|---------|------|
| **Alt+E** | Toggle main Memory Browser okno |
| **F2** | Edit toggle (per instance) |
| **Tab** | HEX ↔ ASCII edit mode (uvnitř Edit) |
| **Esc** | Edit OFF / zavři Search panel |
| **Ctrl+F** | Search panel open |
| **F3** / **Shift+F3** | Find Next / Find Prev |
| **Ctrl+G** | Goto address focus |
| **Ctrl+Z** / **Ctrl+Y** | Undo / Redo (per region) |
| **Šipky** | Posun cursoru po bytech / řádku |
| **PgUp** / **PgDn** | Posun o 16 řádků |
| **Home** / **End** | Start / konec regionu |
| **Ctrl+Home** / **Ctrl+End** | Stejně jako Home/End |
| **Alt+Shift+I** | Toggle Char Inserter okno |

## Sub-stránky

Detailní popis funkčností v dedikovaných sub-stránkách:

- **[hex-view.md](hex-view.md)** - Core hex viewer, encoding,
  editace HEX i ASCII, navigace, Goto, edited badge
- **[layers-regions.md](layers-regions.md)** - Layers panel
  (CDL/Heatmap/Snapshot/Frozen/Symbols), Regions tree per-arch, banking
  status, Bookmarks, Multi-instance
- **[search.md](search.md)** - Search engine s 7 typy,
  Pattern Builder (Bytes HEX+ASCII), encoding-aware case fold, Char
  Inserter paleta
- **[edit-tools.md](edit-tools.md)** - Edit tools: undo/redo
  stack, Pattern fill (6 modes), Annotations s color tag + hover tooltip,
  Freeze bytes
- **[diff-pcg.md](diff-pcg.md)** - Memory Diff window
  (side-by-side snapshot porovnání), PCG glyph editor (MZ-1500)

## Persistence

Stav se ukládá v cfgmain sekcích per instance:
- `[MEMBROWSER_WINDOW_MAIN]` - main okno (instance 0)
- `[MEMBROWSER_WINDOW_2]` až `[MEMBROWSER_WINDOW_5]` - sekundární okna

Klíče per instance:
- `encoding` - aktivní encoding (default Sharp MZ ASCII EU)
- `region_kind` + `region_sub_id` - poslední vybraný region (stable
  key, ne raw region_id)
- `cursor_addr` - poslední pozice cursoru
- `bytes_per_row` - 8 / 16 / 32 (default 16)
- `ascii_column_visible`, `show_pc_marker`, `show_sp_marker` - bool flagy
- `layer_*` - 6 layer toggles
- `regions_panel_open`, `regions_panel_width` - sidebar stav
- `layers_panel_open`, `layers_panel_width`
- `search_type`, `search_pattern`, `search_scope`, `search_case_sensitive`
- `search_history` (8 last patterns)
- `show_origin_labels` - per-row physical origin v Logical view

Annotations + Freeze bytes mají vlastní persist souborové:
- `membrowser_annotations.txt` v pracovním adresáři
- Freeze bytes v cfgmain `[MB_FREEZE]`

## CLI flag

- `--memory-browser` - vynucené otevření main okna při startu
  (= bypass persisted show flag z imgui.ini)

## Související panely

- [Memory Map](memory-map.md) - banking + memext per 4 KB stránku
- [Memory Heatmap](memory-heatmap.md) - heatmap přístupů (= obecnější
  než MB Layers heatmap)
- [Watch](watch.md) - user-defined paměťové hlídky
- [Bookmarks](bookmarks.md) - bookmark editor (= shared store)
- [Symbols](symbols.md) - symbol browser (= shared sym_db)
- [RAM Write Log](../ram-write-log.md) - kdo zapsal bajt (i "Who wrote this byte?" v kontextovém menu)
- [Memory Search](../memory-search.md) - hledání proměnné (životy, skóre, časovač) zužováním kandidátů
