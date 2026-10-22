# Live Test Eval

Live Test Eval is a UI helper in the Edit BP dialog that lets you
evaluate any condition expression against the **current emu state**
**without having to create and save a BP**. It is used to debug
expressions before they are finally written into `condition` or
`action`.

## UI

The "Live Eval Test (optional)" section is the last block in the Edit
BP panel, below the BP Options / Name / Trigger logic / Action on
trigger sections.

Components:

- **Textbox** - full-width input for the expression.
- **Button "Test Eval"** - 100 pt wide, right-aligned, on its own row
  below the textbox.
- **Result** - text below the button. Format `= <dec> (0x<hex>)` for
  success, `ERR: <message>` for a syntax/eval error.

The textbox is not tied to the condition or action field - it is an
independent scratch buffer per Edit panel session. The value is not
stored anywhere (not into the BP, not into persistence).

## Context population

After pressing Test Eval the expression context is filled from the
live emu state:

- **CPU registers** - the current contents of the Z80 CPU.
- **Cycle** = cumulative GDG pixel ticks (not the Z80 T-state counter).
- **Frame** = number of complete frames.
- **Scanline** = current raster row of the beam.
- **BankPC**, **BankAddr** = current banking zone for the PC address.

Fields that depend on the BP hit context (Address / Value / IsRead /
IsWrite / IsExec / IsPort) stay at 0 / false (= the caller usually
fills them per-type via the enforcement layer, but Test Eval has no
hook context).

### What Test Eval fills = works

- **CPU registers** - `A`, `B`, `C`, `D`, `E`, `H`, `L`, `BC`, `DE`,
  `HL`, `SP`, `PC`, `IX`, `IY`.
- **Z80 flags** `Cf`, `Zf`, `Sf`, `Pf`, `Hf`, `Nf` - bit decomposition
  from F.
- **Shadow registers** `AF'`, `BC'`, `DE'`, `HL'`.
- **`I`, `R`, `IM`, `IFF1`, `IFF2`**.
- **`PC`, `SP`** - the main special registers.
- **`BankPC`, `BankAddr`** - banking zone for the current PC.
- **Memory deref** `[addr]`, `{addr}` - reads through live emu memory
  (through the current banking).
- **`$vars`** - global storage (persistent, shared across all BPs).
- **Built-in functions** `min`, `max`, `abs`, `if`, `bit`, `s8`, `s16`,
  `s32`.
- **`Cycle`, `Frame`, `Scanline`** - from GDG state.

### What Test Eval does NOT fill = always 0 / false

- **`Address`, `Value`** - without a BP hit context both are 0.
  Expressions `Address` / `Value` always return 0 in Test Eval.
- **`IsRead`, `IsWrite`, `IsExec`, `IsPort`** - all false. Expressions
  like `IsWrite && Value == 0x42` are always false in Test Eval.
- **`self_id`** - explicitly -1, not the ID of any BP (not even the
  one being edited).

The I/O port read `port[N]` works as everywhere else: a probe without
side effects (see `expression-syntax.md`: for most chips it returns a
cached chip state or the bus latch instead of the value an `IN` would
read).

## When the expression is evaluated

The emulator itself evaluates the expression, not the window. All
identifiers of one expression come from the same moment:

- **Paused emulation** - evaluated right away (within about 20 ms), the
  result is the state the debugger shows.
- **Running emulation** - evaluated at the end of the current frame. The
  result is the state at that moment, which keeps changing; it is fine
  for "right now A is 0x42", not for precise debugging.

If the emulator does not answer within one second, the result is
`ERR: The emulator did not respond in time.`

Recommendation: use Test Eval **while the debugger is paused** (=
classic workflow before pressing Run).

## Debugging examples

### Test a CPU register

```
A == 0x42
```

Returns 1 if the current A register of the emu is 0x42, otherwise 0.

```
HL & 0xFF00 == 0x4000
```

Tests the upper byte of HL.

### Memory probe

```
[0xE000]
```

Reads a byte from memory at address 0xE000 (through the current
banking) and returns the value.

```
{0xC000}
```

16-bit read from 0xC000 (low byte at 0xC000, high byte at 0xC001 -
little endian).

### Banking-aware

```
BankPC == 1
```

Tests whether the current PC is in zone ROM_LOWER (= zone value 1).
Useful to detect "code in the ROM monitor".

### Built-in functions

```
if(A < 10, 1, 0)
```

Returns 1 if A < 10, otherwise 0.

```
bit(F, 6)
```

Extracts the Z flag bit (= flag bit position 6 in the F register,
equivalent to `Zf`).

### $vars

```
$myCounter
```

Reads a user var (= it must have been previously set by an action
`set $myCounter = ...` in some BP, manually via the UI panel
`Variables` (Debugger -> Variables), or via the programmatic API).

```
$myCounter > 100
```

Tests a threshold. A non-existent name -> 0 (= no error). Detail in
`vars.md`.

## What CANNOT be tested

These expressions are **syntactically valid** in Test Eval but do not
give a meaningful result:

- **`Address`, `Value`** - always 0 (= no BP hit ctx).
- **`IsRead`, `IsWrite`, `IsExec`, `IsPort`** - always false.
- **`Cycle`** - returns GDG pixel ticks (= not pure Z80 T-state).
  For per-frame relative timing it is OK.
- **Side-effect memory read** `[addr]!` - parsed, but the `!` flag is
  ignored in Test Eval (= always a regular memory read without
  side-effect: MEM_R breakpoints on the address do not fire, see
  `expression-syntax.md`).
- **Side-effect port read** `port[N]!` - ditto: read as the probe
  `port[N]`, the chip state does not change.
- **Hit count / skip count semantics** - per-BP runtime state, the
  expression has no access to it.

To test conditions that depend on the hit ctx (= "what happens on
MEM_W 0xE000 with value 0x42") you must actually create the BP and
observe its fire (= action `log "%X", Value`).

## Related documents

- `expression-syntax.md` - complete grammar of condition expressions
  (= what can be tested in Test Eval)
- `action-dsl.md` - action DSL syntax (= Test Eval evaluates **only**
  the expression, not the action; there is no analog for the action)
- `types.md` - per-type context (= what BP enforce fills, in contrast
  to Test Eval)
- `README.md` - subsystem overview
