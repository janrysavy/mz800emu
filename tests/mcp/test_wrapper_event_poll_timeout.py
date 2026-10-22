#!/usr/bin/env python3
"""Regresní test MCP wrapperu: emu_event_poll s timeout_ms delším než limit odpovědi.

``emu_event_poll`` dovolí ``timeout_ms`` až 60 000, backend tak dlouho čeká
na událost. Wrapper ale volal ``_send_request`` s výchozím limitem
odpovědi ``SEND_TIMEOUT_S`` (30 s), takže poll s ``timeout_ms`` nad 30 000
skončil chybou wrapperu, i když backend odpověděl včas.

Test běží proti FALEŠNÉMU emulátoru (Python skript s JSONL protokolem),
který na ``event_poll`` odpoví až po ``timeout_ms``. ``SEND_TIMEOUT_S`` je
zkrácený na 1 s, ``timeout_ms`` = 2500, takže test trvá jen pár sekund:

  A  ``emu_event_poll(timeout_ms=2500)`` vrátí odpověď backendu (bez chyby
     wrapperu), i když čekání přesáhne ``SEND_TIMEOUT_S``,
  B  ``emu_event_poll(timeout_ms=0)`` dál funguje.

mcp_server importuje FastMCP; pod systémovým Pythonem bez něj se test
re-execne pod venv interpretem MCP serveru (proměnná MZ_MCP_VENV_PY nebo
mcp-server/.venv), bez venv vrací 77 (SKIP).

Exit 0 = vše PASS, 1 = aspoň jeden FAIL, 77 = SKIP.
"""
import asyncio
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů

SKIP_EXIT = 77

REPO_ROOT = Path(__file__).resolve().parents[2]
MCP_DIR = REPO_ROOT / "mcp-server"


def _find_venv_python():
    """Najde venv interpret MCP serveru (priorita: env, .venv, žádný)."""
    env = os.environ.get("MZ_MCP_VENV_PY")
    if env and Path(env).is_file():
        return Path(env)
    for c in (MCP_DIR / ".venv" / "Scripts" / "python.exe",
              MCP_DIR / ".venv" / "bin" / "python",
              MCP_DIR / ".venv" / "bin" / "python3"):
        if c.is_file():
            return c
    return None


# Úklid spuštěných procesů (i venv interpretu) při selhání, přerušení
# nebo zabití ctestem; vnitřní limit je kratší než TIMEOUT v ctestu.
if __name__ == "__main__":
    emu_test_proc.install(deadline_s=50)

# Spuštění pod venv interpretem (sentinel MZ_MCP_REEXEC zaručí právě jedno
# opakované spuštění), viz test_wrapper_emu_exit.py.
try:
    import mcp  # noqa: F401
except ImportError:
    _venv = _find_venv_python()
    if _venv is not None and not os.environ.get("MZ_MCP_REEXEC"):
        os.environ["MZ_MCP_REEXEC"] = "1"
        sys.exit(subprocess.call(
            [str(_venv), str(Path(__file__).resolve()), *sys.argv[1:]]))
    print("SKIP: FastMCP/mcp not available (.venv missing or without mcp)",
          file=sys.stderr)
    sys.exit(SKIP_EXIT)

sys.path.insert(0, str(MCP_DIR))
os.environ.setdefault("MZ800EMU_TRANSPORT", "pipe")
import mcp_server as m  # noqa: E402

#: Falešný emulátor: hello, na event_poll odpoví po timeout_ms prázdnou
#: frontou, na ostatní příkazy hned.
FAKE_EMU = r'''
import json, sys, time
print(json.dumps({"type": "hello", "commands": ["event_poll"]}), flush=True)
for line in sys.stdin:
    try:
        req = json.loads(line)
    except ValueError:
        continue
    cmd, rid = req.get("cmd"), req.get("req_id")
    data = {}
    if cmd == "event_poll":
        time.sleep(int(req.get("data", {}).get("timeout_ms", 0)) / 1000.0)
        data = {"events": [], "count": 0, "pending": 0, "dropped": 0}
    print(json.dumps({"req_id": rid, "success": True, "data": data}), flush=True)
    if cmd in ("emu_stop", "shutdown"):
        sys.exit(0)
'''

#: Zkrácený výchozí limit odpovědi wrapperu [s].
SHORT_SEND_TIMEOUT_S = 1.0
#: timeout_ms pollu, delší než SHORT_SEND_TIMEOUT_S.
LONG_POLL_MS = 2500


class _FakePipeTransport(m._PipeTransport):
    """Pipe transport spouštějící falešný emulátor (Python skript)."""

    script: Path = Path()

    async def connect(self) -> None:
        self.process = subprocess.Popen(
            [sys.executable, str(self.script)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self._loop = asyncio.get_event_loop()


async def _cleanup():
    """Ukončí falešný emulátor (vlastní PID) a vynuluje stav bridge."""
    tr = m._transport
    if isinstance(tr, m._PipeTransport) and tr.process is not None \
            and tr.process.poll() is None:
        tr.process.kill()
        tr.process.wait()
    if m._reader_task is not None:
        m._reader_task.cancel()
        try:
            await m._reader_task
        except (asyncio.CancelledError, Exception):
            pass
    m._transport = None
    m._reader_task = None
    m._response_queue = None
    m._event_queue = None
    m._send_lock = None
    m._next_req_id = 1
    m._last_exit = None
    m._restart_notice = None


async def _amain(tmp: Path) -> int:
    script = tmp / "fake_emu.py"
    script.write_text(FAKE_EMU, encoding="utf-8")
    _FakePipeTransport.script = script
    m._create_transport = lambda: _FakePipeTransport("fake")
    m.TRANSPORT_KIND = "pipe"
    m.SEND_TIMEOUT_S = SHORT_SEND_TIMEOUT_S
    failed = 0
    try:
        res = json.loads(await m.emu_event_poll(timeout_ms=LONG_POLL_MS))
        if "error" in res or res.get("count") != 0:
            failed += 1
            print(f"FAIL A: long poll returned {res!r}", file=sys.stderr)
        else:
            print(f"OK   A: event_poll timeout_ms={LONG_POLL_MS} > "
                  f"SEND_TIMEOUT_S={SHORT_SEND_TIMEOUT_S} returns the backend reply")
        res = json.loads(await m.emu_event_poll(timeout_ms=0))
        if "error" in res:
            failed += 1
            print(f"FAIL B: pure poll returned {res!r}", file=sys.stderr)
        else:
            print("OK   B: event_poll timeout_ms=0 works")
    except Exception as e:  # noqa: BLE001 - výpis selhání testu
        failed += 1
        print(f"FAIL: {e!r}", file=sys.stderr)
    finally:
        await _cleanup()
    if failed:
        return 1
    print("\nall tests PASS")
    return 0


if __name__ == "__main__":
    _tmp = Path(tempfile.mkdtemp(prefix="mz_wrapper_poll_"))
    try:
        _rc = asyncio.run(_amain(_tmp))
    finally:
        shutil.rmtree(_tmp, ignore_errors=True)
    sys.exit(_rc)
