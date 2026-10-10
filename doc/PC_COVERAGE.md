# Passive logical-PC coverage

Commands use the existing JSONL envelope (`cmd`, `data`) and emulation-thread
debugger queue. Coverage occupies 8,192 host bytes per CPU instance, with small
host counters. It consumes no guest RAM and is off by default.

| Command | Parameters | Effect |
| --- | --- | --- |
| `coverage.start` | `mode: "pc"` optional; `reset: true` default | Enable; optionally clear prior coverage |
| `coverage.stop` | none | Disable, retain data |
| `coverage.reset` | none | Clear data, retain enabled state |
| `coverage.status` | none | Return counters and live cycle observation |
| `coverage.read` | none | Return counters and `bitmap_base64` |

All commands except status require an already paused emulator. They neither
advance execution nor pause it implicitly. Results use mode `pc`, format
`pc-bitset-lsb0`, `map_bytes: 8192`, `enabled`, `unique_pcs`, `dispatch_units`,
`clock_cycles` and `clock_bits: 32`. The last two describe the existing wrapping
CPU cycle counter; coverage never resets or repairs it. Bit `(address & 7)` of
byte `(address >> 3)` marks an executed logical dispatch address.

The hook is immediately before the core's primary opcode fetch in both batch
and per-step execution. Consumed prefix/operand bytes and HALT refresh cycles
do not get independent marks. A breakpoint observed before execution is not a
hit. Block-instruction repeats mark the same start repeatedly. Ignored prefixes
that the existing core dispatches separately retain that core behavior.

No additional guest bus access, instruction cycle, register change, debugger
activation or trace recording is introduced. Reset/load retains this diagnostic
map; call `coverage.reset` to begin a new interval. CPU destruction ends it.
The owner thread serializes every read/change. Maps intentionally merge banks
and self-modified opcode versions at one logical PC; this is address coverage,
not branch, value or full-game coverage.
