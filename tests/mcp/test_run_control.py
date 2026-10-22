#!/usr/bin/env python3
"""E2E test: čísla běhů, čekání na zastavení, deadline a podmíněná pauza (MCP pipe).

Ověřuje řízení běhu přes MCP:

  - ``run_until_addr`` na nedosažitelnou adresu s ``deadline: {frames: 5}``
    se zastaví s ``reason: "deadline"`` na hranici snímku; délka běhu
    (``elapsed_ticks``) je mezi opakovanými běhy stejná (determinismus),
  - ``deadline: {emulated_ms}`` zastaví nejpozději snímek po deadlinu,
  - ``run_until_addr`` na dosažitelnou adresu: událost ``paused``
    s ``reason: "run_to"``, ``pc`` a ``run_seq`` z odpovědi; před ní jedna
    událost ``resumed``; ``wait_stop`` vrátí stejný záznam,
  - ``step_over`` přes CALL vrátí ``completed: false`` a ``wait_stop``
    pak ``run_to`` za CALLem; ``step_into`` vrátí ``completed: true``,
  - ``wait_stop`` během nekonečného běhu skončí po timeoutu
    (``done: false``) a spojení dál odpovídá,
  - ``pause(if_run_seq)`` se starým číslem běh nezastaví, se správným ano,
  - frame-bounded ``run(frames)`` přerušený breakpointem nezastaví další,
    nesouvisející běh na starém cíli,
  - ``get_stop_history`` a ``get_state`` hlásí stejná čísla běhů.

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
#: Adresa CALL, adresa za ním a smyčka JR $.
CALL_ADDR = 0x3001
AFTER_CALL = 0x3004
LOOP = 0x3005
#: Adresa, kam program nikdy nedojde.
UNREACHABLE = 0x4000
#: Nejdelší čekání na událost [s].
WAIT_S = 5.0


def _events(emu, wait_ms=0):
    """Vyzvedne všechny čekající události (volitelně po krátkém čekání)."""
    if wait_ms:
        time.sleep(wait_ms / 1000.0)
    d = emu.call("event_poll", {"timeout_ms": 0, "max_events": 100})
    return d.get("events") or []


def _wait_event(emu, topic, timeout_s=WAIT_S):
    """Čeká na první událost daného tématu; vrátí (data, ostatní události)."""
    seen = []
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        d = emu.call("event_poll", {"timeout_ms": 500, "max_events": 100})
        for ev in d.get("events") or []:
            if ev.get("topic") == topic:
                return ev.get("data") or {}, seen
            seen.append(ev)
    return None, seen


def _load(emu, pc=PROG):
    """Nahraje program, nastaví PC a vyprázdní frontu událostí."""
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEAD})
    emu.call("mem_write", {"addr": SUB, "data_hex": SUB_BYTES})
    emu.call("set_register", {"reg": "PC", "value": pc})
    _events(emu)


def _wait_stop(emu, run_seq, timeout_ms=5000):
    """wait_stop s prodlouženým limitem odpovědi."""
    return emu.call("wait_stop", {"run_seq": run_seq, "timeout_ms": timeout_ms},
                    timeout=timeout_ms / 1000.0 + 10.0)


def case_deadline_frames(emu, vst):
    """Deadline 5 snímků na nedosažitelné adrese: stop deadline, deterministicky."""
    print("Case: run_until_addr with deadline frames")
    elapsed = []
    for i in range(3):
        _load(emu, LOOP)
        r = emu.call("run_until_addr", {"addr": UNREACHABLE, "deadline": {"frames": 5}})
        check(r.get("running") is True and "run_seq" in r, f"run {i}: started ({r})")
        w = _wait_stop(emu, r["run_seq"])
        rec = w.get("record") or {}
        check(w.get("done") is True and rec.get("reason") == "deadline",
              f"run {i}: wait_stop reports deadline ({w})")
        check(rec.get("pc") == LOOP, f"run {i}: stopped inside JR $ ({rec.get('pc')})")
        # Stop na hranici snímku: nejvýš délka jedné instrukce za ní
        # (JR $ = 12 T-stavů, 60 taktů GDG u MZ-800).
        check(rec.get("stop_ticks", -1) % vst < 200,
              f"run {i}: stopped at a frame boundary (stop_ticks {rec.get('stop_ticks')})")
        elapsed.append(rec.get("elapsed_ticks"))
    # První běh začíná uvnitř snímku, další dva přesně na hranici snímku.
    check(elapsed[1] == elapsed[2] == 5 * vst,
          f"deterministic length of 5 frames ({elapsed}, frame = {vst} ticks)")


def case_deadline_ms(emu, pxclk, vst):
    """Deadline v ms emulovaného času: zastaví nejpozději snímek po deadlinu."""
    print("Case: resume with deadline emulated_ms")
    _load(emu, LOOP)
    r = emu.call("run", {"deadline": {"emulated_ms": 30}})
    want = 30 * pxclk // 1000
    check(r.get("deadline_ticks") == want, f"deadline_ticks = {want} ({r})")
    w = _wait_stop(emu, r["run_seq"])
    rec = w.get("record") or {}
    check(rec.get("reason") == "deadline", f"reason deadline ({rec})")
    el = rec.get("elapsed_ticks", 0)
    check(want <= el < want + vst, f"elapsed {el} within one frame after {want}")


def case_run_to_events(emu):
    """run_until_addr na dosažitelnou adresu: resumed, paused, wait_stop."""
    print("Case: run_until_addr reaches the target")
    _load(emu)
    r = emu.call("run_until_addr", {"addr": AFTER_CALL})
    seq = r.get("run_seq")
    ev, before = _wait_event(emu, "paused")
    check(ev is not None, "'paused' event received")
    check(ev.get("reason") == "run_to" and ev.get("pc") == AFTER_CALL,
          f"paused: run_to at {AFTER_CALL:04X}h ({ev})")
    check(ev.get("run_seq") == seq, f"paused carries run_seq {seq} ({ev.get('run_seq')})")
    check(ev.get("started_by") == "mcp" and ev.get("start_cmd") == "run_to",
          f"started by mcp run_to ({ev.get('started_by')}, {ev.get('start_cmd')})")
    resumed = [e for e in before if e.get("topic") == "resumed"]
    check(len(resumed) == 1 and (resumed[0].get("data") or {}).get("run_seq") == seq,
          f"one 'resumed' event with run_seq {seq} before 'paused' ({resumed})")
    extra = [e for e in _events(emu, 300) if e.get("topic") == "paused"]
    check(not extra, f"no second 'paused' event ({extra})")
    w = _wait_stop(emu, seq, 0)
    check(w.get("done") is True and (w.get("record") or {}).get("pc") == AFTER_CALL,
          f"wait_stop returns the same stop ({w})")


def case_steps(emu):
    """step_into je dokončený, step_over přes CALL ne (běh k dočasnému BP)."""
    print("Case: step_into and step_over over CALL")
    _load(emu, PROG)
    r = emu.call("step_into")
    check(r.get("completed") is True and r.get("running") is False,
          f"step_into completed ({r})")
    regs = emu.call("get_registers")
    check(regs.get("PC") == CALL_ADDR, f"PC after step_into is {CALL_ADDR:04X}h ({regs.get('PC')})")
    r = emu.call("step_over")
    check(r.get("completed") is False and r.get("running") is True,
          f"step_over over CALL is a run, not completed ({r})")
    w = _wait_stop(emu, r["run_seq"])
    rec = w.get("record") or {}
    check(rec.get("reason") == "run_to" and rec.get("pc") == AFTER_CALL,
          f"wait_stop: run_to after the CALL ({rec})")
    r2 = emu.call("step_into")
    check(r2.get("run_seq") == r["run_seq"] + 1, f"next step has the next run_seq ({r2})")
    w = _wait_stop(emu, r2["run_seq"], 0)
    check((w.get("record") or {}).get("reason") == "step" and (w.get("record") or {}).get("step") is True,
          f"step record ({w})")


def case_wait_timeout_and_pause_if(emu):
    """wait_stop po timeoutu, pause(if_run_seq) se starým a správným číslem."""
    print("Case: wait_stop timeout and pause(if_run_seq)")
    _load(emu, LOOP)
    r = emu.call("run", {})
    seq = r["run_seq"]
    t0 = time.time()
    w = _wait_stop(emu, seq, 300)
    dt = time.time() - t0
    check(w.get("done") is False and w.get("running") is True,
          f"wait_stop times out while running ({w})")
    check(0.25 <= dt < 5.0, f"wait took about the timeout ({dt:.2f} s)")
    st = emu.call("get_state")
    check(st.get("running") is True and st.get("run_seq") == seq,
          f"connection still answers, get_state run_seq {seq} ({st})")
    p = emu.call("pause", {"if_run_seq": seq - 1})
    check(p.get("paused") is False and p.get("skipped") == "run_seq_mismatch",
          f"pause with an old run_seq does not stop the run ({p})")
    p = emu.call("pause", {"if_run_seq": seq})
    check(p.get("paused_now") is True, f"pause with the current run_seq stops it ({p})")
    w = _wait_stop(emu, seq, 2000)
    rec = w.get("record") or {}
    check(rec.get("reason") == "manual" and rec.get("stopped_by") == "mcp",
          f"stop record: manual pause by mcp ({rec})")
    p = emu.call("pause", {"if_run_seq": seq})
    check(p.get("paused") is True and p.get("skipped") == "not_running",
          f"pause(if_run_seq) on a paused emulator ({p})")


def case_frames_run_not_left_armed(emu):
    """run(frames) přerušený breakpointem nezastaví další běh na starém cíli."""
    print("Case: interrupted run(frames) does not stop a later run")
    _load(emu, PROG)
    bp = emu.call("bp_create_with_init", {"fields": ["type", "addr"],
                                          "type": "PC_EXEC", "addr": AFTER_CALL})
    try:
        r = emu.call("run", {"frames": 50})
        check(r.get("stopped_by") == "breakpoint", f"run(frames) stopped by the breakpoint ({r})")
    finally:
        emu.call("bp_remove", {"id": bp["id"]})
    emu.call("set_register", {"reg": "PC", "value": LOOP})
    r = emu.call("run", {"deadline": {"frames": 100}})
    w = _wait_stop(emu, r["run_seq"], 10000)
    rec = w.get("record") or {}
    check(rec.get("reason") == "deadline",
          f"the later run ends by its own deadline, not by the old frame target ({rec})")


def case_deadline_over_reset(emu, vst):
    """Deadline běží i přes reset stroje uprostřed běhu."""
    print("Case: deadline survives a reset during the run")
    _load(emu, LOOP)
    r = emu.call("run", {"deadline": {"frames": 30}})
    time.sleep(0.2)
    emu.call("reset")
    w = _wait_stop(emu, r["run_seq"], 10000)
    rec = w.get("record") or {}
    check(rec.get("reason") == "deadline", f"run stopped by its deadline after a reset ({rec})")
    el = rec.get("elapsed_ticks", 0)
    # Deadline snímků je hranice snímku; reset uprostřed snímku ji posune
    # nejvýš o zbytek snímku.
    check(29 * vst <= el <= 31 * vst, f"elapsed {el} is about 30 frames ({30 * vst})")


def case_history(emu):
    """get_stop_history: nejnovější záznam odpovídá get_state."""
    print("Case: stop history")
    h = emu.call("get_stop_history", {"limit": 5})
    st = emu.call("get_state")
    recs = h.get("records") or []
    check(1 <= len(recs) <= 5, f"history has records ({len(recs)})")
    check(recs[-1].get("run_seq") == st.get("run_seq") == h.get("current_run_seq"),
          f"newest record is the current run ({recs[-1].get('run_seq')}, {st.get('run_seq')})")
    seqs = [x.get("run_seq") for x in recs]
    check(seqs == sorted(seqs), f"oldest first ({seqs})")


def run(emu):
    """Spustí všechny případy nad jedním procesem emulátoru."""
    emu.call("pause")
    info = emu.call("get_platform_info")
    pxclk = info.get("pxclk_hz")
    vst = pxclk // info.get("framerate_hz")
    check(vst > 0, f"frame length {vst} GDG ticks ({pxclk} Hz)")
    emu.call("event_subscribe", {"topics": ["paused", "resumed"]})
    case_deadline_frames(emu, vst)
    case_deadline_ms(emu, pxclk, vst)
    case_run_to_events(emu)
    case_steps(emu)
    case_wait_timeout_and_pause_if(emu)
    case_frames_run_not_left_armed(emu)
    case_deadline_over_reset(emu, vst)
    case_history(emu)


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=150)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_run_control_"))
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
