# Snapshot to / from memory buffer

In addition to saving snapshots into `.mzs` files, the emulator can
work with snapshots directly in an **in-memory buffer** - no disk I/O
required.

## What it is for

The buffer variant of the snapshot API is an internal building block
for higher emulator layers. Users do not interact with it directly, but
it is used by:

- **MCP server** - an AI client (Claude Code, Claude Desktop, a custom
  LLM app) can send the emulator a snapshot inline as part of a message
  (base64-encoded payload), or request a snapshot back from the emulator
  and store it in its own memory / database. Works even in a sandbox
  profile where the MCP server has no filesystem access.
- **Step-back during a debug session** - the debugger keeps a ring
  buffer of snapshots in memory so emulation can be rewound by N steps
  at any time. Disk I/O would be too slow for high-frequency
  snapshotting.
- **CI / batch test** - regression tests of the snapshot system without
  on-disk side effects.

## Compatibility with the `.mzs` file

The in-memory data uses the **exact same format** as the `.mzs` file
(= ZIP archive with `manifest.xml` + binary data + screenshot). This
means:

- A snapshot created in memory can be written to disk at any time and
  opened as a regular `.mzs` file.
- A `.mzs` file can be loaded into memory (e.g. over the network, from
  a database) and used directly, without writing it to disk first.

From a compatibility standpoint, an in-memory snapshot behaves
**identically** to a file snapshot - same checksum, same metadata, same
architecture verification, same support for MZ-800/700/1500.

The memory files inside the archive (`memory/ram.bin`, `memory/vram.bin`,
`memory/memext_ram.bin`, ...) are described in
`emulator://docs/memory_layout`. Note: with Memext connected, the RAM
the CPU sees is in `memory/memext_ram.bin`, not in `memory/ram.bin`.

## Limits

- Maximum input size for in-memory load is 2 GB. Real-world MZ-800
  snapshots are tens of MB, so this is not a practical limit.
- As with the file API, the emulator must be **paused** for both save
  and load.

## Related documentation

- [Headless mode](headless-mode.md) - typically used together with the
  buffer API in MCP / CI environments
- [MCP server README](README.md) - context of snapshot buffer use in
  the MCP workflow
