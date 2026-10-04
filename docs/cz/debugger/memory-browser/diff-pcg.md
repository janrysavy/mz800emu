# Memory Browser - Memory Diff + PCG glyph editor

Dvě samostatná okna nad Memory Browser jádrem. **Memory Diff** porovnává
dva paměťové snapshoty side-by-side a zvýrazňuje rozdíly. **PCG glyph
editor** kreslí 8x8 bitmapové znaky do PCG RAM na MZ-1500.

Obě okna jsou nezávislá na hlavním Memory Browser oknu (singleton), ale
sdílí s ním definici regionů a layerů (viz [layers-regions](layers-regions.md)).


## 1. Memory Diff (V5)

Okno pro porovnání dvou snapshotů stejného regionu. Zvýrazní byty, které
se mezi A a B liší, a ukáže souhrnnou statistiku.

### 1.1 Otevření

| Cesta | Akce |
|---|---|
| **Menu** Debugger -> Memory Diff | Otevře okno (singleton, druhé otevření jen zaměří) |

V5 nemá vyhrazenou klávesovou zkratku - okno se otevírá výhradně přes menu.

### 1.2 Layout

Okno má v horní liště dva source dropdowny (A a B), výběr regionu, scope
volbu a checkbox **Auto-snapshot at pause**, dále tlačítka **Take
snapshot** a **Export TXT...**. Pod tím dva side-by-side hex view (A
vlevo, B vpravo) rolující synchronně. Byty, které se mezi A a B liší,
jsou zvýrazněny magentou. Spodní řádek nese souhrnnou statistiku.

### 1.3 Source A / Source B

Každý ze dvou source dropdownů má tři varianty:

| Volba | Význam |
|---|---|
| **Live** | Aktuální obsah paměti emulátoru (čte se přes regions API při každém refreshi) |
| **Manual snap A** / **Manual snap B** | Pojmenovaný snapshot uložený přes tlačítko **Take snapshot** |
| **Auto-snapshot at pause** | Snapshot automaticky pořízený v okamžiku pauzy (manual stop, hit breakpointu, step) |

Typické kombinace:

- A = **Live**, B = **Manual snap A** - "co se změnilo od posledního manual snapshotu"
- A = **Manual snap A**, B = **Manual snap B** - porovnání dvou různých momentů v čase
- A = **Live**, B = **Auto-snapshot at pause** - "co se změnilo od poslední pauzy"

### 1.4 Region a Scope

- **Region** - dropdown přes všechny registrované regiony (Logical Z80,
  Physical RAM, VRAM, PCG bank, atd.). Snapshot drží bytové pole pro
  zvolený region; změna regionu invaliduje manual snapshoty (nová
  velikost = nový buffer).
- **Scope** - `whole region` porovná celý region; `range` umožní zadat
  start/end adresu (užitečné pro velké regiony, kde zajímá jen výřez).

### 1.5 Take snapshot

Tlačítko **Take snapshot** vezme aktuální Live obsah zvoleného regionu
a uloží jej jako **Manual snap A** nebo **Manual snap B** podle toho,
který slot je aktivní v dropdownech. Předchozí obsah daného slotu se
přepíše bez potvrzení.

### 1.6 Auto-snapshot at pause

Checkbox v toolbaru. Pokud je zapnutý, **každá** pauza emulátoru
(manuální stop, hit breakpointu, single step) automaticky pořídí
snapshot "before pause" pro aktuální region. Workflow:

1. Zapni **Auto-snapshot at pause**.
2. V emu zmáčkni Resume, vykonej akci ve hře (např. seber předmět).
3. Stopni emu (manual nebo BP hit).
4. V Memory Diff vidíš v B "stav před pauzou", v A "stav po pauze" -
   magenta zvýrazní co se akcí změnilo.

### 1.7 Export TXT

Tlačítko **Export TXT...** otevře IGFD dialog na uložení textového
reportu. Formát:

```
Memory Diff: Logical Z80, A=Live B=Snap @ 2026-05-25 14:32:10
Changed: 12 of 65536 B (0.02%)

0xC010: 50 -> AA
0xC012: 13 -> 99
0xC014: 00 -> 01
...
```

Vhodné jako příloha k bug reportu nebo pro audit log.

### 1.8 Statistika

Spodní řádek okna:

- **Changed: X of Y B (Z%)** - počet odlišných bytů ze scope. Při Z = 0
  se zobrazí **All sources match**.

### 1.9 Use cases

| Scénář | Postup |
|---|---|
| Najít, kde hra drží stav (HP, skóre, pozici) | Snapshot, vykonat akci ve hře, diff -> magenta byty jsou kandidáti |
| Ověřit, že cheat / freeze drží | Snapshot HP byte, nechat hráče utrpět damage, diff -> hodnota stejná = freeze funguje |
| Porovnat dva save stavy | Load `.mzs` A, snapshot, load `.mzs` B, A = Live, B = snap -> rozdíly mezi save body |


## 2. PCG glyph editor (V6, jen MZ-1500)

Okno pro editaci 8x8 bitmapových znaků uložených v **PCG**
(Programmable Character Generator) RAM. MZ-1500 specifický rys -
3 banky x 1024 znaků x 8 B na znak.

### 2.1 Otevření

| Cesta | Akce |
|---|---|
| **Memory Browser** -> region PCG bank 1/2/3 -> kurzor na byte -> pravé tlačítko myši -> **Open in PCG editor...** | Otevře editor a zaměří na znak obsahující kurzor (= addr / 8) |
| **Menu** Debugger -> PCG Editor | Otevře editor na poslední vybraný znak |

### 2.2 Layout

Horní lišta nese **Bank** dropdown (1/2/3), hex input **Char** pro
index znaku, navigační tlačítka **[<] [>]** a indikaci znaku (index +
ASCII reprezentace). Pod tím je 8x8 grid klikatelných cells (= jeden
glyph). Pod gridem řádek **HEX** ukazuje aktuálních 8 bytů glyfu a
panel tlačítek **Inverse / Mirror H / Mirror V / Rotate 90 deg**.
Spodní lišta má **Save / Reload / Close**.

### 2.3 Bank selektor

Dropdown **Bank** přepíná mezi třemi PCG bankami MZ-1500 (sub_id 0/1/2).
Každá banka je 8 KB = 1024 znaků x 8 B (jeden znak = jeden řádek 8 bitů
per byte, celkem 8 bytů na glyph).

### 2.4 Navigace po znacích

- **Char #** - číselný vstup pro přímé zadání indexu (0-1023).
- **[<] [>]** - prev/next znak (jump po 8 B v PCG bank). Na znaku 0
  a 1023 se zastaví (bez wraparoundu).
- Pod gridem editor ukazuje řádek **Raw:** s 8 bajty aktuálního znaku
  v hex a řádek **Bank addr range:** s rozsahem bajtů znaku v bance
  (`char_idx * 8` .. `char_idx * 8 + 7`).

### 2.5 Kreslení

- **Levý klik** na cell v 8x8 gridu - toggle bitu (0/1, tj.
  bg/fg pixel).
- **HEX** řádek pod gridem ukazuje aktuální 8 bytů glyfu (= bytewise
  reprezentace, jeden byte = jeden řádek bitmapy).

### 2.6 Operace nad celým glyfem

| Tlačítko | Efekt |
|---|---|
| **Inverse** | Flip všech 64 bitů (= negativ glyfu) |
| **Mirror H** | Horizontální zrcadlení (= reverse bitů uvnitř každého ze 8 bytů) |
| **Mirror V** | Vertikální zrcadlení (= reverse pořadí 8 bytů) |
| **Rotate 90 deg** | Rotace o 90 stupňů (= transpose matice + mirror) |

Operace pracují nad rozpracovanou (uncommitted) kopií v editoru, ne
přímo nad PCG RAM. Do paměti se zápis projeví až přes **Save**.

### 2.7 Save / Reload / Close

- **Save** - zapíše 8 B do PCG bank na adresu `char_idx * 8`. Změna se
  okamžitě projeví ve videovýstupu emulátoru (pokud je glyph zobrazen).
- **Reload** - zahodí uncommitted úpravy a načte aktuální obsah z PCG
  RAM zpět do editoru.
- **Close** - zavře okno. Pokud existují neuložené změny, editor
  vyzve k potvrzení (= nevyhozené úpravy se ztratí).

### 2.8 Per-architektura dostupnost

| MZARCH | Chování |
|---|---|
| **MZ-1500** | Plně funkční (PCG je hlavní rys video subsystému MZ-1500) |
| **MZ-800** | Okno lze otevřít, ale zobrazí hlášení "PCG not available on this arch" |
| **MZ-700** | Stejně jako MZ-800 - žádná PCG hardwarová podpora |

### 2.9 Use case: custom font glyph

1. Otevři Memory Browser, přepni na region **PCG bank 1**.
2. Najdi volný slot (např. char 0x90, kde je v ROM mezera nebo nepoužitý
   znak).
3. Pravé tlačítko myši -> **Open in PCG editor**.
4. V editoru klikej na cells - vykresli požadovaný glyph.
5. **Save** zapíše 8 B do PCG RAM.
6. Hra nebo aplikace, která vypíše znak s kódem 0x90 přes VRAM, nyní
   uvidí nový glyph.


## 3. Související

- [memory-browser](README.md) - hlavní okno Memory Browser
- [layers-regions](layers-regions.md) - definice regionů a layerů
  (nutné pro výběr regionu v Diff i navigaci v PCG)
- [search](search.md) - hledání bytových vzorů (užitečné společně
  s Diff pro lokalizaci state proměnných)
