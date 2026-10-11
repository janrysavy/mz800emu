# Passive fetched-dispatch coverage (experimental)

`coverage.detail.start`, `.stop`, `.reset`, `.status` and `.read` are independent
of the existing logical-PC bitmap. The CPU owner thread executes each command.
All except status require an existing pause. No command implicitly repairs
guest state or requests a pause.

Start accepts `reset` (boolean, default true) and `capacity` (power of two,
256..1048576; initially 131072). Resizing requires reset. Read accepts `offset`
(default 0) and `limit` (1..1024, default 256). Other fields are rejected.
Pagination requires the same pause; `next_offset` advances through insertion IDs.

Each exact aggregate key contains logical dispatch PC, actual fetched bytes,
M1 mask, explicit flags, the actual conditional/repeat decision (0/1, or -1
when none was recorded), and completion PC before interrupt processing. Rows
retain 64-bit execution counts and first/last dispatch indices. Values come
from the existing reads; the collector never rereads guest memory.

Flag 1 identifies an ignored DD/FD prefix fragment under this core's existing
redispatch convention. Such a unit records its actual two fetches and next PC;
it is not labelled a complete decoded instruction. HALT refresh units are
excluded, consistent with the independent PC bitmap. CPU reset retains the
collector; explicit coverage reset clears its evidence.

Capacity exhaustion, an oversized/nonsequential fetch or a conditional unit
with no decision hook increments explicit failure counters and dropped units.
`complete` is false after any loss. Known keys can still count after capacity
exhaustion. Hash collisions compare every key field and byte; they never merge
different evidence. Memory use is bounded by the selected capacity, and disabled
collection allocates no table.

This collects logical-address byte versions. It does not identify identical
bytes in different physical mappings: `physical_mapping_certified` is false.
It does not establish a reachable denominator, global path coverage or game
qualification. Whole-emulator clocks/state/pixels/audio and host overhead need
separate enabled/disabled qualification before integration.

Standalone synthetic controls build against `src/libs/cpu-z80/z80.c` with
`tests/libs/cpu-z80/test_instruction_coverage.c`. With
`MZ800EMU_CFG_RAM_FASTPATH` defined they exercise 316 enabled/disabled single-step
and batch cases through both RAM paths, including every base conditional and
block-repeat predicate. They compare clocks, CPU state, RAM, actual callback
events and the old PC map, and check prefix fragments, HALT refresh, reset and
self-modified immediate versions. These controls are separate from gameplay.
