# Snapshot do paměti / z paměti

Vedle standardního ukládání snapshotu do souboru `.mzs` umí emulátor
pracovat se snapshotem přímo v **paměťovém bufferu** - bez nutnosti
zapisovat data na disk.

## K čemu to slouží

Buffer varianta snapshot API je vnitřní stavební blok pro vyšší vrstvy
emulátoru. Sám uživatel ji přímo neovládá, ale využívají ji:

- **MCP server** - AI klient (Claude Code, Claude Desktop, vlastní
  LLM aplikace) může emulátoru poslat snapshot jako součást zprávy
  (base64-encoded payload), nebo si od emulátoru snapshot vyžádat zpět
  k uložení ve své vlastní paměti / databázi. Funguje i v sandbox
  profilu, kde MCP server nemá přístup k filesystému.
- **Krok zpět během debug session** - debugger uchovává kruhový buffer
  snapshotů v paměti, aby bylo možné kdykoliv vrátit emulaci o N kroků
  zpět. Disk I/O by byl příliš pomalý pro vysokofrekvenční snapshotting.
- **CI / batch test** - regression testy snapshot systému bez vedlejších
  efektů na disku.

## Kompatibilita s `.mzs` souborem

Data v bufferu mají **identický formát** jako `.mzs` soubor (= ZIP
archiv s `manifest.xml` + binární data + screenshot). To znamená:

- Snapshot vytvořený v paměti lze kdykoliv zapsat na disk a otevřít
  klasicky.
- Soubor `.mzs` lze načíst do paměti (např. po síti, z databáze) a
  použít přímo, bez ukládání na disk.

Z pohledu kompatibility se snapshot v paměti chová **identicky** se
snapshotem v souboru - stejný checksum, stejné metadata, stejné
ověřování architektury, stejná podpora obou architektur MZ-800/700/1500.

Soubory s pamětí uvnitř archivu (`memory/ram.bin`, `memory/vram.bin`,
`memory/memext_ram.bin`, ...) popisuje `emulator://docs/memory_layout`.
Pozor: se zapojeným Memextem je RAM, kterou vidí CPU, v
`memory/memext_ram.bin`, ne v `memory/ram.bin`.

## Omezení

- Maximální velikost vstupu pro načtení z paměti je 2 GB. Reálné
  snapshoty MZ-800 mají desítky MB, takže to není praktický limit.
- Stejně jako u file API musí být emulátor **v pauze** pro save i load
  operaci.

## Související dokumentace

- [Headless režim](headless-mode.md) - typicky se používá společně s
  buffer API v MCP / CI prostředí
- [README MCP serveru](README.md) - kontext použití snapshot bufferu
  v MCP workflow
