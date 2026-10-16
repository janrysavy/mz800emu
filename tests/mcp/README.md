# tests/mcp/ - MCP test suite

Unity testy pro MCP backend (`src/emulator/mcp/`).

## Aktuální stav (V0.A.2)

| Test | Typ | Popis |
|------|-----|-------|
| `test_mcp_jsonl_parser` | STANDALONE | JSONL transport core (parser, encoder, stream I/O) |

Pokrytí `test_mcp_jsonl_parser`:
- 6 typů zpráv (REQUEST, RESPONSE, EVENT, TRAP, TRAP_RESPONSE, HELLO)
  - parser klasifikace podle pole `type` (primární)
  - parser fingerprint fallback (sekundární, bez `type`)
- Encoder builders + roundtrip pro všech 6 typů
- Edge cases: empty/whitespace/invalid JSON, not-an-object, unknown
  schema/type, NULL args, `jsonl_msg_free(NULL)` safety
- Data sub-tree payload roundtrip (JsonNode deep-copy)
- Stream I/O: write/read N řádků nad `tmpfile()`, EOF detekce, CRLF
  stripping, NULL args

## Plánovaný obsah (V0.A.3+)

- `test_mcp_dispatch.c` - mapování `cmd` -> DBGAPI příkaz (V0.A.3)
- `test_mcp_queue.c` - thread-safe fronta (V0.A.4)
- `test_pipe.py` - end-to-end pipe transport (V0.A.4)
- `test_tcp.py` - end-to-end TCP (V0.A.5, vyžaduje `MZ_NO_MCP_TCP=OFF`)

## Build / spuštění

```bash
# Sestavení + spuštění všech testů (běžný flow)
make test

# Jen MCP testy
ctest --test-dir build -R "^mcp_"

# Jen po labelu mcp
ctest --test-dir build -L mcp

# Manuálně přímo binárku
./build/build-tests/test_mcp_jsonl_parser.exe
```

## Kompilační guard

`tests/mcp/CMakeLists.txt` má `if(MZ_NO_MCP) return() endif()` - při
buildu `make NO_MCP=1` se test nezaregistruje (jeho jediný zdroj
`jsonl_io.c` by se v guard-blockovaném režimu zkompiloval jako prázdný
TU a test by selhal na undefined symbol).

## Blocking RUN breakpoint wakeup latency

`test_run_breakpoint_latency.py` measures host response time separately from
guest execution. Assemble its maintained four-byte Z80 fixture first (sjasmplus):

```bash
mkdir -p build/mcp-run-latency
sjasmplus --raw=build/mcp-run-latency/loop.bin tests/mcp/run_breakpoint_latency.asm
python tests/mcp/test_run_breakpoint_latency.py --exe ./mz800emu \
    --program build/mcp-run-latency/loop.bin \
    --report build/mcp-run-latency/report.json
```

Every one of256 stops must be a breakpoint at1200h after exactly14 T-states,
with no frame elapsed and registers preserved except refresh R/WZ. The
default25ms worst host-latency limit detects a missing pause notification:
the old frame-condition wait sometimes sleeps for its50ms safety poll when
no new framebuffer is published. The loop never changes device clocks or
repairs guest state. On a heavily loaded host the optional `--max-host-ms`
sets an explicit latency budget; the JSON keeps every sample. This is an
opt-in performance regression and needs no Python packages.
