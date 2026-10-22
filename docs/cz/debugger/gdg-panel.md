# GDG State - panel video subsystému

Samostatné ladicí okno, které zobrazuje aktuální vnitřní stav obvodu
GDG (Sharp custom video LSI). GDG je generátor video signálů, řídí
raster timing, paletu, border, hardwarový scroll a v případě MZ-1500
i PCG (Programmable Character Generator).

Panel je výhradně **pozorovací** - jen čte a zobrazuje, nikdy nemění
běh emulace. Můžete ho nechat otevřený trvale.

| Panel | Klávesa | Dostupný na |
|-------|---------|-------------|
| GDG State | Alt+Shift+V | MZ-700, MZ-800, MZ-1500 |

Obsah okna se liší podle architektury - každá platforma má vlastní
sadu specifik. Společná je sekce **Raster** dole, ta vypadá stejně
na všech třech.


## Aktivace a perzistence

Okno otevřete jednou z těchto cest:

- Menu **Debugger -> GDG State**.
- Klávesovou zkratkou **Alt+Shift+V** (V = Video).
- Z DBG Workplace - v submenu **Workplace -> GDG** zapnete, aby se
  panel otevíral současně s hlavním debugger oknem. Default vypnuto.

Pozice, velikost okna i stav rozbalených sekcí se pamatují mezi
spuštěními emulátoru. Snímání stavu probíhá pouze pokud je okno
otevřené - zavřené okno emulátor nezatěžuje.


## Společné UI prvky

- Sekce uvnitř panelu jsou v rozbalovacích **collapsing headerech** -
  zhroucený stav se pamatuje.
- Hodnoty se zobrazují v decimální i hex formě podle vhodnosti
  (registry hex, počítadla decimálně).
- Klik na barevný čtvereček v paletě otevře popup s indexem a RGB
  hodnotou.
- Klepnutím nemodifikujete žádnou hodnotu - okno je read-only.


## Sekce na MZ-800

### Video mode

Aktuální grafický mód odvozený z registru DMD (bity D2-D0) a flagu
MZ-700 compat (bit D3).

| Hodnota | Význam |
|---------|--------|
| 320x200 4-color VBANK A | Grafický mód 320x200, 4 barvy, VRAM bank A |
| 320x200 4-color VBANK B | Grafický mód 320x200, 4 barvy, VRAM bank B |
| 320x200 16-color | 16-color graphics mode (4 plane VRAM) |
| 640x200 2-color VBANK A | 640x200, monochrome, VRAM bank A |
| 640x200 2-color VBANK B | 640x200, monochrome, VRAM bank B |
| 640x200 4-color | 640x200, 4 barvy |
| MZ-700 compat (text 40x25) | MZ-700 kompatibilní textový mód |

Pod stavem módu je i surová hodnota registru DMD a rozpis jeho bitů
(MZ700, SCRW640, HICOLOR, VBANK).

### RF / WF

Stav VRAM controlleru pro Read Format (RF) a Write Format (WF).
Ovlivňuje, jakým způsobem CPU přistupuje k 4-plane organizaci VRAM
v 16-color módu.

| Pole | Význam |
|------|--------|
| `WF plane` | Maska, do kterých plane se má zapisovat (I/II/III/IV) |
| `WF mode` | Operace zápisu (SINGLE / EXOR / OR / RESET / REPLACE / PSET) |
| `RF plane` | Maska, ze kterých plane se má číst |
| `RF search` | Maska pro vyhledávání bitového vzoru |
| `VBANK (WF/RF)` | Aktivní VRAM banka pro přístup CPU |
| `WRITE FF` | `set`, dokud GDG nezapsal poslední zápis CPU z bufferu do VRAM; další přístup CPU do VRAM do té doby čeká (režim MZ-800 i MZ-700) |

### HW scroll

Hardwarový vertikální posun zobrazení.

| Pole | Význam |
|------|--------|
| `Enabled` | Zda je hardwarový scroll aktivní |
| `SSA` | Scroll Start Address |
| `SEA` | Scroll End Address |
| `SW` | Scroll Width |
| `SOF` | Scroll Offset |

### CG-RAM (MZ-700 mode)

Informativní sekce. CG-RAM (uživatelsky definované znaky pro MZ-700
textový mód) sídlí v prvních 4 KB VRAM plane I. Samotná data se
nezobrazují - patří do okna **Memory Map**. Panel jen informuje,
zda je MZ-700 kompatibilní mód aktivní.

### Palette

16 IGRB barevných čtverečků v gridu 4x4. Reprezentují všech 16
fyzických barev MZ-800, jak je vidíte na obrazovce s aktuálním
barevným schématem (Normal / Grayscale / Green).

Klik na čtvereček otevře popup s indexem (0-15) a RGB hodnotou.
Pod gridem jsou navíc surové hodnoty palette registrů PALGRP a
PAL0..3.

### Border

Barva borderu (rámu okolo aktivní oblasti) podle registru BOR.
Zobrazuje se index 0-15 a vedle něj malý ColorButton preview.


## Sekce na MZ-700

MZ-700 GDG nemá žádné platform-specifické registry, které by panel
ukazoval - žádnou paletu (jen 8 fixních IGRB barev na buňku přes
attribute RAM), žádný HW scroll, žádný border port. Atribut RAM
a znakový generátor nejsou součástí GDG.

Pro MZ-700 panel obsahuje pouze společnou sekci **Raster** dole.


## Sekce na MZ-1500

### Mode & Priority

Aktuální mód GDG a pořadí vrstev pro PCG overlay.

| Pole | Význam |
|------|--------|
| `DMD` | Surová hodnota Display Mode Descriptor registru |
| `Mode` | MZ-700 compat (PCG vypnutý) nebo MZ-1500 PCG overlay aktivní |
| `Layer priority` | BPF (background -> PCG -> foreground) nebo BFP (background -> foreground -> PCG) |

### PCG bank mapping

Mapování PCG bank do CPU adresního prostoru (D000-EFFF). PCG má tři
banky po 8 KB; v daném okamžiku je jen jedna z nich viditelná pro
CPU (přes SPEC mapping). GDG sám pro renderování čte všechny tři
banky vždy - mapování ovlivňuje výhradně přístup CPU.

| Pole | Význam |
|------|--------|
| `CPU view of D000-EFFF` | Co je aktuálně mapováno (PCG bank 1/2/3, CGROM, nebo VRAM/RAM) |
| `SPEC id` | Surová hodnota SPEC pole |
| `PCG bank size` | Velikost jedné PCG banky (bajty) |
| `PCG bank count` | Počet PCG bank |

### PCG usage

Počet znakových buněk, které mají v atributové RAM nastavený PCG
overlay bit. Užitečný indikátor toho, zda aplikace PCG vůbec
využívá.

- `0 / 1024` = aplikace používá jen znaky z CGROM.
- `> 0 / 1024` a PCG mód aktivní = PCG overlay se reálně používá.
- `> 0 / 1024` a MZ-700 compat mód = PCG bity v atributech jsou
  nastavené, ale GDG je ignoruje (overlay vypnutý DMD bitem).

### PCG palette

8 IGRB barevných čtverečků v jedné řadě. Reprezentuje paletu PCG
overlay (port 0xF1). Index 0 znamená v rendereru "PCG pixel je
průhledný" - barva pod ním (background nebo foreground vrstva)
zůstane viditelná. Indexy 1-7 jsou normální barvy.

Klik na čtvereček otevře popup s indexem, IGRB kódem a RGB hodnotou.


## Společná sekce - Raster

Tato sekce je stejná pro všechny tři architektury. Ukazuje aktuální
stav rastru a tempo generátoru GDG.

### Pozice paprsku a frame counter

| Pole | Význam |
|------|--------|
| `Scanline` | Aktuální řádek paprsku (0 až plný počet řádků snímku) |
| `Pixel clock` | Pozice v rámci aktuálního řádku (počet hodinových ticků od začátku snímku) |
| `VBLN count` | Počet snímků od startu emulátoru / loadnutí snapshotu (frame counter; reset stroje ho nenuluje) |

### Blanking a sync flagy

| Pole | Význam |
|------|--------|
| `HBLN flag` | Horizontal blanking - 1 = mimo viditelnou část řádku |
| `VBLN flag` | Vertical blanking - 1 = mimo viditelnou část snímku |
| `HSYNC` | Aktuální stav horizontálního sync signálu |
| `VSYNC` | Aktuální stav vertikálního sync signálu |

### Tempo signál

GDG generuje pomalý "tempo" signál odvozený z hlavních hodin přes
děličku 229. Slouží jako zdroj pro periodické přerušení (typicky
50 / 60 Hz v závislosti na PAL / NTSC).

| Pole | Význam |
|------|--------|
| `Tempo` | Aktuální stav tempo bitu (0 / 1) |
| `Tempo divider` | Živé počítadlo děličky 0..228 |
| `CTC0 divider` | Kolik master GDG hodinových ticků = 1 CTC0 tick (závisí na platformě a PAL/NTSC) |


## Tipy

### Sledování raster efektů

Pokud ladíte program, který přepíná paletu nebo border barvu uprostřed
snímku, sledujte hodnotu `Scanline` v sekci Raster. Společně s panelem
**CTC State** (kanál CTC2) snadno určíte, na kterém řádku se přerušení
spouští.

### Tichá obrazovka

Pokud emulátor vůbec nic nekreslí, podívejte se do GDG State okna na
hodnoty `VBLN count` a `Scanline`. Pokud `VBLN count` neroste, GDG
neběží (typicky problém v emulačním vlákně). Pokud `Scanline` zamrzlo
na stejné hodnotě, raster timing někde selhal.

### MZ-1500 a PCG

Pokud máte na MZ-1500 černou obrazovku tam, kde čekáte PCG grafiku,
zkontrolujte v sekci **PCG usage**, kolik buněk má PCG bit nastavený.
Pokud `0 / 1024`, aplikace PCG buňky vůbec nevyrobila. Pokud > 0 a
přesto nic nevidíte, podívejte se v sekci **Mode & Priority** zda je
DMD bit pro PCG mód zapnutý - bez něj GDG PCG overlay ignoruje.


## Vztah k ostatním oknům

| Co potřebujete | Kde to najdete |
|----------------|----------------|
| Vnitřní state GDG, paleta, border, PCG | **GDG State** (toto okno) |
| Mapování VRAM / ROM / RAM do CPU adresního prostoru | **Memory Map** |
| Surová data VRAM, CGROM, PCG bank | **Memory Map** (regiony) |
| Per-řádek timing měření (frame analysis) | **Measuring GDG** (jiné okno, jiný účel) |
| Periodické přerušení odvozené z GDG tempo signálu | **CTC State** (kanál CTC2) |
