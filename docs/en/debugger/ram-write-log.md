# RAM Write Log - who wrote this byte?

The RAM write log records the writes into memory while the program runs:
for every write the time, the address of the instruction that wrote, the
CPU address, the memory cell, the old and the new value and the source
of the write. It answers "who wrote the value at address X?" - even for
a byte that the program overwrote long ago - and "which code writes into
this area?".

Available on MZ-800, MZ-700 and MZ-1500, in the GUI, over MCP, in
breakpoint actions and from the command line.

## Opening the window

- Main menu `Debugger` -> `RAM Write Log`.
- Memory Browser: right-click a byte -> **Who wrote this byte?** Opens the
  window and shows the last writer and the write history of that byte.

Whether the window is open is not saved. Recording does not depend on
the window: it is controlled from the window, over MCP, by breakpoint
actions or from the command line.

## Recording

Top part of the window:

- **From** / **To** - range of CPU addresses (hex). Both empty = all
  memory. Only From = a single byte.
- **Mode** - what happens when the log is full: **Ring** keeps the
  newest records (the oldest are overwritten), **Fill** keeps the first
  records and only counts the following writes as dropped.
- **Records** - size of the log (16-16777216 records, 32 bytes each,
  default 1048576 = 32 MB).
- **Start** starts recording (a running log is replaced), **Stop** stops
  it (the records stay readable), **Clear** empties the log.

The status line shows whether the log records, how many records it holds
and how many writes were overwritten (Ring) or dropped (Fill), the memory
used and the range.

A reset of the computer and loading a snapshot empty the log; a running
recording continues.

Recording all memory slows the emulation down more than recording a
small range. On the MZ-800 only the writes into the 4 KiB pages that
contain the range leave the fast memory path of the emulator; the other
writes run as without the log. Measured on the MZ-800 in MAX SPEED with
two programs: one byte or one 4 KiB page cost about 4-12 % of the
emulation speed, all memory about 11-20 %; a MEM_W breakpoint with a
`continue` action on the same byte cost about 17-27 %. Without the log
the emulation speed does not change.

## Who wrote

Enter a CPU address and press **Find** (or use **Who wrote this byte?**
in the Memory Browser). For every memory cell at that address the window
shows:

- the **last writer** - address of the writing instruction and the
  source. It is kept for every cell even when the record of that write
  was already overwritten in the ring;
- the **write history** of the cell from the newest write (up to 100
  writes), as long as the records are still in the log.

**Include unchanged writes** also shows writes that wrote the value the
cell already had.

The address is resolved with the memory mapping at the moment of the
query: the same CPU address can be RAM at one moment and VRAM at another
(banking), those are different cells with their own history. On the
MZ-800 the address of the graphic VRAM gives all four planes. A Memory
Browser region other than Logical (RAM, MemExt, VRAM, CG-RAM, PCG) asks
for exactly the cell of that region.

## Newest writes

The table at the bottom shows the last 200 writes, refreshed twice per
second while **Auto refresh** is on. **Show unchanged writes** also shows
writes that did not change the cell.

## Columns

| Column | Meaning |
|--------|---------|
| Seq | Record number since the log was started or emptied |
| Frame | Emulated frame |
| PC | Address of the instruction that wrote. For an interrupt (source Interrupt / NMI) the return address that the CPU pushed. `-` for writes without an instruction (Debugger, Freeze, Tape, Load) |
| Address | CPU address of the write, `-` when the write had none (e.g. a write into a Memory Browser region) |
| Cell | Memory and offset: RAM, MemExt (with a connected MemExt the RAM pages are MemExt banks), VRAM (MZ-800 graphic VRAM: plane x 2000h + offset, with the plane number), PCG (MZ-1500), ROM |
| Old / New | Value before and after the write |
| Source | Instruction, Interrupt (return address), NMI (return address), Debugger (Memory Browser, MCP memory write), Breakpoint poke, Freeze, Tape (CMT hack), Load |
| Note | `N bytes loaded` = one summary record of a loaded block; `dropped (ROM)` = a write under ROM that did not change memory; `value unchanged` |

Notes:

- `LDIR`, `PUSH` and similar instructions give one record per written
  byte, all with the address of the instruction. The two bytes of `PUSH`
  and of the return address pushed when an interrupt is accepted are
  recorded in the order they are written: high byte first, then low
  byte (as in the Zilog Z80 manual; not measured on a real MZ-800).
- On the MZ-800 a write into the graphic VRAM gives one record per plane
  the write mode actually writes (for example REPLACE also writes 00 into
  the planes that are not selected).
- Loading a program (`--run-mzf`, tape with the CMT hack, a loaded
  memory block) is recorded as one summary record per block and 4 KiB
  page, not byte by byte.

## Command line and configuration

| Option | INI `[RAMLOG]` | Meaning |
|--------|----------------|---------|
| `--ramlog-mode <off\|window\|always>` | `mode = OFF / WITH_WINDOW / ALWAYS` | Start recording from start (`always`), while the debugger window is open (`window`), or not at all (`off`, default) |
| `--ramlog-ring <ring\|fill>` | `ring = RING / FILL` | Mode of the log when full |
| `--ramlog-capacity <N>` | `capacity` | Size in records |
| `--ramlog-lo <addr>` | `range_lo` | First CPU address |
| `--ramlog-hi <addr>` | `range_hi` | Last CPU address (`0` + `0xFFFF` = all memory) |

Numbers in the INI file are hexadecimal (with or without the `0x`
prefix), for example `capacity = 0x100000`, `range_lo = 0x4000`. On the
command line decimal and `0x` hex are accepted.

A log started from the configuration is stopped by the configuration only
(`window` mode: when the debugger window closes); after Stop in the
window or over MCP it is not started again automatically.

## Breakpoint actions

- `ramlog_start` - record all memory; `ramlog_start <from>, <to>` - record
  a range of CPU addresses. The expressions are evaluated when the
  breakpoint hits, e.g. `ramlog_start HL, HL + 15`.
- `ramlog_stop`, `ramlog_clear`.
- Expression function `lastwriter(addr)` - address of the instruction
  that last wrote the cell at `addr` (current mapping), `-1` if unknown.
  Example condition: `lastwriter(0x4000) != 0x3009`.

See [Action DSL](breakpoints/action-dsl.md) and
[Expression syntax](breakpoints/expression-syntax.md).

## MCP

`emu_ramlog_start`, `emu_ramlog_stop`, `emu_ramlog_clear`,
`emu_ramlog_status`, `emu_ramlog_who_wrote`, `emu_ramlog_writers` and the
`ramlog` channel of `emu_trace_read`. See
[MCP tools](../mcp-server/tools-overview.md) and the MCP document
`emulator://docs/ramlog`.
