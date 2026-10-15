# Zachycení zvuku

Zaznamená do složky, co dělá zvukový hardware emulovaného počítače,
v emulovaném čase - pro hledání chyb zvuku v programu i v emulátoru.
Všechny platformy (MZ-800, MZ-700 PAL a NTSC, MZ-1500); MZ-700 má jen
reproduktor.

## Spuštění a zastavení

| Cesta | Jak |
|-------|-----|
| Menu | **Tools -> Audio Capture -> Record Audio Capture** (`Alt + Q`) zachycení spustí; stejná položka nebo klávesa ho zastaví. |
| Příkazová řádka | `--audio-capture <složka>` ho spustí hned po startu, `--audio-capture-frames <počet>` ho zastaví po zadaném počtu snímků; v režimu `--headless` pak emulátor skončí. |
| MCP | `emu_audio_capture_start`, `emu_audio_capture_stop`, `emu_audio_capture_status` (viz [`mcp-server/tools-overview.md`](mcp-server/tools-overview.md)). |

Z menu vytvoří každé zachycení novou složku `<platforma>_audio_RRRRMMDD_HHMMSS`
ve výstupní složce (výchozí: `audio_captures` vedle emulátoru; používá ji
i MCP bez `dir`). Za běhu ukazuje levý horní roh obrazu **AUDIO** a zachycený
čas (v češtině **ZVUK**); po konci notifikace ohlásí složku. **Open Output
Folder** otevře výstupní složku ve správci souborů.

Zachycení začne na konci nejbližšího snímku; když se zapisuje `sdl.wav`, až
po dalších šesti snímcích (asi 0,1 s), během kterých se ustálí zvukový
výstup. Skončí stopem, po zadaném počtu snímků, po jedné hodině, nebo když
emulovaný čas začne znovu (reset, načtení snapshotu).

## Nastavení

**Tools -> Audio Capture -> Settings...** nastavuje výstupní složku a vybírá
soubory k zápisu; ukládá se do konfigurace (sekce `[AUDIO_CAPTURE]`).
Příkazová řádka používá stejný výběr souborů.

| Nastavení | Klíč INI | Výchozí |
|-----------|----------|---------|
| Výstupní složka | `output_dir` | prázdný = `audio_captures` vedle emulátoru |

| Soubor | Klíč INI | Obsah (výchozí vše zapnuto) |
|--------|----------|-------|
| `events.csv` | `events` | každá změna výstupní úrovně zvukového kanálu |
| `psg_writes.csv` | `psg_writes` | každý zápis do PSG s dekódováním |
| `reference.wav` | `reference_wav` | zvuk spočítaný přesně v emulovaném čase |
| `sdl.wav` | `sdl_wav` | zvuk ze zvukového výstupu - to, co slyšíte |

`capture.json` s popisem zachycení se zapisuje vždy.

## Soubory

Časy v CSV: `ticks` = takty hodin videočipu od začátku zachycení (frekvence
je `gdg_clk_hz` v capture.json), `time_us` = totéž v mikrosekundách,
`frame` = snímek od začátku (20 ms PAL, 16,7 ms NTSC).

**events.csv** - `ticks,time_us,frame,channel,level`. Kanály: `ctc0`
(reproduktor, úroveň 0 nebo 15), `psg0_0` .. `psg0_3` (PSG, kanál 3 = šum) a na
MZ-1500 nebo MZ-800 s druhým PSG `psg1_0` .. `psg1_3`. Úroveň 0-15 je výstup
kanálu (u PSG 15 minus útlum, když je výstup generátoru v jedničce, jinak 0).
První řádky (`ticks` 0) jsou úrovně na začátku.

**psg_writes.csv** - `ticks,time_us,frame,psg,byte,channel,reg,value`:
`psg` = cíl (1 = PSG0, 2 = PSG1, 3 = oba), `byte` = zapsaná hodnota (hex),
`channel` 0-3, `reg` a `value` ze stavu PSG po zápisu:

| reg | value |
|-----|-------|
| `tone_lo` | latch bajt tónu: spodní 4 bity děliče (dělič se změní až dalším datovým bajtem) |
| `tone` | celý 10bitový dělič |
| `volume` | útlum 0-15 (15 = vypnuto) |
| `noise` | `(typ << 2) \| rychlost` |

**reference.wav** a **sdl.wav** - 44100 Hz, 16 bit, stereo, stejný formát,
lze je přímo porovnat. reference.wav je spočítaný přesně v emulovaném čase,
sdl.wav je to, co vyrobil zvukový výstup emulátoru. Jejich rozdíl ukazuje
vady zvukového výstupu. sdl.wav je opožděný: bez zvukového zařízení
(`--headless`) přesně o jeden snímek, se zařízením podle jeho bufferu. Se
zařízením při MAX SPEED zvukový výstup snímky vynechává - capture.json má pak
`sdl_blocks` menší než `frames`.

**capture.json** - platforma, `gdg_clk_hz`, `frames_per_sec`, `wav_rate`,
názvy kanálů, počty (`frames`, `events`, `psg_writes`, `reference_samples`,
`sdl_blocks`, `sdl_samples`) a `stop_reason`.

## Příklad

```
mz800emu --headless --run-mzf hra.mzf --audio-capture C:/tmp/snd --audio-capture-frames 500
```

Zachytí 10 sekund hry (PAL) do `C:/tmp/snd` a skončí.
