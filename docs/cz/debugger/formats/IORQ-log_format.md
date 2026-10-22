# IORQ Log - formát exportních souborů

## Co je iorqlog

**iorqlog** je jeden ze subsystémů trace-suite. Loguje **veškerý
I/O bus traffic** - každý IORQ IN / OUT, plus event types pro mapped
MREQ a ghost-bus reads.

Slouží pro analýzu I/O patterns, identifikaci protokolů periferií,
debug raster effects (mid-frame palette/border changes přes port 0xF0
/ 0xCF) a re-simulaci HW state externím tooligem.

iorqlog se ovládá z menu `Debugger Settings -> Trace Suite -> IORQ Log`
analogicky jako cputrack (Off / Only With Debug Window / Always +
Save on Exit + Max size [MB] + Chunk [MB] + Set directory...). Basename
(name) jen přes CLI / INI. Persistentní v ini sekci `[TRACE_IORQLOG]`.

## Adresářová struktura exportu

```
<dir>/
    <name>.json                    # meta + chunks anchor
    <name>.000.bin                 # chunk 0 - per-IORQ eventy
    <name>.001.bin                 # chunk 1
    ...
```

iorqlog **nemá initial state dump** - na rozdíl od cputrack jeho
události samy nesou veškerý kontext (port, value, source addr).

## Per-event záznam (24 B fixed)

| Offset | Velikost | Pole                                          |
|--------|----------|------------------------------------------------|
| 0      | 8 B      | pxclk_total (uint64 LE)                        |
| 8      | 4 B      | screens_total (uint32 LE)                      |
| 12     | 4 B      | pxclk_in_screen (uint32 LE)                    |
| 16     | 1 B      | event_type (viz níže)                          |
| 17     | 1 B      | direction (IN=0 / OUT=1)                       |
| 18     | 2 B      | source_addr (uint16 LE)                        |
| 20     | 2 B      | port_or_addr (uint16 LE)                       |
| 22     | 1 B      | value (uint8)                                  |
| 23     | 1 B      | pulse_duration_pxclk (uint8)                   |

### Pole pxclk_total / screens_total / pxclk_in_screen

Absolutní pxCLK timestamp + raster pozice v okamžiku začátku I/O
operace. Tři redundantní hodnoty pro odolnost (parser nepotřebuje
záhlaví pro převod):

- `pxclk_total` = celkový počet pxCLK od resetu emulátoru
- `screens_total` = počet kompletně vykreslených obrazovek
- `pxclk_in_screen` = pxCLK uvnitř aktuální obrazovky (0..pxclk_per_screen-1)

Vztah: `pxclk_total = screens_total * pxclk_per_screen + pxclk_in_screen`.

### Pole event_type

| Hodnota | Symbol | Popis |
|---------|--------|-------|
| `0` | `IORQLOG_EVENT_IORQ` | Standardní IORQ na mapovaný I/O port (= byl obsloužen některým chipem). |
| `1` | `IORQLOG_EVENT_MREQ_MAPPED` | MREQ na memory-mapped port (0xE000-0xE008 v MZ-700 mode s namapovanou horní ROM). |
| `2` | `IORQLOG_EVENT_IORQ_UNCONNECTED` | IORQ na nemapovaný port -> "duch sběrnice" hodnota z bus latch. |

### Pole direction

| Hodnota | Symbol | Popis |
|---------|--------|-------|
| `0` | `IORQLOG_DIR_IN` | CPU IN = read z portu (Z80 instrukce `IN A,(n)`, `IN r,(C)`, `INI`, `IND`, `INIR`, `INDR`). |
| `1` | `IORQLOG_DIR_OUT` | CPU OUT = write na port (Z80 instrukce `OUT (n),A`, `OUT (C),r`, `OUTI`, `OUTD`, `OTIR`, `OTDR`). |

### Pole source_addr

Adresa instrukce, ze které I/O operace vznikla. **Sémantika závisí
na event_type:**

| event_type | source_addr | Důvod |
|------------|-------------|-------|
| `IORQ` | regBC (Z80 BC registr) | Z80 IORQ s BC na adresové sběrnici (8255 PA, CTC ch select, ...). Plně 16-bit adresování přes IN/OUT (C),r instrukce; pro IN/OUT (n),A je low byte = `n`, high byte = `A`. |
| `MREQ_MAPPED` | regPC (Z80 PC registr) | MREQ má na sběrnici PC instrukce, která access provedla. |
| `IORQ_UNCONNECTED` | regBC | Stejné jako IORQ. |

Tento dual interpretation je v Sharp MZ specific - umožňuje rozlišit
zda byl access skutečný IORQ nebo memory-mapped registr přístup.

### Pole port_or_addr

Cílový port (pro IORQ) nebo cílová adresa (pro MREQ_MAPPED).

- Pro `IORQ` / `IORQ_UNCONNECTED`: 16-bit port, ale Sharp MZ-800/700/1500
  reálně používá jen 8-bit (low byte). Pro 8-bit IN/OUT je `port_or_addr
  = 0x00<port>`. Pro MZ-800 s GDG 16-bit adresováním je `port_or_addr
  = <high>:<low>` (např. `0x06CF` = GDG BORDER).
- Pro `MREQ_MAPPED`: plná 16-bit memory adresa (typicky 0xE000-0xE008
  v MZ-700 mode).

### Pole value

Bajt přečtený (IN) nebo zapsaný (OUT). Pro **IORQ_UNCONNECTED IN**
je to "ghost" hodnota z bus latch (typ. poslední value na sběrnici,
nebo 0xFF).

### Pole pulse_duration_pxclk

Délka I/O cyklu v pxCLK (= šířka IORQ pulzu na sběrnici). Standardní
Z80 IORQ trvá ~3 T-state cycles. Pokud byl udělen WAIT (= chip nestihl),
pulz je delší.

| Hodnota | Význam |
|---------|--------|
| `0` | Nezaznamenáno (současná implementace defauluje na 0). |
| `>0` | Skutečná délka v pxCLK. Pro převod na T-states: `t_states = pulse / cpu_divider`. |

## Manifest `<name>.json`

```json
{
  "subsys": "iorqlog",
  "platform": "MZ-800",
  "pxclk_freq": 17734475,
  "cpu_divider": 5,
  "pxclk_per_screen": 97344,
  "truncated": false,
  "truncated_reason": "",
  "chunks": [
    { "index": 0, "file": "iorqlog.000.bin", "bytes": 67108824,
      "start_pxclk": 0, "start_cpuclk": 0, "start_screens": 0 }
  ],
  "subsys_header": {
    "start_pxclk": 0,
    "start_pxclk_in_screen": 0,
    "start_screens": 0,
    "start_cpuclk": 0,
    "event_record_size": 24
  }
}
```

iorqlog má **jednodušší meta než cputrack** - žádný initial state
dump. Eventy jsou self-contained (každý nese pxclk timestamp, source
addr, port, value).

`chunks[i]` anchor je zachován z konvence (= rychlý seek), ale pro
iorqlog ho parser nepotřebuje (timestamp je v každém eventu).

## Coverage

iorqlog emituje **všechny tři event types**:

- **`IORQ`** - aktivní v obou architekturách (mz800 + mz1500).
- **`MREQ_MAPPED`**:
  - **MZ-1500** mapuje E000-E008 nativně bez podmínek (chipy PIO8255 /
    CTC8253 / GDG status jsou tam vždy)
  - **MZ-800** mapuje E000-E008 jen v MZ-700 mode (DMD bit 3 = 1) s
    namapovanou horní ROM (ROM_E000) **a NE Prohibited stav** (= jinak
    je tam buď horní ROM, mapped ports area "off" 0xFF v 800 native,
    nebo 0x1A shadow byte v Prohibited - žádný chip access se nekoná)
  - Read side: $E009-$E00F v 700 mode vrací 0x1A shadow byte
    (nezávisle na PIO/CTC stavu) - MREQ_MAPPED event NEFIRE
  - Filtrace na reálné CPU instr (ne debug browser nebo load operace)
- **`IORQ_UNCONNECTED`** - ghost reads na nemapované porty se detekují
  podle toho, zda žádný chip access nepoznal port (= retval z bus latch).

`pulse_duration_pxclk` je vždy 0 (extrakt přesné délky IORQ pulzu
z logging cesty není v aktuálním emu k dispozici).

## Použití

### Z UI

`Debugger Settings -> Trace Suite -> IORQ Log`. Stejný UX pattern jako
ostatní trace-suite subsystémy.

### Načtení dat v Pythonu (příklad)

```python
import json, struct
from pathlib import Path

def load_iorqlog(meta_path):
    meta_path = Path(meta_path)
    meta = json.loads(meta_path.read_text())
    base_dir = meta_path.parent

    events = []
    for chunk in meta["chunks"]:
        data = (base_dir / chunk["file"]).read_bytes()
        n_events = chunk["bytes"] // 24
        for i in range(n_events):
            o = i * 24
            pxclk     = struct.unpack_from("<Q", data, o + 0)[0]
            screens   = struct.unpack_from("<I", data, o + 8)[0]
            px_in_scr = struct.unpack_from("<I", data, o + 12)[0]
            evtype    = data[o + 16]
            direction = data[o + 17]
            src_addr  = struct.unpack_from("<H", data, o + 18)[0]
            port      = struct.unpack_from("<H", data, o + 20)[0]
            value     = data[o + 22]
            pulse     = data[o + 23]
            events.append((pxclk, screens, px_in_scr, evtype, direction,
                           src_addr, port, value, pulse))
    return meta, events

meta, events = load_iorqlog("./trace-suite/iorqlog.json")
# Příklad: všechny zápisy na port 0xF0 (GDG palette/border)
palette_writes = [e for e in events if e[3] == 0 and e[4] == 1 and e[6] == 0xF0]
```

## Hot path

iorqlog hook se aktivuje při OUT/IN přes debugger callback swap. Pokud
není aktivní žádný debugger / trace subsystém, dispatch jde přes vanilní
cestu (zero overhead).

Konzolová zpráva `[trace-suite] iorqlog: chunk N swap to disk (XX B)`
upozorní na flush.

