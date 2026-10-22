#!/usr/bin/env python3
"""E2E test: kanál marklog přes trace_start / trace_reset / trace_save / trace_stop (MCP pipe).

Značky z akce breakpointu ``mark`` zapisuje trace kanál marklog. V GUI
a z CLI jde ovládat, přes MCP dřív ne (``trace_*`` znaly jen cputrack,
iorqlog, intlog a hwlog). Ověřuje:

  - ``trace_start`` / ``trace_reset`` / ``trace_save`` / ``trace_stop``
    s ``channel: "marklog"`` uspějí,
  - po běhu programu s breakpointem ``mark "loop"`` a ``trace_stop``
    vznikne v adresáři kanálu manifest ``marks.json`` a neprázdný chunk
    ``marks.NNN.bin``.

Program (MZ-800, RAM 3000h): ``DI; NOP; JR 3001h`` (smyčka přes 3001h,
PC_EXEC BP na 3001h s akcí ``mark "loop"`` pokračuje).

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI
s adresářem marklog v dočasném adresáři, ``--no-save-ini``); zabíjí se
jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Program: DI; NOP; JR 3001h.
PROG = 0x3000
PROG_HEX = "F30018FD"


def run(emu, trace_dir):
    """Nahrávání značek přes MCP a kontrola výstupních souborů."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    res = emu.call("bp_create_with_init",
                   {"fields": ["type", "addr", "action"],
                    "type": "PC_EXEC", "addr": PROG + 1, "action": 'mark "loop"'})
    check(res.get("created") is True, f"breakpoint with mark action created: {res}")

    emu.call("trace_start", {"channel": "marklog"})
    emu.call("run", {"frames": 2})
    emu.call("trace_reset", {"channel": "marklog"})
    emu.call("trace_save", {"channel": "marklog"})
    emu.call("run", {"frames": 1})
    emu.call("trace_stop", {"channel": "marklog"})
    print("  PASS: trace_start/reset/save/stop accept channel marklog")

    manifest = trace_dir / "marks.json"
    check(manifest.is_file(), f"marklog manifest written ({manifest})")
    chunks = [p for p in trace_dir.glob("marks.*.bin") if p.stat().st_size > 0]
    check(len(chunks) >= 1, f"non-empty marklog chunk written ({[c.name for c in chunks]})")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_trace_marklog_"))
    ok = False
    try:
        trace_dir = tmp / "trace"
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                       "[TRACE_MARKLOG]\n"
                       f"dir = {trace_dir.as_posix()}\nname = marks\n"
                       "save_on_exit = 0\nstdout_enabled = 0\n",
                       encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            run(emu, trace_dir)
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
