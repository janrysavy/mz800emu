# Who wrote this byte? (RAM write log, emu_ramlog_*)

Recipe for an AI client that needs to know which code wrote a value into
memory, which code writes into an area, or how a variable changed over
time. Use `emu_ramlog_start`, `emu_ramlog_who_wrote`,
`emu_ramlog_writers` and `emu_trace_read(channel="ramlog")`.

This is cheaper and simpler than a MEM_W breakpoint with an action: the
log records every write in the range without stopping the CPU, and keeps
the last writer of every cell even after its record was overwritten.

## Recipe

1. `emu_ramlog_start(ranges=[{"from": 0x4000, "to": 0x40FF}])` - CPU
   addresses at the moment of the write. Keep the range small: an empty
   `ranges` records all memory and slows the emulation down more.
2. Run the section of interest (`emu_run`, a breakpoint, keystrokes).
3. `emu_ramlog_who_wrote(addr=0x4010)` - the last writer of the cell
   (`last.pc`, `last.src`) and its history, newest first.
4. `emu_ramlog_writers(from_addr=0x4000, to_addr=0x40FF)` - all
   instructions that wrote into the range, most writes first.
5. `emu_trace_read(channel="ramlog")` - all records in order (cursor,
   filters `pc_from`/`pc_to`, `addr_from`/`addr_to`, `space`, `src`,
   `t_from`/`t_to`).
6. `emu_ramlog_stop` when done; the records stay readable until the
   next start.

The same can be started from a breakpoint action
(`ramlog_start 0x4000, 0x40FF`, `ramlog_stop`, `ramlog_clear`, see
`emulator://docs/action_dsl`) or checked in a condition with
`lastwriter(addr)` (see `emulator://docs/bp_dsl`).

## Reading the records

| Field | Meaning |
|-------|---------|
| `t` | GDG ticks since emulation start at the **start of the writing instruction** - equal to `t` of the cputrack row of that instruction. Each repetition of `LDIR` / `LDDR` is a new instruction start |
| `tin` | T-state counter of the instruction core when it wrote (`insn` only, else `null`): an approximate position inside the instruction, e.g. `LD (nn),A` 13, the two `PUSH` bytes 7 and 10 |
| `frame` | Emulated frame |
| `pc` | Address of the instruction that wrote. For `src` `int_ack` / `nmi_ack` the return address the CPU pushed when it accepted the interrupt. `0000` for sources without an instruction |
| `addr` | CPU address of the write (`null` for a write into a memory region without a CPU address) |
| `space`, `offset` | The memory cell: `ram`, `memext` (with a connected MemExt the RAM pages are MemExt banks - the default setup), `vram`, `pcg` (MZ-1500), `rom` (write under ROM, memory not changed) |
| `plane` | MZ-800 graphic VRAM plane 0-3 (offset = plane x 2000h + position) |
| `old`, `new` | Value before and after |
| `src` | `insn`, `int_ack`, `nmi_ack`, `debugger` (MCP `emu_mem_write` / `emu_region_write`, Memory Browser), `poke` (breakpoint action), `freeze`, `cmt_hack`, `load` |
| `flags` | `unchanged` (same value written), `suppressed` (under ROM), `summary` (one record of a loaded block, `count` bytes) |

Rows follow the `columns` list of the response (always in the channel
order, a requested subset keeps that order).

`format="raw_b64"` returns 32-byte little-endian records
(`format_version` 1): `t` u64 @0, `frame` u32 @8, `pc` u16 @12, `addr`
u16 @14, `offset` u32 @16, `space` u8 @20 (0 ram, 1 vram, 2 pcg,
3 memext, 4 rom), `old` @21, `new` @22, `src` @23 (0 insn, 1 int_ack, 2 nmi_ack, 3 debugger, 4 poke, 5 freeze, 6 cmt_hack, 7 load), `tin` @24, `flags`
@25 (bits: 0 unchanged, 1 suppressed, 2 summary, 3 plane, 4 no_cpu,
5 no_map), `plane` @26, `count` u16 @28.

## Things to know

- A cell is physical memory, not a CPU address: the same CPU address
  can be RAM now and VRAM after a banking switch. `emu_ramlog_who_wrote`
  with `addr` uses the current mapping; ask with `space` + `offset` (as
  in the rows) for a specific cell.
- The MZ-800 graphic VRAM: one record per plane the write mode really
  writes (REPLACE writes 00 into the other planes too); `who_wrote` with
  an address in the mapped VRAM returns all 4 planes.
- Writes that did not change the cell and writes under ROM are recorded
  but hidden by default (`filter={"all": true}` in `emu_trace_read`,
  `include_unchanged=true` in the query tools). A loop that keeps
  writing the same value thus does not hide the write that changed it.
- `LDIR` / `PUSH` give one record per byte with the address of the
  instruction. `PUSH`, `CALL`, `RST` and the interrupt / NMI return
  address are written and recorded high byte first (SP-1), then low byte
  (SP-2), as in the Zilog Z80 manual; not measured on a real MZ-800.
  `LD (nn),rr` writes the low byte first.
- A program load (`emu_media_run_mzf`, cmt hack, `--run-mzf`) is one
  `summary` record per block and 4 KiB page.
- `ring` (default) keeps the newest records; `first_seq` > 0 in the
  status = older ones were overwritten, `last` still knows the writer
  (`in_ring: false`). `fill` keeps the first records and counts the rest
  in `dropped`.
- Reset and `emu_snapshot_load` empty the log (new segment); a cursor
  from before returns `segment_replaced:`.
