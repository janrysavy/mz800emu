#!/usr/bin/env python3
"""E2E test: drain fronty na hranici akce BP vykoná jen řídicí příkazy (MCP pipe).

Breakpoint s akcí ``continue`` po každém zásahu obslouží čekající příkazy
fronty (aby šla emulace zastavit i při hustých akcích, které natahují
snímek). MEM_W BP se ale vyhodnocuje před zápisem, tedy uprostřed
instrukce. Dřív tento drain vykonal celou frontu: čtení tam vidělo PC za
instrukcí a paměť ještě před zápisem. Teď v něm proběhnou jen pauza,
vynucená pauza a dotaz na stav; ostatní příkazy počkají na drain na hranici
snímku nebo v pauze.

Program v RAM (2000h): DI; LD HL,0; INC HL; LD (3000h),HL; JR 2004h, MEM_W
BP na 3000h s akcí ``continue``. Ověřuje:

  - ``get_raster_pos`` za běhu se vykoná vždy na hranici snímku: všechny
    vzorky mají stejný řádek rastru (dřív se většina vykonala v drainu BP
    na libovolném řádku),
  - ``pause`` za běhu projde a emulace stojí,
  - v pauze ``get_registers`` + ``mem_read`` vidí konzistentní stav
    ([3000h] = HL, nebo HL-1 před zápisem na PC=2005h).

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: 2000h DI; 2001h LD HL,0; 2004h INC HL; 2005h LD (3000h),HL; 2008h JR 2004h.
PROG = "F32100002322003018FA"
#: Počet vzorků get_raster_pos za běhu.
SAMPLES = 100


def run(emu):
    """Vzorky rastru za běhu s MEM_W BP + continue, pak pauza a konzistence."""
    emu.call("pause")
    emu.call("mem_write", {"addr": 0x2000, "data_hex": PROG})
    emu.call("set_register", {"reg": "PC", "value": 0x2000})
    bp = emu.call("bp_create_with_init", {
        "fields": ["type", "addr", "action", "enabled"],
        "type": "MEM_W", "addr": 0x3000, "action": "continue", "enabled": True})
    check(bp.get("id", 0) > 0, f"MEM_W BP with action continue created ({bp})")

    emu.call("run")
    time.sleep(0.2)
    rows = {}
    t0 = time.time()
    for _ in range(SAMPLES):
        r = emu.call("get_raster_pos")
        rows[r["scanline"]] = rows.get(r["scanline"], 0) + 1
    dt = (time.time() - t0) / SAMPLES * 1000.0
    print(f"  get_raster_pos while running: scanlines {dict(sorted(rows.items()))}, "
          f"{dt:.1f} ms per call")
    check(len(rows) == 1,
          "every get_raster_pos ran at the frame boundary (one scanline), "
          "none in the BP action drain")

    t0 = time.time()
    emu.call("pause")
    print(f"  pause took {(time.time() - t0) * 1000.0:.1f} ms")
    st = emu.call("get_state")
    check(st.get("running") is False, f"emulation paused ({st})")

    regs = emu.call("get_registers")
    raw = base64.b64decode(emu.call("mem_read", {"addr": 0x3000, "len": 2})
                           .get("data_b64", ""))
    val = int.from_bytes(raw, "little") if len(raw) == 2 else -1
    pc, hl = regs["PC"], regs["HL"]
    want = (hl - 1) & 0xFFFF if pc == 0x2005 else hl
    check(val == want, f"paused state consistent: PC={pc:04X}h HL={hl:04X}h "
                       f"[3000h]={val:04X}h")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=100)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_bp_action_drain_"))
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
        emu_test_proc.rmtree_retry(tmp)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
