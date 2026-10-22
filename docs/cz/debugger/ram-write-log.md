# RAM Write Log - kdo zapsal tento bajt?

Záznam zápisů do paměti (RAM Write Log) za běhu programu zaznamenává
zápisy do paměti: u každého zápisu čas, adresu instrukce, která zapsala,
CPU adresu, paměťovou buňku, původní a novou hodnotu a zdroj zápisu.
Odpovídá na otázku "kdo zapsal hodnotu na adresu X?" - i u bajtu, který
program přepsal už dávno - a "který kód zapisuje do této oblasti?".

K dispozici na MZ-800, MZ-700 i MZ-1500, v GUI, přes MCP, v akcích
breakpointů i z příkazové řádky.

## Otevření okna

- Hlavní menu `Debugger` -> `RAM Write Log`.
- Memory Browser: pravé tlačítko na bajtu -> **Who wrote this byte?**
  Otevře okno a ukáže posledního zapisovatele a historii zápisů bajtu.

Zda je okno otevřené, se neukládá. Záznam na okně nezávisí: ovládá se
z okna, přes MCP, akcemi breakpointů nebo z příkazové řádky.

## Záznam

Horní část okna:

- **From** / **To** - rozsah CPU adres (hex). Obě pole prázdná = celá
  paměť. Jen From = jeden bajt.
- **Mode** - co se stane, když se záznam zaplní: **Ring** drží nejnovější
  záznamy (nejstarší se přepisují), **Fill** drží první záznamy a další
  zápisy jen počítá jako zahozené.
- **Records** - velikost záznamu (16-16777216 položek po 32 bajtech,
  výchozí 1048576 = 32 MB).
- **Start** spustí záznam (běžící záznam se nahradí), **Stop** ho zastaví
  (záznamy zůstanou čitelné), **Clear** záznam vyprázdní.

Stavový řádek ukazuje, zda se zaznamenává, kolik položek záznam drží,
kolik zápisů se přepsalo (Ring) nebo zahodilo (Fill), použitou paměť
a rozsah.

Reset počítače a načtení snapshotu záznam vyprázdní; běžící záznam
pokračuje.

Záznam celé paměti zpomalí emulaci víc než záznam malého rozsahu. Na
MZ-800 opouštějí rychlou paměťovou cestu emulátoru jen zápisy do 4 KiB
stránek, ve kterých rozsah leží; ostatní zápisy běží jako bez záznamu.
Změřeno na MZ-800 v MAX SPEED se dvěma programy: jeden bajt nebo jedna
4 KiB stránka stály zhruba 4-12 % rychlosti emulace, celá paměť zhruba
11-20 %; MEM_W breakpoint s akcí `continue` na stejném bajtu zhruba
17-27 %. Bez záznamu se rychlost emulace nemění.

## Kdo zapsal

Zadejte CPU adresu a stiskněte **Find** (nebo použijte **Who wrote this
byte?** v Memory Browseru). Pro každou paměťovou buňku na této adrese
okno ukáže:

- **posledního zapisovatele** - adresu zapisující instrukce a zdroj.
  Drží se pro každou buňku, i když záznam toho zápisu ring už přepsal;
- **historii zápisů** buňky od nejnovějšího (nejvýš 100 zápisů), pokud
  jsou záznamy ještě v záznamu.

**Include unchanged writes** ukáže i zápisy, které zapsaly hodnotu,
kterou už buňka měla.

Adresa se převede podle mapování paměti v okamžiku dotazu: stejná CPU
adresa může být jednou RAM a jindy VRAM (banking), to jsou různé buňky
s vlastní historií. Na MZ-800 dá adresa grafické VRAM všechny čtyři
plány. Region Memory Browseru jiný než Logical (RAM, MemExt, VRAM,
CG-RAM, PCG) se ptá přesně na buňku daného regionu.

## Nejnovější zápisy

Tabulka dole ukazuje posledních 200 zápisů, obnovuje se dvakrát za
sekundu, pokud je zapnuté **Auto refresh**. **Show unchanged writes**
ukáže i zápisy, které buňku nezměnily.

## Sloupce

| Sloupec | Význam |
|---------|--------|
| Seq | Číslo záznamu od spuštění nebo vyprázdnění záznamu |
| Frame | Emulovaný snímek |
| PC | Adresa instrukce, která zapsala. U přerušení (zdroj Interrupt / NMI) návratová adresa, kterou CPU uložil. `-` u zápisů bez instrukce (Debugger, Freeze, Tape, Load) |
| Address | CPU adresa zápisu, `-` když ji zápis neměl (např. zápis do regionu Memory Browseru) |
| Cell | Paměť a offset: RAM, MemExt (s připojeným MemExt jsou stránky RAM banky MemExt), VRAM (grafická VRAM MZ-800: plán x 2000h + offset, s číslem plánu), PCG (MZ-1500), ROM |
| Old / New | Hodnota před zápisem a po něm |
| Source | Instruction, Interrupt (return address), NMI (return address), Debugger (Memory Browser, zápis do paměti přes MCP), Breakpoint poke, Freeze, Tape (CMT hack), Load |
| Note | `N bytes loaded` = jeden souhrnný záznam nahraného bloku; `dropped (ROM)` = zápis pod ROM, který paměť nezměnil; `value unchanged` |

Poznámky:

- `LDIR`, `PUSH` a podobné instrukce dají jeden záznam na každý zapsaný
  bajt, všechny s adresou instrukce. Dva bajty `PUSH` a návratové adresy
  uložené při přijetí přerušení se zaznamenají v pořadí zápisu: vyšší
  bajt dřív, pak nižší (jako v manuálu Zilog Z80; na skutečném MZ-800
  neměřeno).
- Na MZ-800 dá zápis do grafické VRAM jeden záznam na každý plán, do
  kterého režim zápisu skutečně zapisuje (např. REPLACE zapíše 00 i do
  plánů, které nejsou vybrané).
- Nahrání programu (`--run-mzf`, páska přes CMT hack, nahraný blok
  paměti) se zaznamená jedním souhrnným záznamem na blok a 4 KiB stránku,
  ne po bajtech.

## Příkazová řádka a konfigurace

| Volba | INI `[RAMLOG]` | Význam |
|-------|----------------|--------|
| `--ramlog-mode <off\|window\|always>` | `mode = OFF / WITH_WINDOW / ALWAYS` | Spustit záznam od startu (`always`), jen když je otevřené okno debuggeru (`window`), nebo vůbec (`off`, výchozí) |
| `--ramlog-ring <ring\|fill>` | `ring = RING / FILL` | Režim zaplněného záznamu |
| `--ramlog-capacity <N>` | `capacity` | Velikost v položkách |
| `--ramlog-lo <addr>` | `range_lo` | První CPU adresa |
| `--ramlog-hi <addr>` | `range_hi` | Poslední CPU adresa (`0` + `0xFFFF` = celá paměť) |

Čísla v souboru INI jsou šestnáctková (s prefixem `0x` i bez něj),
např. `capacity = 0x100000`, `range_lo = 0x4000`. Na příkazové řádce se
přijímá desítkový zápis i hex s `0x`.

Záznam spuštěný z konfigurace zastaví zase jen konfigurace (režim
`window`: když se zavře okno debuggeru); po Stop v okně nebo přes MCP se
automaticky znovu nespustí.

## Akce breakpointů

- `ramlog_start` - záznam celé paměti; `ramlog_start <from>, <to>` - záznam
  rozsahu CPU adres. Výrazy se vyhodnotí při zásahu breakpointu, např.
  `ramlog_start HL, HL + 15`.
- `ramlog_stop`, `ramlog_clear`.
- Funkce výrazu `lastwriter(addr)` - adresa instrukce, která naposledy
  zapsala buňku na `addr` (aktuální mapování), `-1` = neznámý. Příklad
  podmínky: `lastwriter(0x4000) != 0x3009`.

Viz [Action DSL](breakpoints/action-dsl.md) a
[Syntaxe výrazů](breakpoints/expression-syntax.md).

## MCP

`emu_ramlog_start`, `emu_ramlog_stop`, `emu_ramlog_clear`,
`emu_ramlog_status`, `emu_ramlog_who_wrote`, `emu_ramlog_writers` a kanál
`ramlog` nástroje `emu_trace_read`. Viz
[MCP nástroje](../mcp-server/tools-overview.md) a MCP dokument
`emulator://docs/ramlog`.
