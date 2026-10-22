# Memory Search - hledání proměnné

Memory Search najde, kde program drží hodnotu - životy, skóre, časovač,
polohu - zužováním množiny kandidátních adres: pořídí vzorek paměti,
nechá hodnotu v programu změnit, ponechá jen pozice, které se změnily
stejně, a opakuje. Co zbude, jde sledovat, zmrazit, hlídat breakpointem
na zápis nebo otevřít v Memory Browseru.

K dispozici na MZ-800, MZ-700 a MZ-1500, v GUI i přes MCP (viz
[MCP nástroje](../mcp-server/tools-overview.md), oddíl Memory search).

## Otevření okna

- Hlavní menu `Debugger` -> `Memory Search`, nebo **Alt+Shift+E**.

Jestli je okno otevřené, se neukládá; jeho nastavení (prostor, typ,
pořadí bajtů, ...) se ukládá do sekce `[MEMSEARCH_WINDOW]` konfigurace.
Kandidáti se neukládají.

## Kde hledat

**Space** volí paměť:

| Prostor | Obsah |
|---------|-------|
| User RAM (výchozí) | 64 KB RAM tak, jak ji čte CPU, bez ohledu na to, co je přes ni namapované (ROM, VRAM). S MemExtem jsou to banky namapované v okamžiku vzorku |
| Logical Z80 | Co CPU vidí teď, včetně ROM, VRAM a mapovaných portů na 0E000h (jejich hodnoty jsou z poslední aktualizace emulace) |
| VRAM | Textová a atributová VRAM režimu MZ-700 a čtyři VRAM roviny MZ-800 |
| MemExt banks | Všechny RAM banky paměťového rozšíření |
| RAM disks | Banky RAM disku a RAM disků PEZIK |
| All writable | RAM, VRAM, CG-RAM, PCG, RAM MemExt a RAM disky dohromady |

ROM, CG-ROM a FLASH se neprohledávají nikdy. Zahrnou se jen připojená
zařízení.

Když program přepíná banky MemExt, ukazuje User RAM v různých vzorcích
různou paměť a hledání může proměnnou ztratit. Hledejte pak v **MemExt
banks**.

Prostor nad 4 MiB (RAM disk 16 MB) potřebuje **Allow large space** (až
32 MiB): hledání drží v paměti tři vzorky prostoru a historii undo.

## Co hledat

**Type**:

- `u8`, `s8`, `u16`, `s16`, `u24`, `s24`, `u32`, `s32` - celá čísla bez
  znaménka / se znaménkem. **Byte order** LE (pořadí Z80, nižší bajt
  první) nebo BE.
- `bcd` - packed BCD, dvě číslice na bajt, 1-4 bajty (**Bytes**). Bajt
  s nibblem nad 9 není BCD a pozice vypadne.
- `digits` - jedna desítková číslice na bajt, 1-8 bajtů. **Digits** volí
  kód: holé 00h-09h, ASCII 30h-39h nebo display kód Sharp MZ 20h-29h (to,
  co se zapisuje do textové VRAM). S **Leading spaces as zeros** se mezera
  před první číslicí počítá jako 0 (ASCII 20h, display kód 00h). BE =
  nejvyšší číslice první, jak se tiskne.

**Aligned** ponechá jen pozice dělitelné šířkou. Hodnota nikdy
nepřesahuje konec regionu.

Skóre vytištěné na obrazovce se často najde snáz jako `digits` ve VRAM
(display kód) než v RAM; breakpoint na zápis na tuto pozici obrazovky
pak najde rutinu, která ho tiskne, a z ní se dojde k proměnné.

## Hledání

1. **New search** pořídí vzorek; každá pozice je kandidát.
2. Nechte hodnotu v programu změnit (ztráta života, čekání na časovač).
3. Zvolte filtr a stiskněte **Apply filter**. Se zapnutým **Take new
   sample** (výchozí) se pořídí a porovná nový vzorek; kandidáti, kteří
   nevyhoví, vypadnou.
4. Kroky 2-3 opakujte, dokud nezbude pár kandidátů.

Za běhu emulace se vzorek pořídí na nejbližším snímku, v pauze hned.
Řádek pod tlačítky ukazuje počet kandidátů, krok a snímek posledního
vzorku.

**Filtry** (**compared with** hodnota, předchozí nebo první):

| Filtr | Kandidát zůstane, když jeho hodnota |
|-------|-------------------------------------|
| equal to / not equal to / less than / less or equal / greater than / greater or equal | je vůči referenci v daném vztahu |
| between | leží mezi **Value** a **and** (včetně) |
| increased / decreased | vzrostla / klesla vůči předchozímu nebo prvnímu vzorku |
| increased by / decreased by / changed by | se změnila přesně o **Difference** |
| changed / unchanged | se změnila / nezměnila |
| bits under mask equal | má bity pod **Mask** stejné jako reference (celočíselné typy) |

- **previous** = vzorek před porovnávaným, **first** = vzorek z New
  search.
- **Not** filtr obrátí. Neplatná hodnota BCD nebo číslic vypadne vždy.
- Aritmetika je bez přetečení: u8 z 00h na FFh není "decreased by 1".
- **Sample** pořídí nový vzorek bez filtrace. Pak filtrujte s vypnutým
  **Take new sample**: poslední vzorek se porovná s tím před ním.
- **Preview** označí červeně kandidáty, které by aktuální filtr vyřadil,
  podle posledního vzorku (bez nového vzorku).
- **Undo** / **Redo** vrací kandidáty spolu s referenčními vzorky: filtr
  po Undo porovná se správnou předchozí hodnotou.

Čítač ve hře se nemusí změnit mezi každými dvěma vzorky (časovač může
chvíli stát). Striktní **decreased** ho pak vyřadí. Spolehlivější je
v každém kroku **less or equal** proti **previous** a k tomu **less
than** proti **first** (Take new sample vypnuté), který vyřadí hodnoty,
co se nezměnily nikdy.

Kandidáti v regionu, který byl při vzorku odpojený (MemExt nebo RAM disk
odebraný během hledání), zůstávají beze změny a zobrazí se jako
odpojení.

## Kandidáti

Tabulka se zobrazí, když zbude méně než 10000 kandidátů: region, offset
v regionu, CPU adresa (kde je buňka teď pro CPU vidět), bajty a aktuální,
předchozí a první hodnota.

Pravé tlačítko na kandidátovi:

- **Show in Memory Browser** - otevře Memory Browser na tomto bajtu.
- **Add to Watch** - přidá CPU adresu do okna Watch (BCD, číslice
  a 24bitové hodnoty jako bajty). Potřebuje CPU adresu.
- **Add write breakpoint** - breakpoint MEM_W na hodnotu: zóna RAM pro
  User RAM (nechytá zápisy do ROM ani VRAM na stejné adrese), VRAM
  framebuffer pro textovou VRAM MZ-700, banka PEHU a offset v bance pro
  banku MemExt PEHU (spustí se, ať je banka namapovaná do kteréhokoliv
  okna CPU), CPU adresa pro banku Luftner namapovanou právě teď. Není pro
  VRAM rovinu, CG-RAM, PCG a RAM disky.
- **Freeze at current value** - zmrazí bajty hodnoty (Freeze Bytes, viz
  [Memory Browser](memory-browser/README.md)). Hodnota se zapisuje zpět
  jednou za snímek: program svou změnu vidí až do konce snímku, takže hra,
  která ubere život a ve stejném snímku ho otestuje, to pozná.
- **Who wrote this byte?** - otevře [RAM Write Log](ram-write-log.md) pro
  buňku.
- **Copy address**, **Eliminate** (ruční vyřazení kandidáta).

## Související

- [Memory Browser](memory-browser/README.md) - hex pohled na všechny
  regiony, jednorázové hledání vzoru, Freeze Bytes.
- [Watch](watch.md) - živé hodnoty proměnných.
- [RAM Write Log](ram-write-log.md) - kdo zapsal bajt.
