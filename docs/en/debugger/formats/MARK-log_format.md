# Marker Log - export file format

## What is marklog

**marklog** is one of the trace-suite subsystems. It logs **user-defined
markers** triggered by the BP action `mark "name"`:

```
break $pc == 0x1234 do
    mark "frame_start"
end
```

When the BP fires, a 24 B record is written to `<dir>/<name>.NNN.bin`.
The marker name is pre-registered during BP parsing and is represented
in the binary by a stable `uint16 marker_id`. The id -> name mapping is
dumped to the manifest `<name>.json` `subsys_header.markers`.

Used for:
- marking significant points in the recording for later seek (= "an
  interesting frame starts here")
- correlating with other trace-suite logs (cputrack, intlog, iorqlog,
  hwlog) via the shared `pxclk_total` timestamp
- delimiting a user-defined sequence ("ISR_entry" -> "ISR_exit") and
  counting T-states between them

marklog is controlled from the menu `Debugger Settings -> Trace Suite ->
Marker Log` (Off / Only With Debug Window / Always + Save on Exit + Max size
[MB] + Chunk [MB] + Set directory... + a "Print marks to stdout" toggle). Also
via CLI (`--marklog-mode`, `--marklog-save-on-exit`, `--marklog-max-total-mb`,
`--marklog-stdout`, `--marklog-dir`, `--marklog-name`, `--marklog-chunk-mb`),
the INI section `[TRACE_MARKLOG]` or a breakpoint action (`mark "name"`). The
basename (name) is set via CLI / INI only.

The `stdout_enabled` flag controls the back-compat printing of
`[BP-MARK] <name>` to stdout. **Independent of marklog mode** - you can
have both, one, or neither.

## Export directory structure

```
<dir>/
    <name>.json                    # meta + chunks anchor + markers registry
    <name>.000.bin                 # chunk 0 - per-marker records
    <name>.001.bin                 # chunk 1
    ...
```

## Per-event record (24 B fixed)

Alignment 8 B, compatible with intlog header (shared first 16 B).

| Offset | Size     | Field                                       |
|--------|----------|---------------------------------------------|
| 0      | 8 B      | pxclk_total (uint64 LE)                     |
| 8      | 4 B      | screens_total (uint32 LE)                   |
| 12     | 4 B      | pxclk_in_screen (uint32 LE)                 |
| 16     | 2 B      | marker_id (uint16 LE, 0xFFFF = invalid)     |
| 18     | 6 B      | reserved (padding, zero-filled)             |

### Field `marker_id`

Stable within a single emulator run. The mapping to human-readable
name is in the manifest `<name>.json`:

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

Currently always zero. Reserved for future extensions:
- 4 B value (e.g. associated `$var` snapshot)
- 1 B marker type tag (info / warn / error)
- 1 B padding

## Manifest `<name>.json` - full structure

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

## Limits and edge-case behavior

| Situation | Behavior |
|-----------|----------|
| Marker name > 63 chars | Truncate to 63 chars + stderr warning. The truncated name is used in the registry (= two longer names sharing the same prefix may collide - not recommended). |
| 65536 unique markers | Registration refused + stderr warning. A BP with this mark action does not log to marklog, but stdout still works if cfg ON. |
| Record with invalid marker_id (0xFFFF) | No-op (graceful, no error). |
| Mode = OFF + stdout_enabled = ON | Stdout works, marklog file is not created. |
| Mode = ALWAYS + stdout_enabled = OFF | Marklog file contains records, stdout silent. |
| `mark "..."` in BP after reaching max_total_mb | Marklog truncated, records stop being stored. Stdout (if ON) keeps working. |

## Correlation with other logs

`pxclk_total` is a shared monotonic counter across all 5 trace-suite
subsystems. The analyzer can merge timestamp-sort from multiple logs
onto a common time axis (e.g. correlate a "ISR_entry" marker with a
specific INTLOG_EVENT_IRQ_ACK_IM2 record at the same `pxclk_total`).
