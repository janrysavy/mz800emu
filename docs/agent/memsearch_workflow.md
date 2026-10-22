# Find a variable (memory search, emu_memsearch_*)

Recipe for an AI client that needs to find where a program keeps a value
(lives, score, a timer, a position) without disassembling it. Use
`emu_memsearch_start`, `emu_memsearch_filter`, `emu_memsearch_list`,
then act on the result with `emu_watch_add`, `emu_bp_create_with_init`,
`emu_freeze_add` or `emu_ramlog_who_wrote`.

The search narrows a set of candidate positions: every filter keeps only
the positions whose value changed the way you expect. Samples are taken
by the emulator thread at a frame boundary (at once when paused), so a
sample is consistent for the whole space and the same input sequence
gives the same candidates.

## Recipe

1. Bring the program to a state where the value is known or about to
   change (`emu_media_run_mzf`, `emu_input_send_keys`, `emu_run`).
2. `emu_memsearch_start(space="ram", type="u8")` - every position of User
   RAM is a candidate. Add `op="eq", value=3` when the value is known
   (3 lives).
3. Change the value in the program (`emu_run(frames=50)`, keys).
4. `emu_memsearch_filter(session_id, op="dec_by", value=1)` (lost a life),
   `op="dec"`, `op="changed"`, `op="eq", value=N`, ...
5. Repeat 3-4 until `candidates` is small, then
   `emu_memsearch_list(session_id)`.
6. Act on an item:
   - `emu_watch_add(addr=item["z80_addr"])` - live value;
   - `emu_bp_create_with_init(addr=hint["addr"], fields=hint["fields"],
     values=hint["values"])` with `hint = item["bp_hint"]` - a MEM_W
     breakpoint in the right zone; it stops in the code that changes the
     value;
   - `emu_freeze_add(kind=item["kind"], sub_id=item["sub_id"],
     offset=item["region_offset"], bytes=[...])` - keep the value
     (infinite lives);
   - `emu_ramlog_who_wrote(space=..., offset=...)` - who wrote it, without
     stopping (see `emulator://docs/ramlog`).
7. `emu_memsearch_close(session_id)`.

`emu_memsearch_undo` returns the previous step including the reference
samples; use it when a filter removed too much.

## Choosing the filter

| Situation | Filter |
|-----------|--------|
| Known value | `op="eq", value=N` (also as the first filter of `emu_memsearch_start`) |
| Lost one life / used one item | `op="dec_by", value=1` |
| Score grew by a known amount | `op="inc_by", value=N` |
| Value unknown, it went down | `op="dec"` |
| A timer or counter that ticks irregularly | every round `op="le", compare_to="previous"`, then `op="lt", compare_to="first", sample=False` |
| Nothing changed in this round | `op="unchanged"` (removes noise) |
| Flag bit | `op="mask_eq", mask=0x08, value=0x08` |

Strict `dec` / `inc` per round removes a counter that did not change in
that round (a game timer can stand still for a moment; measured on a
MZ-700 game: sampled every 50 frames, the timer did not move in 2 of 11
intervals).
The `le` + `lt first` pair keeps it and still removes constants.

Arithmetic has no wrap-around: an u8 going 00h -> FFh is not `dec_by 1`.

## Types and spaces

- `type`: `u8`, `s8`, `u16`, `s16`, `u24`, `s24`, `u32`, `s32` (Z80 is
  little-endian, `endian="le"` default), `bcd` (packed, `width` 1-4,
  a nibble above 9 drops the position) or `digits` (one digit per byte,
  `width` 1-8, `digit_encoding` `raw` / `ascii` / `display`). A score
  printed on the text screen is `digits` with the display code
  (20h-29h, space 00h; see `emulator://docs/sharp_display_code`) in
  `space="vram"`; a write breakpoint there finds the print routine.
- `space`: `ram` (default), `logical`, `vram`, `memext`, `ramdisk`,
  `all_writable` or a list of regions (`emu_regions_list` kinds).
  With a MemExt (connected in the default setup) User RAM shows the banks
  mapped at sample time; if the program switches banks, search
  `space="memext"`.
- A space over 4 MiB needs `allow_large=True` (up to 32 MiB).

## Things to know

- `z80_addr` is null when the cell is not visible to the CPU (VRAM plane,
  CG-RAM, PCG, RAM disk, unmapped MemExt part); `bp_hint` is null where a
  write breakpoint cannot match the cell. For a PEHU MemExt bank the hint
  uses zone `MMEXT_BANK` with `bp_addr_space="bank_offset"`: it fires
  whichever CPU window the bank is in.
- Freeze writes the value back once per frame. The program sees its own
  change until the end of the frame: a game that decrements lives and
  tests them in the same frame still reacts.
- `sample=False` in `emu_memsearch_filter` compares the last sample with
  the one before it (after `emu_memsearch_sample`) - no new sample.
- Candidates of a region that got disconnected stay unchanged
  (`connected: false` in the list).
- Sessions are separate from the GUI Memory Search window; at most 8 are
  open.
