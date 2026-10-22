#!/usr/bin/env python3
"""E2E test: stránkované čtení Trace Suite přes trace_status / trace_read (MCP pipe).

Ověřuje kontrakt devdoc/trace/stream-read.md na skutečném emulátoru:

  1  krátký běh pod velikost chunku: data jsou jen v RAM bufferu, a přesto
     ``trace_read`` za běhu (v pauze, záznam běží) vrátí všechny záznamy
     (dřív "krátký běh se do souboru nedostal"); stránky navazují bez
     mezer; po ``trace_stop`` je segment dál čitelný (``live: false``)
     a ``raw_b64`` všech stránek je bajt po bajtu shodný s chunk soubory;
  2  čas cputrack: chunky po 1 MB, program s trvalým INT z CTC2 (IM 2);
     čas posledního záznamu chunku + jeho T-stavy = kotva dalšího chunku
     v manifestu (dřív chybělo přijetí přerušení, posun 19 T na přijetí);
     záznamy přijetí přerušení mají ``asm`` "INT ACK";
  3  stop + start téhož kanálu: starý kurzor -> chyba ``segment_replaced:``;
  4  marklog: značky z akce breakpointu ``mark`` se čtou se jménem;
  5  eventlog (ring Event Vieweru): události s textem detailu, seq
     souvisle; po ``eventlog_clear`` starý kurzor -> ``segment_replaced:``.

Program (MZ-800, RAM 3000h). Proces běží izolovaně (dočasný ``--cfg-dir``
/``--work-dir``, vlastní INI s adresáři trace v dočasném adresáři,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import json
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

PROG = 0x3000
#: Trvalé INT z CTC2 (viz test_snapshot_z80_hidden.py): DI; IM 2; I = 32h;
#: CT1 režim 2 N = 2, CT2 režim 0 N = 1 přes 0E003h-0E007h; PC2 = 1;
#: DJNZ; EI; smyčka INC C; JR -3.
PROG_INT = bytes.fromhex(
    "F3" "ED5E" "3E32" "ED47"
    "3E04" "3203E0"
    "3E74" "3207E0" "3E02" "3205E0" "AF" "3205E0"
    "3EB0" "3207E0" "3E01" "3206E0" "AF" "3206E0"
    "3E05" "3203E0"
    "0600" "10FE"
    "FB" "0C" "18FD")
#: ISR na 3434h: INC D; EI; RETI; tabulka IM 2 3200h-3300h -> 3434h.
ISR = bytes.fromhex("14" "FB" "ED4D")
#: DI; NOP; JR 3001h (smyčka přes 3001h pro BP s mark).
PROG_LOOP = bytes.fromhex("F3" "00" "18FD")
#: Takty GDG na T-stav (MZ-800).
DIVIDER = 5


def read_all(emu, channel, columns=None, fmt="rows", limit=None):
    """Přečte kanál od začátku po konec stránkami; vrátí (stránky, poslední odpověď)."""
    pages = []
    req = {"channel": channel, "format": fmt}
    if columns:
        req["columns"] = columns
    if limit is not None:
        req["limit"] = limit
    cursor = None
    for _ in range(10000):
        r = dict(req)
        if cursor:
            r["cursor"] = cursor
        resp = emu.call("trace_read", r)
        pages.append(resp)
        cursor = resp["next_cursor"]
        if resp["end_of_data"]:
            return pages, resp
    raise TestFailure("trace_read did not reach end_of_data")


def case_short_run(emu, trace_dir):
    """Scénář 1: krátký běh jen v RAM, čtení za běhu i po stopu, raw = soubory."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_LOOP.hex()})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("trace_start", {"channel": "cputrack"})
    emu.call("run", {"frames": 1})
    st = emu.call("trace_status", {"channel": "cputrack"})["channels"][0]
    check(st["live"] and st["next_seq"] > 0 and st["chunks"] == 0,
          f"short run: records only in RAM ({st['next_seq']} records, {st['chunks']} chunks)")
    pages, last = read_all(emu, "cputrack", columns=["seq", "pc", "asm"], limit=500)
    seqs = [row[0] for p in pages for row in p["rows"]]
    check(seqs == list(range(st["next_seq"])) or seqs[:st["next_seq"]] == list(range(st["next_seq"])),
          f"live read returns all records in order ({len(seqs)} rows)")
    check(len(pages) > 1, f"read took several pages ({len(pages)})")
    check(all(len(json.dumps(p)) <= 20000 for p in pages), "every page fits into max_chars 20000")
    emu.call("trace_stop", {"channel": "cputrack"})
    st2 = emu.call("trace_status", {"channel": "cputrack"})["channels"][0]
    check(not st2["live"] and st2["segment"] == st["segment"] and st2["chunks"] >= 1,
          "after trace_stop the segment stays readable")
    raw_pages, _ = read_all(emu, "cputrack", fmt="raw_b64")
    raw = b"".join(base64.b64decode(p["data_b64"]) for p in raw_pages)
    files = b"".join(f.read_bytes() for f in sorted(trace_dir.glob("ct.*.bin")))
    check(raw == files and len(raw) == st2["next_seq"] * 12,
          f"raw_b64 equals chunk files byte for byte ({len(raw)} B)")
    return pages[0]["next_cursor"], st2["segment"]


def case_time(emu, trace_dir):
    """Scénář 2: čas cputrack přes hranice chunků s trvalým INT."""
    emu.call("pause")
    emu.call("mem_write", {"addr": 0x3200, "data_hex": "34" * 257})
    emu.call("mem_write", {"addr": 0x3434, "data_hex": ISR.hex()})
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_INT.hex()})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("trace_start", {"channel": "cputrack"})
    emu.call("run", {"frames": 30})
    emu.call("trace_stop", {"channel": "cputrack"})
    meta = json.loads((trace_dir / "ct.json").read_text(encoding="utf-8"))
    chunks = meta["chunks"]
    check(len(chunks) >= 2, f"recording spans several chunks ({len(chunks)})")
    check(meta["subsys_header"].get("event_record_version") == 2, "manifest event_record_version 2")
    seq0 = 0
    for k in range(len(chunks) - 1):
        n = chunks[k]["bytes"] // 12
        first = emu.call("trace_read", {"channel": "cputrack", "from": f"seq:{seq0}", "limit": 1,
                                        "columns": ["t"]})
        check(first["rows"][0][0] == chunks[k]["start_pxclk"],
              f"chunk {k}: first record time = chunk anchor")
        r = emu.call("trace_read", {"channel": "cputrack", "from": f"seq:{seq0 + n - 1}", "limit": 1,
                                    "columns": ["seq", "t_std", "wait", "t"]})
        row = dict(zip(r["columns"], r["rows"][0]))
        end = row["t"] + (row["t_std"] + row["wait"]) * DIVIDER
        check(end == chunks[k + 1]["start_pxclk"],
              f"chunk {k}: last record end {end} = next chunk anchor {chunks[k + 1]['start_pxclk']}")
        seq0 += n
    irq = emu.call("trace_read", {"channel": "cputrack", "from": "seq:0", "limit": 400,
                                  "columns": ["pc", "asm", "t_std"]})
    acks = [row for row in irq["rows"] if row[1] == "INT ACK"]
    check(acks and all(row[0] == "3434" and row[2] == 19 for row in acks),
          f"interrupt acknowledge records: pc 3434h, 19 T ({len(acks)} in the first page)")


def case_segment_replaced(emu, cursor):
    """Scénář 3: kurzor starého segmentu po stop + start."""
    try:
        emu.call("trace_read", {"channel": "cputrack", "cursor": cursor})
    except TestFailure as e:
        check("segment_replaced:" in str(e), f"old cursor rejected with segment_replaced ({e})")
        return
    raise TestFailure("old cursor was accepted")


def case_marklog(emu):
    """Scénář 4: značky z akce mark se čtou se jménem."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_LOOP.hex()})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    res = emu.call("bp_create_with_init",
                   {"fields": ["type", "addr", "action"],
                    "type": "PC_EXEC", "addr": PROG + 1, "action": 'mark "loop"'})
    check(res.get("created") is True, "breakpoint with mark action created")
    emu.call("trace_start", {"channel": "marklog"})
    emu.call("run", {"frames": 1})
    emu.call("pause")
    r = emu.call("trace_read", {"channel": "marklog", "limit": 5})
    check(r["rows"] and all(dict(zip(r["columns"], row))["name"] == "loop" for row in r["rows"]),
          f"marklog rows carry the marker name ({len(r['rows'])} rows)")
    emu.call("trace_stop", {"channel": "marklog"})


def case_eventlog(emu):
    """Scénář 5: ring Event Vieweru přes trace_read."""
    emu.call("pause")
    emu.call("eventlog_set_mask", {"mask": "0xFFFFFFFFFFFFFFFF"})
    emu.call("eventlog_start")
    emu.call("reset")
    emu.call("run", {"frames": 5})
    emu.call("pause")
    st = emu.call("trace_status", {"channel": "eventlog"})["channels"][0]
    check(st["next_seq"] > 0, f"eventlog recorded events ({st['next_seq']})")
    r = emu.call("trace_read", {"channel": "eventlog", "limit": 50})
    rows = [dict(zip(r["columns"], row)) for row in r["rows"]]
    check(rows and all(row["text"] for row in rows), "eventlog rows carry the detail text")
    check([row["seq"] for row in rows] == list(range(r["first_seq"], r["first_seq"] + len(rows))),
          "eventlog seq is contiguous from first_seq")
    cursor = r["next_cursor"]
    emu.call("eventlog_clear")
    try:
        emu.call("trace_read", {"channel": "eventlog", "cursor": cursor})
    except TestFailure as e:
        check("segment_replaced:" in str(e), "eventlog_clear makes the old cursor segment_replaced")
    else:
        raise TestFailure("old eventlog cursor was accepted after eventlog_clear")
    emu.call("eventlog_stop")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=280)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_trace_read_"))
    ok = False
    try:
        trace_dir = tmp / "trace"
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                       "[TRACE_CPUTRACK]\n"
                       f"dir = {trace_dir.as_posix()}\nname = ct\nchunk_mb = 1\nsave_on_exit = 0\n"
                       "[TRACE_MARKLOG]\n"
                       f"dir = {(tmp / 'marks').as_posix()}\nname = marks\n"
                       "save_on_exit = 0\nstdout_enabled = 0\n",
                       encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            empty = emu.call("trace_read", {"channel": "intlog"})
            check(empty["segment"] == 0 and empty["end_of_data"] and not empty["rows"],
                  "channel without recording: empty page, segment 0")
            cursor, _ = case_short_run(emu, trace_dir)
            case_time(emu, trace_dir)
            case_segment_replaced(emu, cursor)
            case_marklog(emu)
            case_eventlog(emu)
            ok = True
        finally:
            emu.close()
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        shutil.rmtree(tmp, ignore_errors=True)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
