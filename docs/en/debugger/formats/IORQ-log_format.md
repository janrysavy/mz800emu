# IORQ Log - export file format

## What is iorqlog

**iorqlog** is one of the trace-suite subsystems. It logs **all I/O bus
traffic** - every IORQ IN / OUT, plus event types for mapped MREQ and
ghost-bus reads.

Used for I/O pattern analysis, identification of peripheral protocols,
debug of raster effects (mid-frame palette/border changes via port 0xF0
/ 0xCF) and external-tool re-simulation of HW state.

iorqlog is controlled from the menu `Debugger Settings -> Trace Suite ->
IORQ Log`, analogously to cputrack (Off / Only With Debug Window /
Always + Save on Exit + Max size [MB] + Chunk [MB] + Set directory...).
Basename (name) only via CLI / INI. Persistent in INI section `[TRACE_IORQLOG]`.

## Export directory structure

```
<dir>/
    <name>.json                    # meta + chunks anchor
    <name>.000.bin                 # chunk 0 - per-IORQ events
    <name>.001.bin                 # chunk 1
    ...
```

iorqlog **has no initial state dump** - unlike cputrack, its events
themselves carry all the context (port, value, source addr).

## Per-event record (24 B fixed)

| Offset | Size     | Field                                          |
|--------|----------|------------------------------------------------|
| 0      | 8 B      | pxclk_total (uint64 LE)                        |
| 8      | 4 B      | screens_total (uint32 LE)                      |
| 12     | 4 B      | pxclk_in_screen (uint32 LE)                    |
| 16     | 1 B      | event_type (see below)                         |
| 17     | 1 B      | direction (IN=0 / OUT=1)                       |
| 18     | 2 B      | source_addr (uint16 LE)                        |
| 20     | 2 B      | port_or_addr (uint16 LE)                       |
| 22     | 1 B      | value (uint8)                                  |
| 23     | 1 B      | pulse_duration_pxclk (uint8)                   |

### Fields pxclk_total / screens_total / pxclk_in_screen

Absolute pxCLK timestamp + raster position at the moment the I/O
operation starts. Three redundant values for robustness (the parser
needs no header for conversion):

- `pxclk_total` = total pxCLK count since emulator reset
- `screens_total` = number of fully rendered screens
- `pxclk_in_screen` = pxCLK inside the current screen
  (0..pxclk_per_screen-1)

Relation: `pxclk_total = screens_total * pxclk_per_screen + pxclk_in_screen`.

### Field event_type

| Value | Symbol | Description |
|-------|--------|-------------|
| `0` | `IORQLOG_EVENT_IORQ` | Standard IORQ on a mapped I/O port (= it was serviced by some chip). |
| `1` | `IORQLOG_EVENT_MREQ_MAPPED` | MREQ on a memory-mapped port (0xE000-0xE008 in MZ-700 mode with upper ROM mapped). |
| `2` | `IORQLOG_EVENT_IORQ_UNCONNECTED` | IORQ on an unmapped port -> "bus ghost" value from the bus latch. |

### Field direction

| Value | Symbol | Description |
|-------|--------|-------------|
| `0` | `IORQLOG_DIR_IN` | CPU IN = read from a port (Z80 instructions `IN A,(n)`, `IN r,(C)`, `INI`, `IND`, `INIR`, `INDR`). |
| `1` | `IORQLOG_DIR_OUT` | CPU OUT = write to a port (Z80 instructions `OUT (n),A`, `OUT (C),r`, `OUTI`, `OUTD`, `OTIR`, `OTDR`). |

### Field source_addr

Address of the instruction that originated the I/O operation. **The
semantics depend on event_type:**

| event_type | source_addr | Reason |
|------------|-------------|--------|
| `IORQ` | regBC (Z80 BC register) | Z80 IORQ with BC on the address bus (8255 PA, CTC ch select, ...). Full 16-bit addressing via IN/OUT (C),r instructions; for IN/OUT (n),A, the low byte is `n` and the high byte is `A`. |
| `MREQ_MAPPED` | regPC (Z80 PC register) | MREQ has on the bus the PC of the instruction performing the access. |
| `IORQ_UNCONNECTED` | regBC | Same as IORQ. |

This dual interpretation is specific to Sharp MZ - it allows
distinguishing whether the access was a real IORQ or a memory-mapped
register access.

### Field port_or_addr

Target port (for IORQ) or target address (for MREQ_MAPPED).

- For `IORQ` / `IORQ_UNCONNECTED`: 16-bit port, but Sharp MZ-800/700/1500
  actually uses only 8-bit (low byte). For 8-bit IN/OUT, `port_or_addr
  = 0x00<port>`. For MZ-800 with GDG 16-bit addressing, `port_or_addr
  = <high>:<low>` (e.g. `0x06CF` = GDG BORDER).
- For `MREQ_MAPPED`: full 16-bit memory address (typically 0xE000-0xE008
  in MZ-700 mode).

### Field value

Byte read (IN) or written (OUT). For **IORQ_UNCONNECTED IN** it is the
"ghost" value from the bus latch (typically the last value on the bus,
or 0xFF).

### Field pulse_duration_pxclk

Length of the I/O cycle in pxCLK (= IORQ pulse width on the bus).
A standard Z80 IORQ takes ~3 T-state cycles. If a WAIT was granted
(= the chip could not keep up), the pulse is longer.

| Value | Meaning |
|-------|---------|
| `0` | Not recorded (the current implementation defaults to 0). |
| `>0` | Actual length in pxCLK. For conversion to T-states: `t_states = pulse / cpu_divider`. |

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

iorqlog has a **simpler meta than cputrack** - no initial state dump.
Events are self-contained (each carries pxclk timestamp, source addr,
port, value).

The `chunks[i]` anchor is preserved by convention (= fast seek), but
the parser does not need it for iorqlog (the timestamp is in every
event).

## Coverage

iorqlog emits **all three event types**:

- **`IORQ`** - active in both architectures (mz800 + mz1500).
- **`MREQ_MAPPED`**:
  - **MZ-1500** maps E000-E008 natively without conditions (PIO8255 /
    CTC8253 / GDG status chips are always there)
  - **MZ-800** maps E000-E008 only in MZ-700 mode (DMD bit 3 = 1) with
    the upper ROM mapped (ROM_E000) **and NOT in Prohibited state**
    (= otherwise it is either the upper ROM, mapped ports area "off"
    0xFF in 800 native, or 0x1A shadow byte in Prohibited - no chip
    access happens)
  - Read side: $E009-$E00F in 700 mode returns the 0x1A shadow byte
    (independent of PIO/CTC state) - MREQ_MAPPED event does NOT fire
  - Filtered to real CPU instr (not debug browser or load operations)
- **`IORQ_UNCONNECTED`** - ghost reads on unmapped ports are detected
  by checking whether no chip access recognized the port (= retval
  from the bus latch).

`pulse_duration_pxclk` is always 0 (extracting the exact IORQ pulse
length from the logging path is not currently available in the emu).

## Usage

### From the UI

`Debugger Settings -> Trace Suite -> IORQ Log`. Same UX pattern as the
other trace-suite subsystems.

### Loading data in Python (example)

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
# Example: all writes to port 0xF0 (GDG palette/border)
palette_writes = [e for e in events if e[3] == 0 and e[4] == 1 and e[6] == 0xF0]
```

## Hot path

The iorqlog hook is activated on OUT/IN via the debugger callback swap.
If no debugger / trace subsystem is active, dispatch goes through the
vanilla path (zero overhead).

The console message `[trace-suite] iorqlog: chunk N swap to disk (XX B)`
notifies about a flush.

