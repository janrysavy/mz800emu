#!/usr/bin/env python3
"""E2E regresní test: snapshot z jiné HW konfigurace se načte bezpečně (MCP pipe).

Chyba: load handler RAMdisku a IDE8 převzal ze snapshotu velikost
a připojení zařízení, ale nepřizpůsobil jim buffer ani otevřený obraz:

  - RAMdisk STD: buffer je alokovaný podle velikosti z INI. Snapshot
    s RAMdiskem 1 MB načtený do procesu s RAMdiskem 64 kB zkopíroval
    1 MB obsahu do 64kB bufferu (přetečení haldy) a čtení z vyšších bank
    šlo mimo buffer;
  - IDE8: snapshot s připojeným diskem načtený do procesu bez otevřeného
    obrazu nastavil ``connected`` bez souboru; první příkaz READ SECTOR
    pak volal ``fseek(NULL)``.

Scénáře (MZ-800):

  1  RAMdisk STD: proces A (1 MB, ``include_ramdisk = 1``) zapíše přes
     porty 0E9h/0EBh/0EAh vzor do banky 0Fh, ``snapshot_save``; proces B
     (64 kB) ``snapshot_load``, přečte vzor zpět přes porty (program
     v RAM) a vzor se musí shodovat; proces nesmí spadnout;
  2  IDE8: proces A (HDD0 připojený, obraz v dočasném adresáři)
     ``snapshot_save``; proces B (HDD0 odpojený) ``snapshot_load``; disk
     musí zůstat odpojený (``media_state``) a READ SECTOR (porty 78h-7Fh)
     nesmí shodit proces. Dřív zůstal "připojený" bez souboru a každé
     čtení skončilo chybou (na Windows ``fseek(NULL)`` vrátí chybu, jinde
     je to nedefinované chování).

Každý proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní
INI, ``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka).
"""

import base64
import json
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_snapshot_gdg_event import TestFailure, _find_exe, check  # noqa: E402

_REPO_ROOT = Path(__file__).resolve().parent.parent.parent

#: Programy v RAM od 3000h, výsledky od 3100h.
_PROG = 0x3000
_RESULT = 0x3100

#: Zápis vzoru 5Ah, A5h na RAMdisk STD banka 0Fh, offset FFF0h:
#: OUT (0E9h)=0Fh (banka), OUT (0EBh) s B=0FFh, A=0F0h (offset),
#: OUT (0EAh) dvakrát (offset se po každém bajtu zvýší); JR $.
_RD_WRITE = bytes.fromhex(
    "01E900" "3E0F" "ED79"
    "01EBFF" "3EF0" "ED79"
    "01EA00" "3E5A" "ED79" "3EA5" "ED79"
    "18FE")
#: Čtení vzoru zpět do 3100h-3101h stejnými porty; JR $.
_RD_READ = bytes.fromhex(
    "01E900" "3E0F" "ED79"
    "01EBFF" "3EF0" "ED79"
    "01EA00" "ED78" "320031" "ED78" "320131"
    "18FE")
#: IDE8 READ SECTOR (master, LBA, registr SECTOR = 1 = blok 0): HEAD
#: (7Eh) = 0E0h, SCOUNT (7Ah) = 1, SECTOR (7Bh) = 1, LCYL/HCYL = 0,
#: COMMAND (7Fh) = 20h; pak IN A,(7Fh) do 3100h; JR $.
_IDE_READ = bytes.fromhex(
    "3EE0" "D37E" "3E01" "D37A" "D37B" "AF" "D37C" "D37D"
    "3E20" "D37F" "DB7F" "320031" "18FE")
#: Kolik kroků programy potřebují (s rezervou, JR $ se opakuje).
_STEPS = 40


class PipeEmu:
    """Emulátor v pipe módu s vlastním INI (JSONL přes stdin/stdout)."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, tmp, ini_text):
        tmp.mkdir(parents=True, exist_ok=True)
        ini = tmp / "emu.ini"
        ini.write_text(ini_text, encoding="utf-8")
        args = [str(exe), "--mcp-pipe", "--headless", "--no-save-ini",
                "--no-first-run-windows", f"--cfg-dir={tmp}",
                f"--work-dir={tmp}", f"--config={ini}"]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                     encoding="utf-8", cwd=str(_REPO_ROOT))
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.rid = 0
        if not self._read(20.0, lambda m: m.get("type") == "hello"):
            raise TestFailure("no hello from emulator")

    def _pump(self):
        for line in iter(self.proc.stdout.readline, ""):
            self.q.put(line.strip())
        self.q.put(None)

    def _read(self, timeout, pred):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = self.q.get(timeout=max(0.05, deadline - time.time()))
            except queue.Empty:
                return None
            if line is None:
                return None
            if line.startswith("{"):
                msg = json.loads(line)
                if pred(msg):
                    return msg
        return None

    def call(self, cmd, data=None, timeout=30.0):
        """Request; vrátí data úspěšné odpovědi, jinak TestFailure."""
        self.rid += 1
        rid = self.rid
        req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
        try:
            self.proc.stdin.write(json.dumps(req) + "\n")
            self.proc.stdin.flush()
        except OSError as e:
            raise TestFailure(f"{cmd}: emulator gone ({e}, exit code {self.proc.poll()})")
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        if resp is None:
            raise TestFailure(f"no response to {cmd} "
                              f"(emulator exit code: {self.proc.poll()})")
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def run_program(self, prog):
        """Program na _PROG, PC = _PROG, _STEPS kroků."""
        self.call("pause")
        self.call("mem_write", {"addr": _PROG, "data_hex": prog.hex()})
        self.call("set_register", {"reg": "PC", "value": _PROG})
        for _ in range(_STEPS):
            self.call("step_into")

    def result(self, n):
        """n bajtů výsledku z _RESULT."""
        return base64.b64decode(self.call("mem_read", {"addr": _RESULT, "len": n})["data_b64"])

    def close(self):
        try:
            self.call("shutdown", timeout=10.0)
        except Exception:  # noqa: BLE001 - úklid nesmí zakrýt výsledek
            pass
        try:
            self.proc.wait(timeout=15.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()  # vlastní PID
            self.proc.wait()


def _ini(extra):
    """INI s vypnutým autoload/autosave breakpointů a danými sekcemi."""
    return "[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n" + extra


def case_ramdisk(exe, tmp):
    """Scénář 1: snapshot s RAMdiskem 1 MB do procesu s 64 kB."""
    snap = tmp / "rd.mzs"
    rd = "[RAMDISK]\nmr1r18_pluged = 1\nmr1r18_type = STANDARD\nmr1r18_size = {}\n"
    a = PipeEmu(exe, tmp / "a", _ini(rd.format("1M") + "[SNAPSHOT]\ninclude_ramdisk = 1\n"))
    try:
        a.run_program(_RD_WRITE)
        a.run_program(_RD_READ)
        check(a.result(2) == b"\x5a\xa5", "pattern written to RAMdisk bank 0Fh (1 MB)")
        a.call("snapshot_save", {"path": str(snap)})
    finally:
        a.close()
    b = PipeEmu(exe, tmp / "b", _ini(rd.format("64K")))
    try:
        b.call("pause")
        b.call("mem_write", {"addr": _RESULT, "data_hex": "0000"})
        b.call("snapshot_load", {"path": str(snap)})
        b.run_program(_RD_READ)
        got = b.result(2)
        check(got == b"\x5a\xa5", f"RAMdisk 1 MB from snapshot readable in 64 kB process ({got.hex()})")
    finally:
        b.close()
    check(b.proc.returncode == 0, f"emulator exited normally (exit code {b.proc.returncode})")


def case_ide8(exe, tmp):
    """Scénář 2: snapshot s připojeným HDD0 do procesu bez obrazu."""
    snap = tmp / "ide.mzs"
    img = tmp / "hdd0.img"
    a = PipeEmu(exe, tmp / "ia", _ini(f"[IDE8]\nhdd0_connected = 1\nhdd0_filepath = {img.as_posix()}\n"))
    try:
        a.call("pause")
        a.call("snapshot_save", {"path": str(snap)})
    finally:
        a.close()
    b = PipeEmu(exe, tmp / "ib", _ini("[IDE8]\nhdd0_connected = 0\n"))
    try:
        b.call("pause")
        b.call("snapshot_load", {"path": str(snap)})
        slots = {s["slot"]: s for s in b.call("media_state")["slots"]}
        check(not slots["ide8"]["inserted"],
              "HDD0 stays disconnected without an open image after load")
        b.run_program(_IDE_READ)
        b.call("get_registers")
    finally:
        b.close()
    check(b.proc.returncode == 0, f"READ SECTOR after load did not crash (exit code {b.proc.returncode})")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=170)
    exe = _find_exe("mz800emu")
    if not exe:
        print("SKIP: mz800emu binary not found")
        return 77
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_snap_safety_"))
    failed = []
    for name, case in (("ramdisk", case_ramdisk), ("ide8", case_ide8)):
        print(f"== {name}")
        try:
            case(exe, tmp)
        except TestFailure as e:
            print(f"  FAIL: {e}")
            failed.append(name)
    if failed:
        print(f"FAIL: {', '.join(failed)}; artifacts kept in {tmp}")
        return 1
    shutil.rmtree(tmp, ignore_errors=True)
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
