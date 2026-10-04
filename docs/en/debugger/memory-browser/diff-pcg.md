# Memory Browser - Memory Diff + PCG glyph editor

Two standalone windows built on top of the Memory Browser core.
**Memory Diff** compares two memory snapshots side-by-side and
highlights the differences. **PCG glyph editor** draws 8x8 bitmap
characters into the PCG RAM on the MZ-1500.

Both windows are independent of the main Memory Browser window
(singleton) but share its region and layer definitions (see
[layers-regions](layers-regions.md)).


## 1. Memory Diff (V5)

Window for comparing two snapshots of the same region. It highlights
bytes that differ between A and B and shows a summary statistic.

### 1.1 Opening

| Path | Action |
|---|---|
| **Menu** Debugger -> Memory Diff | Opens the window (singleton, second open just focuses) |

V5 has no dedicated keyboard shortcut - the window is opened
exclusively via the menu.

### 1.2 Layout

The top bar carries two source dropdowns (A and B), region selector,
scope selector and an **Auto-snapshot at pause** checkbox, plus the
**Take snapshot** and **Export TXT...** buttons. Below them two
side-by-side hex views (A on the left, B on the right) scroll
synchronously. Bytes that differ between A and B are highlighted in
magenta. The bottom row carries summary statistics.

### 1.3 Source A / Source B

Each of the two source dropdowns has three options:

| Option | Meaning |
|---|---|
| **Live** | Current emulator memory contents (read via the regions API on each refresh) |
| **Manual snap A** / **Manual snap B** | Named snapshot stored via the **Take snapshot** button |
| **Auto-snapshot at pause** | Snapshot taken automatically at the moment of pause (manual stop, breakpoint hit, step) |

Typical combinations:

- A = **Live**, B = **Manual snap A** - "what changed since the last
  manual snapshot"
- A = **Manual snap A**, B = **Manual snap B** - comparison of two
  different moments in time
- A = **Live**, B = **Auto-snapshot at pause** - "what changed since
  the last pause"

### 1.4 Region and Scope

- **Region** - dropdown over all registered regions (Logical Z80,
  Physical RAM, VRAM, PCG bank, etc.). The snapshot holds a byte
  array for the selected region; changing the region invalidates
  manual snapshots (new size = new buffer).
- **Scope** - `whole region` compares the entire region; `range`
  lets you specify a start/end address (useful for large regions
  where only a slice is of interest).

### 1.5 Take snapshot

The **Take snapshot** button captures the current Live contents of
the chosen region and stores it as **Manual snap A** or **Manual snap
B** depending on which slot is active in the dropdowns. The previous
contents of that slot are overwritten without confirmation.

### 1.6 Auto-snapshot at pause

Toolbar checkbox. When enabled, **every** emulator pause (manual stop,
breakpoint hit, single step) automatically takes a "before pause"
snapshot of the current region. Workflow:

1. Enable **Auto-snapshot at pause**.
2. Resume the emulator, perform an in-game action (e.g. pick up an
   item).
3. Stop the emu (manual or BP hit).
4. In Memory Diff you see B = "state before pause", A = "state after
   pause" - magenta highlights what the action changed.

### 1.7 Export TXT

The **Export TXT...** button opens an IGFD dialog to save a text
report. Format:

```
Memory Diff: Logical Z80, A=Live B=Snap @ 2026-05-25 14:32:10
Changed: 12 of 65536 B (0.02%)

0xC010: 50 -> AA
0xC012: 13 -> 99
0xC014: 00 -> 01
...
```

Suitable as an attachment to a bug report or for an audit log.

### 1.8 Statistic

Bottom row of the window:

- **Changed: X of Y B (Z%)** - count of differing bytes within scope.
  When Z = 0, **All sources match** is shown.

### 1.9 Use cases

| Scenario | Steps |
|---|---|
| Find where the game holds state (HP, score, position) | Snapshot, perform an in-game action, diff -> magenta bytes are candidates |
| Verify that a cheat / freeze holds | Snapshot the HP byte, let the player take damage, diff -> value unchanged = freeze works |
| Compare two save states | Load `.mzs` A, snapshot, load `.mzs` B, A = Live, B = snap -> differences between save points |


## 2. PCG glyph editor (V6, MZ-1500 only)

Window for editing 8x8 bitmap characters stored in **PCG**
(Programmable Character Generator) RAM. MZ-1500 specific feature -
3 banks x 1024 characters x 8 B per character.

### 2.1 Opening

| Path | Action |
|---|---|
| **Memory Browser** -> region PCG bank 1/2/3 -> cursor on a byte -> right mouse button -> **Open in PCG editor...** | Opens the editor and focuses on the character containing the cursor (= addr / 8) |
| **Menu** Debugger -> PCG Editor | Opens the editor on the last selected character |

### 2.2 Layout

The top bar carries the **Bank** dropdown (1/2/3), hex input **Char**
for the character index, navigation buttons **[<] [>]** and the
character indication (index + ASCII representation). Below that an
8x8 grid of clickable cells (= one glyph). Below the grid the **HEX**
row shows the current 8 bytes of the glyph and a button panel
**Inverse / Mirror H / Mirror V / Rotate 90 deg**. The bottom bar has
**Save / Reload / Close**.

### 2.3 Bank selector

The **Bank** dropdown switches between the three MZ-1500 PCG banks
(sub_id 0/1/2). Each bank is 8 KB = 1024 characters x 8 B (one
character = one row of 8 bits per byte, 8 bytes total per glyph).

### 2.4 Character navigation

- **Char #** - number input for direct index entry (0-1023).
- **[<] [>]** - prev/next character (jump by 8 B within the PCG
  bank). Stops at characters 0 and 1023 (no wraparound).
- Below the grid the editor shows the line **Raw:** with the 8 bytes
  of the current character in hex and the line **Bank addr range:**
  with the byte range of the character inside the bank
  (`char_idx * 8` .. `char_idx * 8 + 7`).

### 2.5 Drawing

- **Left click** on a cell in the 8x8 grid - toggle a bit (0/1, i.e.
  bg/fg pixel).
- The **HEX** row under the grid shows the current 8 bytes of the
  glyph (= bytewise representation, one byte = one row of the bitmap).

### 2.6 Whole-glyph operations

| Button | Effect |
|---|---|
| **Inverse** | Flip all 64 bits (= negative of the glyph) |
| **Mirror H** | Horizontal mirror (= reverse the bits within each of the 8 bytes) |
| **Mirror V** | Vertical mirror (= reverse the order of the 8 bytes) |
| **Rotate 90 deg** | 90 degree rotation (= matrix transpose + mirror) |

The operations work on an in-progress (uncommitted) copy in the
editor, not directly on PCG RAM. The write into memory only takes
effect via **Save**.

### 2.7 Save / Reload / Close

- **Save** - writes the 8 B into the PCG bank at address `char_idx * 8`.
  The change is immediately reflected in the emulator video output
  (if the glyph is displayed).
- **Reload** - discards uncommitted edits and reloads the current
  contents from PCG RAM back into the editor.
- **Close** - closes the window. If there are unsaved changes, the
  editor asks for confirmation (= un-discarded edits would be lost).

### 2.8 Per-architecture availability

| MZARCH | Behavior |
|---|---|
| **MZ-1500** | Fully functional (PCG is the central feature of the MZ-1500 video subsystem) |
| **MZ-800** | The window can be opened, but shows the message "PCG not available on this arch" |
| **MZ-700** | Same as MZ-800 - no PCG hardware support |

### 2.9 Use case: custom font glyph

1. Open Memory Browser, switch to region **PCG bank 1**.
2. Find a free slot (e.g. char 0x90 where the ROM has a gap or an
   unused character).
3. Right mouse button -> **Open in PCG editor**.
4. In the editor click on cells - draw the desired glyph.
5. **Save** writes the 8 B into PCG RAM.
6. A game or application that prints a character with code 0x90 via
   VRAM will now see the new glyph.


## 3. Related

- [memory-browser](README.md) - main Memory Browser window
- [layers-regions](layers-regions.md) - region and layer definitions
  (required for region selection in Diff and navigation in PCG)
- [search](search.md) - searching for byte patterns (useful together
  with Diff to locate state variables)
