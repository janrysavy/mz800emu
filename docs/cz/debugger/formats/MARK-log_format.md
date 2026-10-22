# Marker Log - formát exportních souborů

## Co je marklog

**marklog** je jeden ze subsystémů trace-suite. Loguje **uživatelské
markery** vyvolávané z BP action `mark "name"`:

```
break $pc == 0x1234 do
    mark "frame_start"
end
```

Při fire BP se zapíše 24 B záznam do `<dir>/<name>.NNN.bin`. Marker
name je předem zaregistrované při BP parse a v binárce ho reprezentuje
stabilní `uint16 marker_id`. Mapování id → name se dumpuje do
manifest `<name>.json` `subsys_header.markers`.

Slouží pro:
- označení významných míst v záznamu pro pozdější seek (= "tady startuje
  zajímavý frame")
- korelaci s ostatními trace-suite logy (cputrack, intlog, iorqlog,
  hwlog) přes sdílený `pxclk_total` timestamp
- označení uživatelské sekvence ("ISR_entry" → "ISR_exit") + spočítání
  T-states mezi nimi

marklog se ovládá z menu `Debugger Settings -> Trace Suite -> Marker Log`
(Off / Only With Debug Window / Always + Save on Exit + Max size [MB] +
Chunk [MB] + Set directory... + přepínač "Print marks to stdout"). Dále přes
CLI (`--marklog-mode`, `--marklog-save-on-exit`, `--marklog-max-total-mb`,
`--marklog-stdout`, `--marklog-dir`, `--marklog-name`, `--marklog-chunk-mb`),
INI sekci `[TRACE_MARKLOG]` nebo akci breakpointu (`mark "name"`). Basename
(name) jen přes CLI / INI.

`stdout_enabled` flag řídí back-compat zapsání `[BP-MARK] <name>` na
stdout. **Nezávislý na marklog mode** - lze mít obě, jednu, ani jednu.

## Adresářová struktura exportu

```
<dir>/
    <name>.json                    # meta + chunks anchor + markers registry
    <name>.000.bin                 # chunk 0 - per-marker záznamy
    <name>.001.bin                 # chunk 1
    ...
```

## Per-event záznam (24 B fixed)

Alignment 8 B, kompatibilní s intlog headerem (sdílené prvních 16 B).

| Offset | Velikost | Pole                                       |
|--------|----------|---------------------------------------------|
| 0      | 8 B      | pxclk_total (uint64 LE)                     |
| 8      | 4 B      | screens_total (uint32 LE)                   |
| 12     | 4 B      | pxclk_in_screen (uint32 LE)                 |
| 16     | 2 B      | marker_id (uint16 LE, 0xFFFF = invalid)     |
| 18     | 6 B      | reserved (padding, vyplněno nulami)         |

### Pole `marker_id`

Stabilní v rámci jednoho běhu emulátoru. Mapování na human-readable
jméno je v manifestu `<name>.json`:

```json
"subsys_header": {
    "event_record_size": 24,
    "marker_id_invalid": 65535,
    "marker_count": 3,
    "markers": {
        "0": "frame_start",
        "1": "ISR_entry",
        "2": "ISR_exit"
    },
    "start_pxclk": 0,
    "start_pxclk_in_screen": 0,
    "start_screens": 0,
    "start_cpuclk": 0
}
```

### Reserved padding (6 B)

Aktuálně vždy nulové. Reservováno pro budoucí rozšíření:
- 4 B value (např. asociovaný `$var` snapshot)
- 1 B marker type tag (info / warn / error)
- 1 B padding

## Manifest `<name>.json` - kompletní struktura

```json
{
    "subsystem": "marklog",
    "platform": "MZ-800",
    "pxclk_freq_hz": 17734475,
    "cpu_divider": 5,
    "pxclk_per_screen": 97344,
    "subsys_header": {
        "event_record_size": 24,
        "marker_id_invalid": 65535,
        "marker_count": 2,
        "markers": {
            "0": "frame_start",
            "1": "ISR_entry"
        },
        "start_pxclk": 0,
        "start_pxclk_in_screen": 0,
        "start_screens": 0,
        "start_cpuclk": 0
    },
    "chunks": [
        {
            "index": 0,
            "bytes": 48,
            "start_pxclk": 0,
            "start_cpuclk": 0,
            "start_screens": 0
        }
    ],
    "truncated": false
}
```

## Limity a chování při edge case

| Situace | Chování |
|---------|---------|
| Marker name > 63 chars | Truncate na 63 chars + stderr warning. Truncated name se používá v registry (= dva delší jména se stejným prefixem mohou kolidovat - nedoporučuje se). |
| 65536 unikátních markerů | Registrace odmítnuta + stderr warning. BP s touto mark action neloguje do marklog, ale stdout funguje pokud cfg ANO. |
| Záznam s neplatným marker_id (0xFFFF) | No-op (graceful, žádný error). |
| Mode = OFF + stdout_enabled = ON | Stdout funguje, marklog soubor se nevytvoří. |
| Mode = ALWAYS + stdout_enabled = OFF | Marklog soubor obsahuje záznamy, stdout tichý. |
| `mark "..."` v BP po dosažení max_total_mb | Marklog truncated, záznamy se přestanou ukládat. Stdout (pokud ON) funguje dál. |

## Korelace s ostatními logy

`pxclk_total` je sdílený monotónní counter napříč všemi 5 trace-suite
subsystémy. Analyzátor může merge timestamp-sort z více logů na společnou
osu času (např. korelovat marker "ISR_entry" s konkrétním
INTLOG_EVENT_IRQ_ACK_IM2 záznamem ze stejného `pxclk_total`).
