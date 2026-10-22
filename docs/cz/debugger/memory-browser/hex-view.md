# Memory Browser - Hex view

Core hex viewer + editor s 10 encodings, navigací, edit modes a per-byte
metadaty (recently edited badge, PC/SP marker, symbol overlay, annotation
indikátor, bookmark marker).

## Toolbar

3-řádkový header okna:

### Row 1 - File ops + Edit master + Region

```
[Load BIN] [Save BIN] | [Edit: OFF/ON] | Region: ▼
```

- **Load BIN** - načte binární soubor a zapíše do regionu od cursor pos
  (disabled při Edit OFF / RO regionu)
- **Save BIN** - dialog From/Size/Whole region pro export bytů do .bin
  (disabled pokud region je prázdný / disconnected)
- **Edit: OFF/ON** - master toggle, červená BG když ON. Pro RO regiony
  (ROM, FLASH) je force-disabled. F2 dělá to samé.
- **Region ▼** - dropdown všech dostupných regionů (banking-aware,
  per-arch). Viz [layers-regions](layers-regions.md) pro detail.

### Row 2 - View options

```
Encoding ▼ | Bytes/row ▼ | ☑ ASCII | ☑ PC ☑ SP | Origin
```

- **Encoding ▼** - 10 encodings pro ASCII sloupec (viz níže)
- **Bytes/row ▼** - 8 / 16 / 32 (default 16)
- **ASCII** - toggle ASCII sloupce vpravo od hex
- **PC** / **SP** - highlight řádků obsahujících Z80 PC / SP (jen
  Logical Z80 region)
- **Origin** - toggle per-row physical origin labelu (viz
  [layers-regions](layers-regions.md))

### Row 3 - Navigation + Search

```
HEX:____ DEC:____ [Goto] [Search] < > Page: N of: M  0xXXXX
```

- **HEX:** input - hex adresa pro goto (např. `C000` nebo `c000`,
  prefix `0x` není nutný)
- **DEC:** input - decimal adresa
- **Goto** - skok na zadanou adresu (Ctrl+G fokusuje HEX input)
- **Search** - toggle search panelu (viz [search](search.md))
- **< >** - paging (page = 16 řádek × bytes_per_row), klávesy PgUp/PgDn
- **Page: N of: M  0xXXXX** - status aktuální stránky + aktuální cursor addr

## Hex tabulka

Layout per řádek (default 16 B/row):

```
| AAAA: | XX XX XX XX | XX XX XX XX | XX XX XX XX | XX XX XX XX  | ASCII... |
```

- **Address column** - adaptivní hex width (4 digity pro < 64 KB, 6
  pro < 16 MB, 8 pro větší). Adresa = offset v rámci regionu.
- **Mark column** (1 char) - znak indikátor: '\*' = bookmark, 'P' =
  Z80 PC, 'S' = Z80 SP, '!' = freeze (= viz Layers panel pro detail)
- **HEX column** - byty rozdělené po 4 přes `|` separátor pro čitelnost.
  Per-byte custom BG color z Layers panelu (CDL X/R/W, Heatmap, atd.).
- **ASCII column** - per byte UTF-8 glyph dle aktivního encoding.
  Netisknutelné = `.`. Per-byte BG + "recently edited" žluté podtržení.

Render: ImGuiListClipper virtualizace - i pro 16 MB Ramdisk plynulý
scroll. Per-row 32 B read přes backend (= chunk per draw call).

## Encodings (10)

| ID | Encoding | Popis |
|----|----------|-------|
| 0 | **Raw** | Printable ASCII 0x20-0x7E, jinak `.` |
| 1 | Sharp MZ ASCII (EU) | Sharp MZ EU charset → ASCII (1B per glyph) |
| 2 | Sharp MZ ASCII (JP) | Sharp MZ JP charset → ASCII |
| 3 | Sharp MZ UTF-8 (EU) | Sharp MZ EU → UTF-8 (vícebajtové glyphs s diakritikou) |
| 4 | Sharp MZ UTF-8 (JP) | Sharp MZ JP → UTF-8 (vč. katakana) |
| 5 | Sharp MZ EU CG1 | MZ ASCII → video kód → mzglyphs EU bank 1 (= live emu display look) |
| 6 | Sharp MZ EU CG2 | MZ ASCII → video kód → mzglyphs EU bank 2 (= graphics chars) |
| 7 | Sharp MZ JP CG1 | dtto JP bank 1 |
| 8 | Sharp MZ JP CG2 | dtto JP bank 2 (= katakana / hiragana glyphs) |
| 9 | **KOI8-CS (Czech/Slovak)** | 8-bit kódování v Sharp MZ CP/M ekosystému, plné Czech diakritika |

CG variants (5-8) renderují **skutečné MZ CG-ROM bitmapy** přes
`mzglyphs.ttf` font v PUA range U+E100-E4FF - tj. vidíš co by se
zobrazilo na MZ obrazovce v textovém režimu.

KOI8-CS (9) - používá plnou Cousine font (= obsahuje Latin Extended-A
pro č, ď, ě, ž, š, ř, ů, ň, ...). SAPO-P scroll text je v KOI8-CS.

Změna encodingu **nepřepočítává input mezi encodings** (= silent abort
aktivní edit).

## Editace HEX

V Edit mode + HEX sub-mode (default po F2):

- Cursor pozice v hex sloupci, půl-byte nibble pozice
- Klávesy **0-9** / **A-F** zapíší nibble (high → low → další byte)
- Klávesy keypad **0-9** taky
- Žluté podtržení (= "recently edited" badge) se objeví per editovaný byte
- Žádné modifikátory (Ctrl/Alt/Super = skip, kolize s shortcuts)

Write-through přes backend `write_bytes` - pro VRAM regiony se okamžitě
projeví v video output. Per-byte undo push (max 10 levels per region).

## Editace ASCII

Tab přepne na ASCII sub-mode (= label "Mode: ASCII (Tab to switch)" v
bottom bar, červený caret pod glyfem).

- Akceptuje **raw printable 7-bit ASCII 0x20..0x7E** přímo z klávesnice
  (= písmena, číslice, interpunkce, mezera)
- Non-ASCII chars (Czech diakritika, Kanji, MZ CG glyfy) **silent skip**
- Pro speciální znaky použij **Char Inserter** okno (viz
  [search](search.md))

## Klik v hex / ASCII column

- **LMB v HEX column** → cursor jump na klikled byte + edit_mode = HEX
- **LMB v ASCII column** → cursor jump + edit_mode = ASCII
- **RMB v obou** → context menu (bookmark, freeze, annotation, fill,
  undo/redo, insert character, show in Memory Map, PCG editor pro
  PCG region, Who wrote this byte? - viz
  [RAM Write Log](../ram-write-log.md))

## Klávesová navigace

| Klávesa | Akce |
|---------|------|
| **Šipky L/R** | cursor o byte vlevo / vpravo |
| **Šipky U/D** | cursor o řádek (= bytes_per_row) |
| **PgUp / PgDn** | scroll o 16 řádek + cursor přesun |
| **Home / End** | start / konec regionu |
| **Ctrl+Home/End** | dtto |
| **Ctrl+G** | focus na HEX goto input |
| **F2** | Edit toggle |
| **Tab** | HEX ↔ ASCII sub-mode (v Edit) |
| **Esc** | Edit OFF |

## PC / SP markers

Jen pro **Logical Z80 region** (= 16-bit Z80 addr má smysl jen tam).
Pokud zapnuto:
- **'P' marker** v mark column na řádku Z80 PC
- **'S' marker** na řádku Z80 SP
- Highlight celého řádku tlumeně (= nepřepiš BG layers)

Pro non-Logical regiony (RAM, ROM, VRAM, ...) jsou markers vždy OFF
(= cursor.addr je offset v regionu, ne Z80 addr).

## Cursor info v bottom bar

```
Cursor: 0x0000C005 = 0x6F 'o' (KOI8-CS)
```

- Adresa offset 8-digit (s zero-padding)
- Byte hodnota hex
- ASCII glyph dle aktivního encoding
- Encoding label

Pokud Edit ON, navíc:
```
... || Mode: HEX (Tab to switch)
```

## Recently edited badge

Žluté podtržení pod HEX byte i ASCII glyph pro byty editované v aktuální
session. Storage per instance, max 1024 entries FIFO. Auto-clear při
region switch (= nová session). Manual clear přes tlačítko **Clear
edited** v row 1.

**Auto-unmark po undo na original**: každý záznam drží `original_byte`
(= hodnota PŘED prvním edit v session). Po undo (nebo manuálním editu
na původní hodnotu) se entry odebere a podtržení zmizí.

## Symbol overlay

V ASCII column, jen Logical Z80 region + Layer Symbols ON. Pokud řádek
obsahuje sym_db symbol, jeho name se prepende jako žlutý label s
truncation. Hover tooltip s full name + kind + komentář.

## Annotation indikátor

V bottom bar pod cursor info, pokud byte pod kurzorem má annotation:

```
[■] Annotation: <text>
```

(viz [edit-tools](edit-tools.md) pro annotation detail)

V V6.1 navíc **hover tooltip** nad annotated byte (HEX i ASCII sloupec)
- ImGui tooltip s color swatch + addr + text. Implementace přes manuální
`IsMouseHoveringRect` (hex cells jsou DrawList rendery, ne ImGui items
pro IsItemHovered).

## Bookmark marker

`'*'` v mark column pokud byte má sym_db entry s `kind=BOOKMARK`. Klik
LMB stejně jako jiný hex cell. RMB context menu Add/Remove bookmark.

## Související

- [layers-regions](layers-regions.md) - Layers panel (CDL/Heatmap/Δ/Frozen/Symbols), Regions tree, Multi-instance
- [search](search.md) - Search engine + Pattern Builder + Char Inserter
- [edit-tools](edit-tools.md) - Edit features (undo/redo, fill, annotations, freeze)
- [diff-pcg](diff-pcg.md) - Memory Diff window + PCG glyph editor
