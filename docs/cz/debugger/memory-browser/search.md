# Memory Browser - Search engine + Pattern Builder + Char Inserter

Search engine v Memory Browseru umožňuje hledat sekvence bajtů, texty,
slovní hodnoty, regulární výrazy nebo výsledky výrazů napříč jedním
regionem nebo všemi regiony aktuální architektury. Pro pohodlné
sestavení hledaného vzoru slouží **Pattern Builder** (dual editable
HEX + ASCII pole) a **Char Inserter** (paleta speciálních znaků).

Hledání je jednorázové. Proměnnou s neznámou hodnotou (životy, skóre,
časovač) podle toho, jak se mění, najde [Memory Search](../memory-search.md).

## 1. Otevření search panelu

| Cesta | Co se stane |
|---|---|
| **Klávesa** `Ctrl+F` | Toggle search panelu (= rozšíření hlavního okna o search řádky) |
| **Tlačítko** `Search` ve 3. toolbar řádku | Otevře panel |
| **Klávesa** `Esc` | Zavře panel |

Search panel je součástí okna Memory Browseru, nezabírá samostatné
okno. Po zavření se nastavení zachová v cfgmain (= per instance).

## 2. Search types

V dropdownu **Search type** je 7 + 1 varianta. Doporučeným typem pro
většinu použití je první (`Bytes (HEX + ASCII)`), ostatní jsou buď
specializované, nebo zachované pro zpětnou kompatibilitu.

| # | Typ | Popis |
|---|-----|-------|
| 1 | **Bytes (HEX + ASCII)** | V4.1+ Pattern Builder, dva synchronizované editovatelné fieldy (HEX + ASCII). Doporučený výchozí typ. |
| 2 | **Byte sequence (hex, legacy)** | V0 - jedno hex pole `AA BB CC`. Zachováno pro kompatibilitu. |
| 3 | **ASCII (current encoding, legacy)** | V0 - jedno textové pole, kompilace přes aktuální encoding. |
| 4 | **Word LE (16-bit)** | Literál typu `0x1234` se zakóduje jako little-endian bajty `34 12`. |
| 5 | **Word BE (16-bit)** | Literál `0x1234` se zakóduje jako big-endian bajty `12 34`. |
| 6 | **Masked (AA ?? BB)** | Hex sekvence s `??` jako wildcard pro libovolný bajt. |
| 7 | **Regex (POSIX ERE)** | `std::regex_extended` nad raw bajty regionu. |
| 8 | **Expression (per byte)** | Pro každý bajt se vyhodnotí výraz `bp_expr` (`Value = bajt`, `Address = offset`). Match nastane když je výsledek nenulový. |

## 3. Pattern Builder (Bytes type)

Nejmodernější UX (V4.1+). Dvě synchronizovaná editovatelná pole:

```
Search type: [ Bytes (HEX + ASCII) v ]
HEX:    [56 C1 DA 65 6E C9    ]  ASCII: [Vážení    ]  [+]  [H]
Scope: [Current region v]  ☐ Case sens.  [Find Next] [Prev] [All] [Cancel] [Close]
```

- **HEX field** (vlevo): free-form hex `56 C1 DA 65 6E C9`. Mezery
  i čárky jsou volitelné. Tooltip: `HEX bytes: AA BB CC (spaces optional)`.
- **ASCII field** (vpravo): vykreslený z byte bufferu přes aktuální
  encoding (např. KOI8-CS -> `Vážení`, Raw -> tečky pro non-ASCII).
  Pole je editovatelné - edit walká UTF-8 codepointy, per znak se
  zavolá `utf8_to_byte(current_encoding)` a výsledný bajt přepíše
  byte buffer. Tooltip: `ASCII view (current encoding). Type to
  update HEX. Use [+] for special chars.`
- **[+] Insert character** otevře okno Char Inserter v CALLBACK
  módu (= klik na cell v paletě appende bajt do pattern bufferu,
  ne do paměti).
- **[H] History** otevře dropdown s posledními 8 patterny.

### Synchronizace HEX <-> ASCII

- Edit v HEX poli -> ASCII scratch buffer se regeneruje per-frame
  (= pouze pokud ASCII pole **není** focused).
- Edit v ASCII poli -> re-parse přes encoding, update byte bufferu
  i HEX fieldu.
- Focus se detekuje přes ImGui `GetActiveID()`, aby HEX-driven
  regenerace nepřebíjela rozepsaný ASCII edit.

Kanonické úložiště patternu = hex string v `search_pattern[]`. Jen
ten se persistuje v cfgmain.

## 4. Encoding-aware case fold

Checkbox **Case sens** aktivuje porovnávání s ohledem na velikost
písmen. Když je vypnutý, search používá encoding-aware tolower
tabulku - funguje pro **všechny encodingy**, kde existuje case pair:

| Encoding | Pokrytí case foldu |
|----------|---------------------|
| **Raw** | Latin `A-Z` (0x41-0x5A -> 0x61-0x7A) |
| **SharpMZ EU** | Latin + EU national chars (podle SharpMZ EU charset tabulky) |
| **SharpMZ JP** | Latin + JP national chars |
| **KOI8-CS** | Latin + plná česká/slovenská diakritika (Á->á 0xE1->0xC1, Ž->ž 0xFA->0xDA, Č->č, Ř->ř, Š->š, Ů->ů, Ť->ť atd.) |
| **CG variants** (CG1/CG2/UTF8) | Aliasované na canonical `_ASCII` counterpart (identický byte layout, jen jiný render) |

Implementace: per-encoding lazy-init 256B tabulka. Algoritmus per
bajt = `byte -> utf8 -> codepoint -> g_unichar_tolower -> utf8 -> byte`
přes GLib.

**Příklad case foldu**: SAPO-P scroll text obsahuje `Vážení`
v KOI8-CS jako bajty `56 C1 DA 65 6E C9`. Pattern `VÁŽENÍ` (vše
velké) v encoding KOI8-CS s vypnutým **Case sens** najde i lowercase
`Vážení` (KOI8-CS uppercase bajty `0xE1/0xFA/0xE9` se foldují na
lowercase `0xC1/0xDA/0xC9`).

## 5. Char Inserter okno

Singleton okno s paletou speciálních znaků (V1.5+). Tři taby:

| Tab | Encoding | Glyfy |
|-----|----------|-------|
| **ASCII EU** | SharpMZ EU CG1 | MZ CG-ROM bitmapy z `mzglyphs.ttf`, PUA `U+E100-E1FF` |
| **ASCII JP** | SharpMZ JP CG1 | MZ CG-ROM bitmapy z `mzglyphs.ttf`, PUA `U+E300-E3FF` |
| **KOI8-CS** | KOI8-CS | Standardní UTF-8 přes výchozí font (česká diakritika) |

Layout v každém tabu: 16x16 grid (= 256 bajtů `0x00..0xFF`), per
buňka glyph + tooltip `0xNN - <utf8 glyph>`.

### Otevírání a targety

| Cesta | Target | Co dělá klik na buňku |
|---|---|---|
| **Context menu hex view** -> `Insert character...` | `MB_CURSOR` mode - cursor pos zdrojové MB instance | Zapíše bajt do paměti, posune cursor, push undo |
| **Search panel** -> `[+]` vedle ASCII fieldu | `CALLBACK` mode - search pattern buffer | Appende bajt do `search_pattern` hex stringu |
| **Menu Debugger -> Char Inserter** | Default MB instance (#0 main) | Stejné jako context menu (`MB_CURSOR` mode) |

Status řádek nahoře okna ukazuje aktuální target:

- `MB_CURSOR` mode: `Target: MB main @ 0xC100  Region: ...  [Edit ON]`
- `CALLBACK` mode: `Target: Search pattern` (`TextColored` modře)

## 6. Search scope

Dropdown vedle search type:

| Hodnota | Význam |
|---------|--------|
| **Current region** | Hledá jen v aktivním regionu MB instance |
| **All regions** | Iteruje všechny regiony aktuální architektury |

## 7. Find módy

| Tlačítko / klávesa | Akce |
|---|---|
| **Find Next** / `Enter` / `F3` | Skok na další match od `cursor + 1`, bez wraparoundu |
| **Find Prev** / `Shift+F3` | Skok na předchozí match |
| **Find All** | Sebere výsledky do panelu pod search (max 256 hitů). Klik na výsledek = jump na adresu. |

## 8. Progress + Cancel

Pro velké regiony (= 16 MB Ramdisk) běží search **chunked** -
256 KB / frame = cca 15 MB/s při 60 FPS = 16 MB Ramdisk projde
za zhruba 1 s. UI zůstává responsivní (= žádný stutter).

V průběhu se v search status řádku zobrazuje **progress bar** v %.
Tlačítko **Cancel** přeruší hledání (stav přejde do `CANCELED`).

## 9. Search history

Tlačítko **H** vedle pattern fieldu otevře dropdown s 8 posledními
patterny (most recent first). Klik na historickou položku ji nastaví
do pattern fieldu a zároveň obnoví odpovídající `search_type`.

## 10. Persistence

Search nastavení per MB instance v `cfgmain`:

| Klíč | Význam |
|------|--------|
| `search_type` | Aktivní typ (default `BYTES`) |
| `search_pattern` | Poslední pattern string |
| `search_scope` | `Current region` / `All regions` |
| `search_case_sensitive` | bool |
| `search_history` | 8 posledních patternů |

## 11. Příklady

### Najít sekvenci bajtů

1. Search type: **Bytes (HEX + ASCII)**
2. HEX field: `C3 00 01` (= `JP 0x0100`, Z80 jump instruction)
3. **Find Next** -> skok na první výskyt

### Najít text `napsat` ve SAPO-P scroll

1. Encoding: **KOI8-CS** (= top toolbar Encoding dropdown)
2. Search type: **Bytes (HEX + ASCII)**
3. ASCII field: `napsat` -> HEX se automaticky aktualizuje na
   `6E 61 70 73 61 74`
4. **Find Next** -> kurzor skočí na výskyt v SAPO-P scroll RAM

### Case insensitive český search

1. Encoding: **KOI8-CS**
2. Type: **Bytes (HEX + ASCII)**, **Case sens** vypnutý
3. Pattern: `VÁŽENÍ` -> najde i `Vážení`, `vážení`, `VÁŽENÍ` atd.

### Vložit Kanji z palety do search patternu

1. Encoding: **SharpMZ JP CG1**
2. Search type: **Bytes (HEX + ASCII)**, kurzor v HEX nebo ASCII fieldu
3. Klik **[+]** -> otevře Char Inserter, automaticky přepne na tab
   `ASCII JP`
4. Klik na Kanji glyph -> bajt se appende do HEX fieldu
   `search_pattern`
5. **Find Next** -> najde výskyt v paměti

## Související

- [hex-view](hex-view.md) - core hex viewer + cursor + edit
- [edit-tools](edit-tools.md) - edit features (undo, fill, anotace)
- [Memory Map](../memory-map.md), [Watch](../watch.md)
