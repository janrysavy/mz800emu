#!/usr/bin/env python3
"""E2E test: byte backstop akcí BP nezastaví samotný trace (MCP pipe).

Byte backstop (``[BREAKPOINTS] fwd_byte_limit_mb``) hlídá objem zápisů
akcí breakpointů. Dřív se do něj započítával každý zápis trace na disk,
i bez jakéhokoli breakpointu: po překročení prahu se emulace sama
zapauzovala ("BP #-1"), headless běh s ``--exit-after-frames`` pak visel.
Teď se trace bez akcí BP do backstopu nepočítá (hlídá ho jeho vlastní
``max_total_mb``); trace, který spustila akce BP, dál ano.

Práh 1 MB, chunk cputrack 1 MB, program v RAM (2000h: INC HL; JR 2000h)
zapíše za 60 snímků několik MB. Fáze (každá v novém procesu):

  A  ``trace_start`` přes MCP, bez breakpointu: ``run`` 60 snímků doběhne
     (``stopped_by: frames``), trace má víc chunků než práh.
  B  trace spouští akce PC_EXEC BP (``trace_start cputrack``): backstop
     emulaci zastaví, ``run`` hlásí ``stopped_by: bp_action_saturation``.

Procesy běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: 2000h INC HL; 2001h JR 2000h.
PROG = "2318FD"
#: Počet snímků běhu (několik MB cputrack záznamů).
FRAMES = 60


def _ini(tmp):
    """INI: backstop 1 MB, chunk cputrack 1 MB (čísla v INI jsou hex)."""
    return (
        "[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
        "fwd_byte_limit_mb = 0x1\n"
        "[TRACE_CPUTRACK]\nchunk_mb = 0x1\n"
        f"dir = {(tmp / 'trace').as_posix()}\n"
    )


def _start(tmp, name):
    """Spustí izolovaný proces s INI testu a nahraje program."""
    d = tmp / name
    (d / "cfg").mkdir(parents=True)
    ini = d / "test.ini"
    ini.write_text(_ini(d), encoding="utf-8")
    emu = PipeEmu(_find_exe(), d / "cfg", ini)
    emu.call("pause")
    emu.call("mem_write", {"addr": 0x2000, "data_hex": PROG})
    emu.call("set_register", {"reg": "PC", "value": 0x2000})
    return emu


def phase_a(tmp):
    """Trace přes MCP bez BP: backstop ho nezastaví."""
    print("  Phase A: trace started over MCP, no breakpoint")
    emu = _start(tmp, "a")
    try:
        emu.call("trace_start", {"channel": "cputrack"})
        r = emu.call("run", {"frames": FRAMES}, timeout=60.0)
        st = emu.call("trace_status", {"channel": "cputrack"})["channels"][0]
        print(f"    run: {r.get('stopped_by')} {r.get('actual_frames')} frames, "
              f"trace chunks {st.get('chunks')}")
        check(r.get("stopped_by") == "frames" and r.get("actual_frames") == FRAMES,
              f"run of {FRAMES} frames completed (stopped_by: {r.get('stopped_by')})")
        check(st.get("chunks", 0) >= 2,
              f"trace wrote more than the 1 MB limit ({st.get('chunks')} chunks)")
    finally:
        emu.close()


def phase_b(tmp):
    """Trace spuštěný akcí BP: backstop emulaci zastaví s důvodem."""
    print("  Phase B: trace started by a breakpoint action")
    emu = _start(tmp, "b")
    try:
        bp = emu.call("bp_create_with_init", {
            "fields": ["type", "addr", "action", "enabled"],
            "type": "PC_EXEC", "addr": 0x2000,
            "action": "trace_start cputrack", "enabled": True})
        check(bp.get("id", 0) > 0, f"PC_EXEC BP with trace_start created ({bp})")
        r = emu.call("run", {"frames": FRAMES}, timeout=60.0)
        print(f"    run: {r.get('stopped_by')} {r.get('actual_frames')} frames")
        check(r.get("stopped_by") == "bp_action_saturation"
              and r.get("actual_frames", FRAMES) < FRAMES,
              f"byte backstop paused the run (stopped_by: {r.get('stopped_by')})")
    finally:
        emu.close()


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=150)
    print(f"Using binary: {_find_exe()}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_trace_backstop_"))
    failures = []
    for name, fn in (("A", phase_a), ("B", phase_b)):
        try:
            fn(tmp)
        except TestFailure as e:
            failures.append(f"phase {name}: {e}")
            print(f"  FAIL: {e}")
    if not failures:
        emu_test_proc.rmtree_retry(tmp)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
