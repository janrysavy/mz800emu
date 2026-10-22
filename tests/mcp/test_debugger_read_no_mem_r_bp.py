#!/usr/bin/env python3
"""E2E test: čtení paměti debuggerem nespouští MEM_R breakpoint (MCP pipe).

Disassembler (GUI i MCP ``dasm``), MCP ``mem_read``, zásobník a další
čtení debuggerem šly přes ``memory_read_byte``, který vyhodnocuje MEM_R
breakpointy. Každé takové čtení adresy s MEM_R BP zvýšilo ``hits``
a vyslalo ``breakpoint_hit`` + ``paused``, přestože CPU nic nečetlo.
Ověřuje:

  - ``mem_read`` a ``dasm`` přes adresu s MEM_R BP nezmění ``hits``
    a nevyšlou ``breakpoint_hit``,
  - skutečné čtení CPU (``LD A,(4000h)``) breakpoint dál spustí.

Program (MZ-800, RAM 3000h): ``DI; LD A,(4000h); JR $``.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Adresa hlídaná MEM_R breakpointem.
WATCHED = 0x4000
#: Program: DI; LD A,(4000h); JR $.
PROG = 0x3000
PROG_HEX = "F33A004018FE"


def _hits(emu, bp_id):
    """Vrátí počet zásahů breakpointu podle bp_list."""
    for bp in emu.call("bp_list").get("breakpoints") or []:
        if bp.get("id") == bp_id:
            return bp.get("hits")
    raise TestFailure(f"breakpoint {bp_id} not in bp_list")


def _topics(emu, wait_ms=300):
    """Vrátí témata událostí ve frontě (po krátkém čekání)."""
    time.sleep(wait_ms / 1000.0)
    d = emu.call("event_poll", {"timeout_ms": 0, "max_events": 100})
    return [ev.get("topic") for ev in d.get("events") or []]


def run(emu):
    """Čtení debuggerem bez zásahu BP, čtení CPU se zásahem."""
    emu.call("pause")
    emu.call("event_subscribe", {"topics": ["breakpoint_hit", "paused"]})
    res = emu.call("bp_create_with_init", {"fields": ["type", "addr"],
                                           "type": "MEM_R", "addr": WATCHED})
    check(res.get("created") is True, f"MEM_R breakpoint created: {res}")
    bp_id = res["id"]
    emu.call("event_poll", {"timeout_ms": 0, "max_events": 100})

    emu.call("mem_read", {"addr": WATCHED - 16, "len": 32})
    check(_hits(emu, bp_id) == 0, "mem_read over the watched address: hits stay 0")
    emu.call("dasm", {"addr": WATCHED - 2, "count": 4})
    check(_hits(emu, bp_id) == 0, "dasm over the watched address: hits stay 0")
    topics = _topics(emu)
    check("breakpoint_hit" not in topics,
          f"no breakpoint_hit event from debugger reads ({topics})")

    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("run")
    deadline = time.time() + 5.0
    while time.time() < deadline and _hits(emu, bp_id) == 0:
        time.sleep(0.05)
    check(_hits(emu, bp_id) == 1, "CPU read LD A,(4000h) still hits the breakpoint")
    state = emu.call("get_state")
    check(state.get("paused") is True, f"emulation paused by the breakpoint ({state})")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_dbg_read_memr_"))
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
