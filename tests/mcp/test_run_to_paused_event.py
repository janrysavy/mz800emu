#!/usr/bin/env python3
"""E2E test: zastavení na dočasném breakpointu vyšle událost ``paused`` (MCP pipe).

``run_until_addr``, ``step_over`` přes CALL a ``step_out`` zastavují na
dočasném breakpointu. Dřív emulátor jen pozastavil: nenastavil důvod pauzy
a nevyslal událost ``paused``, takže klient musel stav pollovat. Ověřuje:

  - ``run_until_addr``, ``step_over`` přes CALL i ``step_out`` vyšlou
    ``paused`` s ``reason: "run_to"`` a ``pc`` = cílová adresa,
  - ``get_state`` po zastavení hlásí pauzu,
  - dočasný BP na adrese, kde je i uživatelský PC breakpoint s nesplněnou
    podmínkou, emulaci přesto zastaví (``reason: "run_to"``),
  - uživatelský breakpoint bez podmínky na cílové adrese dá jedinou
    událost ``paused`` (``reason: "breakpoint"``), ne dvě.

Program (MZ-800, RAM 3000h):

    3000 F3        DI
    3001 CD 10 30  CALL 3010h
    3004 00        NOP
    3005 18 FE     JR $
    3010 00        NOP
    3011 C9        RET

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

#: Adresa programu v RAM.
PROG = 0x3000
#: Bajty programu na PROG (viz docstring modulu).
PROG_HEAD = "F3CD103000" + "18FE"
#: Podprogram na 3010h: NOP; RET.
SUB = 0x3010
SUB_BYTES = "00C9"
#: Adresa CALL a adresa za ním (návrat z podprogramu).
CALL_ADDR = 0x3001
AFTER_CALL = 0x3004
#: Nejdelší čekání na událost [s].
WAIT_S = 5.0


def _drain(emu):
    """Vyprázdní frontu událostí (bez čekání)."""
    emu.call("event_poll", {"timeout_ms": 0, "max_events": 100})


def _wait_paused(emu, timeout_s=WAIT_S):
    """Čeká na první událost ``paused``; vrátí její data, nebo None."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        d = emu.call("event_poll", {"timeout_ms": 500, "max_events": 100})
        for ev in d.get("events") or []:
            if ev.get("topic") == "paused":
                return ev.get("data") or {}
    return None


def _count_paused(emu, wait_ms=600):
    """Vrátí počet událostí ``paused`` ve frontě po krátkém čekání."""
    time.sleep(wait_ms / 1000.0)
    d = emu.call("event_poll", {"timeout_ms": 0, "max_events": 100})
    return sum(1 for ev in d.get("events") or [] if ev.get("topic") == "paused")


def _load_program(emu):
    """Nahraje program a podprogram, PC = PROG, fronta událostí prázdná."""
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEAD})
    emu.call("mem_write", {"addr": SUB, "data_hex": SUB_BYTES})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    _drain(emu)


def _expect_run_to(emu, pc, what):
    """Ověří událost ``paused`` s reason run_to a daným PC a stav pauzy."""
    ev = _wait_paused(emu)
    check(ev is not None, f"{what}: 'paused' event received")
    check(ev.get("reason") == "run_to", f"{what}: reason is run_to ({ev})")
    check(ev.get("pc") == pc, f"{what}: pc is {pc:04X}h ({ev})")
    state = emu.call("get_state")
    check(state.get("paused") is True, f"{what}: get_state reports paused ({state})")


def case_run_until_addr(emu):
    """run_until_addr na adresu za CALL."""
    print("Case: run_until_addr")
    _load_program(emu)
    emu.call("run_until_addr", {"addr": AFTER_CALL})
    _expect_run_to(emu, AFTER_CALL, "run_until_addr")


def case_step_over_call(emu):
    """step_over na instrukci CALL."""
    print("Case: step_over over CALL")
    _load_program(emu)
    emu.call("run_until_addr", {"addr": CALL_ADDR})
    _expect_run_to(emu, CALL_ADDR, "run_until_addr to CALL")
    emu.call("step_over")
    _expect_run_to(emu, AFTER_CALL, "step_over over CALL")


def case_step_out(emu):
    """step_out z podprogramu (callstack zapíná profiler)."""
    print("Case: step_out")
    emu.call("profiler_start")
    try:
        _load_program(emu)
        emu.call("run_until_addr", {"addr": SUB})
        _expect_run_to(emu, SUB, "run_until_addr into subroutine")
        emu.call("step_out")
        _expect_run_to(emu, AFTER_CALL, "step_out")
    finally:
        emu.call("profiler_stop")


def case_conditional_user_bp(emu):
    """Uživatelský BP s nesplněnou podmínkou nesmí zakrýt dočasný BP."""
    print("Case: run_until_addr onto a conditional user breakpoint")
    res = emu.call("bp_create_with_init",
                   {"fields": ["type", "addr", "expr"],
                    "type": "PC_EXEC", "addr": AFTER_CALL, "expr": "0"})
    check(res.get("created") is True, f"conditional breakpoint created: {res}")
    try:
        _load_program(emu)
        emu.call("run_until_addr", {"addr": AFTER_CALL})
        _expect_run_to(emu, AFTER_CALL, "run_until_addr with false-condition BP")
    finally:
        emu.call("bp_remove", {"id": res["id"]})


def case_single_event(emu):
    """Uživatelský BP bez podmínky na cíli: jediná událost paused."""
    print("Case: run_until_addr onto an unconditional user breakpoint")
    res = emu.call("bp_create_with_init",
                   {"fields": ["type", "addr"],
                    "type": "PC_EXEC", "addr": AFTER_CALL})
    check(res.get("created") is True, f"breakpoint created: {res}")
    try:
        _load_program(emu)
        emu.call("run_until_addr", {"addr": AFTER_CALL})
        ev = _wait_paused(emu)
        check(ev is not None and ev.get("pc") == AFTER_CALL,
              f"'paused' event at {AFTER_CALL:04X}h ({ev})")
        check(ev.get("reason") == "breakpoint",
              f"user breakpoint wins the reason ({ev})")
        n = _count_paused(emu)
        check(n == 0, f"no second 'paused' event (got {n} more)")
    finally:
        emu.call("bp_remove", {"id": res["id"]})


def run(emu):
    """Spustí všechny případy nad jedním procesem emulátoru."""
    emu.call("pause")
    emu.call("event_subscribe", {"topics": ["paused"]})
    case_run_until_addr(emu)
    case_step_over_call(emu)
    case_step_out(emu)
    case_conditional_user_bp(emu)
    case_single_event(emu)


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_run_to_paused_"))
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
