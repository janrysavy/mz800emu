# Memory Search - find a variable

Memory Search finds where a program keeps a value - lives, score, a
timer, a position - by narrowing a set of candidate addresses: take a
sample of memory, let the value change in the program, keep only the
positions that changed the same way, repeat. What is left can be
watched, frozen, guarded by a write breakpoint or opened in the Memory
Browser.

Available on MZ-800, MZ-700 and MZ-1500, in the GUI and over MCP (see
[MCP tools](../mcp-server/tools-overview.md), section Memory search).

## Opening the window

- Main menu `Debugger` -> `Memory Search`, or **Alt+Shift+E**.

Whether the window is open is not saved; its settings (space, type,
byte order, ...) are saved in the `[MEMSEARCH_WINDOW]` section of the
configuration. Candidates are not saved.

## Where to search

**Space** chooses the memory:

| Space | Contents |
|-------|----------|
| User RAM (default) | 64 KB of RAM as the CPU reads it, regardless of what is mapped over it (ROM, VRAM). With a MemExt it is the banks mapped at the moment of the sample |
| Logical Z80 | What the CPU sees now, including ROM, VRAM and the memory-mapped ports at 0E000h (their values come from the last update of the emulation) |
| VRAM | Text and attribute VRAM of the MZ-700 mode and the four VRAM planes of the MZ-800 |
| MemExt banks | All RAM banks of the memory expansion |
| RAM disks | Banks of the RAM disk and of the PEZIK RAM disks |
| All writable | RAM, VRAM, CG-RAM, PCG, MemExt RAM and RAM disks together |

ROM, CG-ROM and FLASH are never searched. Only connected devices are
included.

If a program switches MemExt banks, User RAM shows different memory at
different samples and the search can lose the variable. Search the
**MemExt banks** instead.

A space over 4 MiB (a 16 MB RAM disk) needs **Allow large space** (up to
32 MiB): the search keeps three samples of the space plus the undo
history in memory.

## What to search for

**Type**:

- `u8`, `s8`, `u16`, `s16`, `u24`, `s24`, `u32`, `s32` - integers without
  / with sign. **Byte order** LE (Z80 order, low byte first) or BE.
- `bcd` - packed BCD, two digits per byte, 1-4 bytes (**Bytes**). A byte
  with a nibble above 9 is not BCD and the position drops out.
- `digits` - one decimal digit per byte, 1-8 bytes. **Digits** chooses
  the code: raw 00h-09h, ASCII 30h-39h or the Sharp MZ display code
  20h-29h (what is written into the text VRAM). With **Leading spaces as
  zeros** a space before the first digit counts as 0 (ASCII 20h, display
  code 00h). BE = the highest digit first, as it is printed.

**Aligned** keeps only positions divisible by the width. A value never
crosses the end of a region.

A score printed on the screen is often easier to find as `digits` in the
VRAM (display code) than in the RAM; a write breakpoint on that screen
position then finds the routine that prints it, and the routine leads to
the variable.

## Searching

1. **New search** takes a sample; every position is a candidate.
2. Let the value change in the program (lose a life, wait for the timer).
3. Choose the filter and press **Apply filter**. With **Take new sample**
   on (default) a new sample is taken and compared; the candidates that
   do not match are removed.
4. Repeat steps 2-3 until a few candidates remain.

While the emulation runs, a sample is taken at the next frame; when it
is paused, at once. The line under the buttons shows the number of
candidates, the step and the frame of the last sample.

**Filters** (**compared with** value, previous or first):

| Filter | The candidate stays when its value |
|--------|------------------------------------|
| equal to / not equal to / less than / less or equal / greater than / greater or equal | compares so with the reference |
| between | lies between **Value** and **and** (inclusive) |
| increased / decreased | grew / dropped against the previous or the first sample |
| increased by / decreased by / changed by | changed by exactly **Difference** |
| changed / unchanged | changed / did not change |
| bits under mask equal | has the bits under **Mask** equal to the reference (integer types) |

- **previous** = the sample before the compared one, **first** = the
  sample of New search.
- **Not** inverts the filter. An invalid BCD or digit value is removed
  anyway.
- Arithmetic has no wrap-around: an u8 going 00h -> FFh is not
  "decreased by 1".
- **Sample** takes a new sample without filtering. Then filter with
  **Take new sample** off: the last sample is compared with the one
  before it.
- **Preview** marks red the candidates the current filter would remove,
  using the last sample (no new sample).
- **Undo** / **Redo** return the candidates together with the reference
  samples: a filter after Undo compares with the right previous value.

A counter in a game does not have to change between every two samples
(a timer may stand still for a moment). Strict **decreased** then
removes it. More robust: **less or equal** than **previous** in every
step and **less than** than **first** (Take new sample off) to remove
the values that never changed.

Candidates in a region that was disconnected at the sample (MemExt or a
RAM disk removed during the search) stay unchanged and are shown as
disconnected.

## Candidates

The table is shown when fewer than 10000 candidates remain: region,
offset in the region, CPU address (where the cell is visible to the CPU
now), the bytes and the current, previous and first value.

Right-click a candidate:

- **Show in Memory Browser** - opens the Memory Browser on that byte.
- **Add to Watch** - adds the CPU address to the Watch window (BCD,
  digits and 24-bit values as bytes). Needs a CPU address.
- **Add write breakpoint** - a MEM_W breakpoint on the value: zone RAM
  for User RAM (it does not catch writes into ROM or VRAM at the same
  address), VRAM framebuffer for the MZ-700 text VRAM, the PEHU bank and
  the offset inside the bank for a PEHU MemExt bank (it fires whichever
  CPU window the bank is mapped into), the CPU address for a Luftner bank
  mapped now. Not available for a VRAM plane, CG-RAM, PCG and RAM disks.
- **Freeze at current value** - freezes the bytes of the value (Freeze
  Bytes, see the [Memory Browser](memory-browser/README.md)). The value
  is written back once per frame: the program sees its own change until
  the end of the frame, so a game that decrements lives and tests them in
  the same frame still notices.
- **Who wrote this byte?** - opens the [RAM Write Log](ram-write-log.md)
  for the cell.
- **Copy address**, **Eliminate** (remove the candidate by hand).

## Related

- [Memory Browser](memory-browser/README.md) - hex view of all regions,
  one-shot pattern search, Freeze Bytes.
- [Watch](watch.md) - live values of variables.
- [RAM Write Log](ram-write-log.md) - who wrote a byte.
