# Passive fetched-dispatch coverage v2 (experimental)

`coverage.detail.start`, `.stop`, `.reset`, `.status` and `.read` are independent
of the existing logical-PC bitmap. The CPU owner thread executes each command.
All except status require an existing pause. No command implicitly repairs
guest state or requests a pause.

Start accepts `reset` (boolean, default true) and `capacity` (power of two,
256..1048576; initially 131072). Resizing requires reset. Read accepts `offset`
(default 0) and `limit` (1..1024, default 256). Other fields are rejected.
Pagination requires the same pause; `next_offset` advances through insertion IDs.

Responses use `schema: 2`, `format: "z80-dispatch-fetched-v2"`. Preserve v1
evidence and validate it as v1; its untagged rows do not rule out host shortcuts.

Each exact aggregate key contains logical dispatch PC, actual fetched bytes,
M1 mask, explicit flags, semantic-kind bits, the actual conditional/repeat decision (0/1, or -1
when none was recorded), and completion PC before interrupt processing. Rows
retain execution counts and first/last dispatch indices up to `counter_limit`
(`INT64_MAX`). Values come
from the existing reads; the collector never rereads guest memory.

Flag 1 identifies an ignored DD/FD prefix fragment under this core's existing
redispatch convention. Such a unit records its actual two fetches and next PC;
it is not labelled a complete decoded instruction. HALT refresh units are
excluded, consistent with the independent PC bitmap. CPU reset retains the
collector; explicit coverage reset clears its evidence.

Flag 8 marks a known host semantic shortcut. `semantic_kinds` is a bitmask:
16 identifies the virtual CMT file/header service and 32 the body service.
The shared format reserves bits 1, 2, 4 and 8 for Fuse tape save, tape load,
SLT and loader acceleration. The `semantic_kind_units` object exposes the
corresponding six names (`fuse_tape_save`, `fuse_tape_load`, `fuse_slt`,
`fuse_loader_acceleration`, `mz_cmthack_file`, `mz_cmthack_body`). A unit may
carry multiple kinds; each kind counts once per unit. `semantic_override_units`
counts the units carrying any kind, so the sum of kind counts can exceed it.
These counters include tagged units lost for another reason, such as table
capacity. Only a complete capture reconciles kind counts directly to rows.

The CMT services can change AF/RAM even when loading fails or is cancelled.
Their actual fetched bytes and completion PC remain in tagged rows. Repeated
or nested hooks with the same kind do not duplicate a unit. Calls made outside
a pending CPU dispatch, including bootstrap and paused debugger loads, do not
create rows or override counts; describe those operations in capture provenance.

`contains_semantic_overrides` reports whether such units occurred.
`native_semantics_only` means complete capture with no known semantic overrides;
it does not certify hardware accuracy. Exclude rows with flag 8 from native
instruction/branch denominators. Known shortcuts may skip the normal predicate
(`outcome: -1`); if a predicate actually executes, preserve its 0/1 decision.
`complete` describes lossless dispatch observation, including these tagged
units, and preserves reconciliation with the independent PC collector.

Capacity exhaustion, an oversized/nonsequential fetch or a conditional unit
with no decision hook increments explicit failure counters and dropped units.
Unknown opcode substitution increments `opcode_override_failures`, separately
from capture read failures.
`complete` is false after any loss or `counter_overflow`. Checked counters do
not wrap; reaching the supported limit produces a sticky overflow indication
when another increment would exceed it. Further accounting freezes until reset;
do not require exact row/count reconciliation after overflow. Reset clears that
indication. Known keys can still count after capacity
exhaustion. Hash collisions compare every key field and byte; they never merge
different evidence. Memory use is bounded by the selected capacity, and disabled
collection initially allocates no table. Stop retains the existing table.

`clock_cycles` is the existing guest `total_cycles` counter modulo `2^32`,
advertised as `clock_bits: 32`. It wraps and resets on CPU reset even though
coverage rows persist. Coverage reset does not reset the guest clock. Compare
an independently accumulated replay clock modulo that width. Unwrapping
successive samples requires an explicit reset epoch and intervals shorter
than one wrap; a final sample alone cannot reveal how many wraps occurred.
The observer does not alter physical CPU/device clocks.

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

`instruction_coverage_cmthack` is a registered headless CTest component control
using the production CMT services and actual CPU OUT instructions. It compares
collection off/on for file-dialog cancellation, a missing body, successful body
loading, and a body read error. CPU registers, RAM, CPU cycles, GDG elapsed ticks,
port address/value/timing, and the old PC bitmap/counts must agree. A separate
synthetic dispatch envelope checks direct file-helper entry and repeated hooks;
the real bootstrap helpers outside a dispatch must leave coverage empty.
The API controls cover all six named kind counters, exact `INT64_MAX` JSON
parsing, explicit unknown-opcode loss, overflow, stop/reset retention, and an
actual CPU clock wrap. They do not exercise a GUI file chooser or certify audio
and native game behavior.

With the project dependencies and compiler on PATH, reproduce the headless
component controls in a fresh build directory:

```sh
cmake -S . -B build-coverage -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-coverage --target test_instruction_coverage_cmthack
ctest --test-dir build-coverage -R '^instruction_coverage_cmthack$' --output-on-failure
cc -std=c11 -O2 -Wall -Wextra -Werror -Isrc/libs/cpu-z80 tests/libs/cpu-z80/test_instruction_coverage_table.c -o build-coverage/table-c
c++ -x c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc/libs/cpu-z80 tests/libs/cpu-z80/test_instruction_coverage_table.c -o build-coverage/table-cpp
cc -std=gnu11 -O2 -DMZ800EMU_CFG_RAM_FASTPATH -Isrc/libs/cpu-z80 tests/libs/cpu-z80/test_instruction_coverage.c src/libs/cpu-z80/z80.c -o build-coverage/cpu-coverage
./build-coverage/table-c
./build-coverage/table-cpp
./build-coverage/cpu-coverage
```

These tests passed on 2026-10-11 with GCC/G++ 16.2.0 on Windows UCRT64.
The component fixture fixes its RAM expansion and MZ-800 mode explicitly, and
compares GDG elapsed increments because that lifetime clock survives CPU reset.
