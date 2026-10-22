#!/usr/bin/env python3
"""E2E test: zápis paměti debuggerem nespouští MEM_W breakpoint (MCP pipe).

MCP ``mem_write``, ``region_write`` do regionu Logical (a úpravy paměti
v GUI, freeze) šly přes ``memory_write_byte``, který vyhodnocuje MEM_W
breakpointy: každý zapsaný bajt na hlídané adrese zvýšil ``hits``
a vyslal ``breakpoint_hit`` + ``paused``. Ověřuje:

  - ``mem_write`` a ``region_write`` (Logical) přes adresu s MEM_W BP
    nezmění ``hits`` a nevyšlou ``breakpoint_hit``; zápis se provede,
  - skutečný zápis CPU (``LD (4000h),A``) breakpoint dál spustí.

Program (MZ-800, RAM 3000h): ``DI; LD A,55h; LD (4000h),A; JR $``.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Adresa hlídaná MEM_W breakpointem.
WATCHED = 0x4000
#: Program: DI; LD A,55h; LD (4000h),A; JR $.
PROG = 0x3000
PROG_HEX = "F33E5532004018FE"


def _hits(emu, bp_id):
    """Vrátí počet zásahů breakpointu podle bp_list."""
    for bp in emu.call("bp_list").get("breakpoints") or []:
        if bp.get("id") == bp_id:
            return bp.get("hits")
    raise TestFailure(f"breakpoint {bp_id} not in bp_list")


def _byte(emu, addr):
    """Přečte jeden bajt přes mem_read."""
    d = emu.call("mem_read", {"addr": addr, "len": 1})
    return base64.b64decode(d.get("data_b64", ""))[0]


def run(emu):
    """Zápisy debuggerem bez zásahu BP, zápis CPU se zásahem."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("event_subscribe", {"topics": ["breakpoint_hit", "paused"]})
    res = emu.call("bp_create_with_init", {"fields": ["type", "addr"],
                                           "type": "MEM_W", "addr": WATCHED})
    check(res.get("created") is True, f"MEM_W breakpoint created: {res}")
    bp_id = res["id"]
    emu.call("event_poll", {"timeout_ms": 0, "max_events": 100})

    emu.call("mem_write", {"addr": WATCHED, "data_hex": "11"})
    check(_hits(emu, bp_id) == 0, "mem_write to the watched address: hits stay 0")
    check(_byte(emu, WATCHED) == 0x11, "mem_write was carried out")

    logical = [r for r in emu.call("regions_list").get("regions") or []
               if r.get("kind") == "logical"]
    check(len(logical) == 1, f"one Logical region ({logical})")
    emu.call("region_write", {"region_id": logical[0]["id"], "offset": WATCHED,
                              "data_b64": base64.b64encode(b"\x22").decode()})
    check(_hits(emu, bp_id) == 0, "region_write (Logical) to the watched address: hits stay 0")
    check(_byte(emu, WATCHED) == 0x22, "region_write was carried out")

    time.sleep(0.3)
    d = emu.call("event_poll", {"timeout_ms": 0, "max_events": 100})
    topics = [ev.get("topic") for ev in d.get("events") or []]
    check("breakpoint_hit" not in topics,
          f"no breakpoint_hit event from debugger writes ({topics})")

    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("run")
    deadline = time.time() + 5.0
    while time.time() < deadline and _hits(emu, bp_id) == 0:
        time.sleep(0.05)
    check(_hits(emu, bp_id) == 1, "CPU write LD (4000h),A still hits the breakpoint")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_dbg_write_memw_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n",
                       encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            run(emu)
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
