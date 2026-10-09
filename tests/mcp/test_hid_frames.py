#!/usr/bin/env python3
"""E2E test: HID nástroje MCP počítají snímky emulace; run_until_addr
a snapshot_load za běhu emulace vracejí chybu s důvodem (pipe).

Reprodukuje podněty z inboxu emu-experiments (mz-sw-atlas, 8.-9. 10. 2026):

1. ``input_send_key(frames=N)`` při MAX SPEED nechal proběhnout stovky
   snímků emulace místo N (naměřeno 402 pro N = 4) a při normální
   rychlosti na statické obrazovce 13. Čekalo se v MCP vlákně na čítač
   VYKRESLENÝCH snímků, který při MAX SPEED roste nejvýš jednou za 20 ms
   reálného času a bez změny obrazu neroste vůbec. Nově sekvenci provádí
   emu vlákno na hranicích snímků emulace.
2. ``run_until_addr`` na běžícím emulátoru odpověděl ``running: true``,
   ale emulátor jen pauzl (mimo cíl).
3. ``snapshot_load`` na běžícím emulátoru vrátil jen "snapshot_load failed".

Pro každou nalezenou binárku (mz700emu-pal, mz700emu-ntsc, mz800emu,
mz1500emu) spustí izolovaný proces v pipe módu a ověří:

  A  z pauzy, normální rychlost: ``input_send_key`` (4 snímky) a
     ``input_send_keys_with_delays`` (3+2, 5+0 = 10 snímků) posunou čítač
     snímků emulace (``get_periph_gdg.total_screens``) PŘESNĚ o požadovaný
     počet a emulace zůstane v pauze; ``emu_frames`` v odpovědi souhlasí.
  B  totéž při MAX SPEED (dřív stovky snímků).
  C  za běhu, MAX SPEED i normální rychlost: ``emu_frames`` = požadavek,
     ``complete``, emulace dál běží; ``input_send_keys`` a
     ``input_send_joystick`` (MZ-800, MZ-1500) také.
  D  za běhu na promptu ROM: klávesa držená 4 snímky dosedne
     (``landing_verified``) - sonda dosednutí funguje i v nové cestě.
  E  ``run_until_addr`` za běhu: chyba "Emulator is running", emulace
     dál běží (nepauzne se); z pauzy doběhne na cíl (PC = cíl, pauza);
     další ``run`` běží dál (dočasný breakpoint po pauze zrušen - dřív
     v headless zůstal a emulaci na cíli znovu zastavil).
  F  ``snapshot_load`` za běhu: chyba s důvodem "not paused", emulace
     dál běží; po pauze se tentýž soubor nahraje.
  G  breakpoint během sekvence: ``input_send_key`` a
     ``input_send_keys_with_delays`` vrátí ``interrupted: true``, emulace
     stojí na breakpointu a virtuální klávesnice je uvolněná.

Zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka nenalezena).
"""

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

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent

#: (jméno binárky, má joystick)
_PLATFORMS = [
    ("mz700emu-pal", False),
    ("mz700emu-ntsc", False),
    ("mz800emu", True),
    ("mz1500emu", True),
]

#: Exec adresa testovacího programu.
_EXEC = 0x1200

#: Tělo programu: 1200h DI; 1201h INC A; 1202h JR 1201h.
_PROGRAM = bytes([0xF3, 0x3C, 0x18, 0xFD])

#: Cíl run_until_addr uvnitř smyčky programu.
_RUN_TO = 0x1202

#: Kolik snímků nechat emulaci běžet po startu (boot ROM na prompt).
_BOOT_FRAMES = 150


class TestFailure(Exception):
    """Selhání kontroly testu (zpráva je pro uživatele, anglicky)."""


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def _find_exe(name):
    """Najde binárku v kořeni repa (s příponou .exe i bez), jinak None."""
    for c in (_REPO_ROOT / (name + ".exe"), _REPO_ROOT / name):
        if c.is_file():
            return c
    return None


def _make_mzf(path):
    """Vytvoří MZF: atribut 01h, fstrt = fexec = 1200h, tělo _PROGRAM."""
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"HID TEST\r"
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = len(_PROGRAM).to_bytes(2, "little")
    hdr[0x14:0x16] = _EXEC.to_bytes(2, "little")
    hdr[0x16:0x18] = _EXEC.to_bytes(2, "little")
    path.write_bytes(bytes(hdr) + _PROGRAM)


class PipeEmu:
    """Emulátor v pipe módu: JSONL request/response přes stdin/stdout."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, tmp):
        ini = tmp / "emu.ini"
        ini.write_text("", encoding="utf-8")
        args = [str(exe), "--mcp-pipe", "--headless", "--no-save-ini",
                "--no-first-run-windows", f"--cfg-dir={tmp}",
                f"--work-dir={tmp}", f"--config={ini}"]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                     encoding="utf-8", cwd=str(_REPO_ROOT))
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.rid = 0
        self.dead = False
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
            if not line.startswith("{"):
                continue
            msg = json.loads(line)
            if pred(msg):
                return msg
        return None

    def request(self, cmd, data=None, timeout=30.0):
        """Pošle request a vrátí celou response (i neúspěšnou)."""
        self.rid += 1
        rid = self.rid
        req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        if resp is None:
            self.dead = True
            raise TestFailure(f"no response to {cmd} within {timeout:.0f} s "
                              f"(emulator exit code: {self.proc.poll()})")
        return resp

    def call(self, cmd, data=None, timeout=30.0):
        """Pošle request a vrátí data úspěšné response (jinak FAIL)."""
        resp = self.request(cmd, data, timeout)
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def screens(self):
        """Čítač snímků emulace (g_gdg.total_elapsed.screens)."""
        return int(self.call("get_periph_gdg")["total_screens"])

    def paused(self):
        """True = emulace v pauze."""
        return bool(self.call("get_state").get("paused"))

    def close(self):
        """Ukončí proces přes shutdown, jinak zabije vlastní PID."""
        if not self.dead:
            try:
                self.call("shutdown", timeout=10.0)
            except Exception:  # noqa: BLE001 - úklid nesmí zakrýt výsledek
                pass
        try:
            self.proc.wait(timeout=15.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()  # vlastní PID
            self.proc.wait()


_DELAYS = [{"key": "A", "hold_frames": 3, "gap_frames": 2},
           {"key": "B", "hold_frames": 5, "gap_frames": 0}]


def _phase_paused(emu, speed):
    """A/B: z pauzy posune sekvence čítač snímků přesně o požadavek."""
    emu.call("set_speed", {"mode": speed})
    emu.call("pause")
    check(emu.paused(), f"[{speed}] emulator paused before sequence")

    s0 = emu.screens()
    r = emu.call("input_send_key", {"key": "SHIFT", "frames": 4})
    s1 = emu.screens()
    check(s1 - s0 == 4, f"[{speed}] paused send_key(frames=4): emulation "
                        f"advanced exactly 4 frames (got {s1 - s0})")
    check(r.get("emu_frames") == 4 and r.get("complete") is True,
          f"[{speed}] send_key reply emu_frames=4, complete ({r})")
    check(emu.paused(), f"[{speed}] emulator paused again after send_key")

    s0 = emu.screens()
    r = emu.call("input_send_keys_with_delays", {"events": _DELAYS})
    s1 = emu.screens()
    check(s1 - s0 == 10, f"[{speed}] paused send_keys_with_delays (3+2, 5+0): "
                         f"exactly 10 frames (got {s1 - s0})")
    check(r.get("total_frames") == 10 and r.get("emu_frames") == 10
          and r.get("events_processed") == 2 and r.get("complete") is True,
          f"[{speed}] send_keys_with_delays reply ({r})")
    check(emu.paused(), f"[{speed}] emulator paused again after sequence")


def _phase_running(emu, speed, has_joy):
    """C: za běhu sedí emu_frames a emulace dál běží."""
    emu.call("set_speed", {"mode": speed})
    emu.call("run")
    check(not emu.paused(), f"[{speed}] emulator running")

    t0 = time.monotonic()
    r = emu.call("input_send_key", {"key": "SHIFT", "frames": 4})
    dt = time.monotonic() - t0
    check(r.get("emu_frames") == 4 and r.get("complete") is True,
          f"[{speed}] running send_key(frames=4): emu_frames=4 ({r})")
    check(dt < 5.0, f"[{speed}] running send_key answered in {dt:.2f} s")

    r = emu.call("input_send_keys_with_delays", {"events": _DELAYS})
    check(r.get("emu_frames") == 10 and r.get("complete") is True,
          f"[{speed}] running send_keys_with_delays: emu_frames=10 ({r})")

    r = emu.call("input_send_keys", {"text": "[\"SHIFT\",\"SHIFT\",\"SHIFT\"]",
                                     "encoding": "key_names",
                                     "frame_per_key": 2})
    # 3 x 2 snímky držení + 2 mezery po 1 snímku (opakovaná klávesa se
    # musí zaregistrovat dvakrát).
    check(r.get("keys_sent") == 3 and r.get("emu_frames") == 8
          and r.get("total_frames") == 8 and r.get("complete") is True,
          f"[{speed}] running send_keys 3 x 2 frames + 2 gaps: emu_frames=8 ({r})")

    if has_joy:
        r = emu.call("input_send_joystick", {"port": 0, "state": 1, "frames": 5})
        check(r.get("emu_frames") == 5 and r.get("complete") is True,
              f"[{speed}] running send_joystick(frames=5): emu_frames=5 ({r})")

    check(not emu.paused(), f"[{speed}] emulator still running after sequences")


def _phase_landing(emu):
    """D: na promptu ROM klávesa držená 4 snímky dosedne.

    Klávesa A: ROM po startu neskenuje všechny sloupce (SHIFT, RETURN
    a SPACE nedosednou ani na masteru před touto změnou, ověřeno
    9. 10. 2026 na mz800emu), sloupec A ano.
    """
    emu.call("set_speed", {"mode": "normal"})
    emu.call("run")
    r = emu.call("input_send_key", {"key": "A", "frames": 4})
    check(r.get("landing_verified") is True,
          f"ROM prompt: A held 4 frames landed ({r})")


def _wait_paused(emu, timeout=5.0):
    """Počká na pauzu emulace (run_until_addr dosáhne cíle)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if emu.paused():
            return True
        time.sleep(0.05)
    return False


def _phase_run_until_addr(emu, mzf):
    """E: run_until_addr za běhu = chyba bez pauzy; z pauzy doběhne."""
    emu.call("set_speed", {"mode": "normal"})
    emu.call("media_run_mzf", {"path": str(mzf)})
    emu.call("run")
    check(not emu.paused(), "program running")

    resp = emu.request("run_until_addr", {"addr": _RUN_TO})
    err = resp.get("error") or ""
    check(not resp.get("success") and "Emulator is running" in err,
          f"run_until_addr while running is an error ({err!r})")
    check(not emu.paused(), "emulator still running after rejected run_until_addr")

    emu.call("pause")
    r = emu.call("run_until_addr", {"addr": _RUN_TO})
    check(r.get("running") is True, "run_until_addr from pause started")
    check(_wait_paused(emu), "run_until_addr stopped")
    pc = int(emu.call("get_registers")["PC"])
    check(pc == _RUN_TO, f"run_until_addr stopped at target (PC={pc:04X}h)")

    # Dočasný breakpoint platí jen do pauzy: další run se na cíli znovu
    # nezastaví (dřív v headless zůstal aktivní - emulator_pause ho mazal
    # jen s otevřeným oknem debuggeru).
    emu.call("run")
    time.sleep(0.5)
    check(not emu.paused(), "run after run_until_addr keeps running "
                            "(temporary breakpoint removed)")
    emu.call("pause")


def _phase_interrupt(emu, mzf):
    """G: breakpoint během sekvence ji přeruší a klávesu uvolní.

    Program běží ve smyčce přes _RUN_TO, breakpoint na ní zastaví
    emulaci hned po rozběhu sekvence (pauza zvenku).
    """
    emu.call("set_speed", {"mode": "normal"})
    emu.call("media_run_mzf", {"path": str(mzf)})
    emu.call("pause")
    bp_id = emu.call("bp_add", {"addr": _RUN_TO})["id"]
    try:
        r = emu.call("input_send_key", {"key": "A", "frames": 50})
        check(r.get("interrupted") is True and r.get("complete") is False
              and r.get("emu_frames", 99) < 50,
              f"send_key cut short by breakpoint: interrupted ({r})")
        check(emu.paused(), "emulator stays paused at the breakpoint")
        pc = int(emu.call("get_registers")["PC"])
        check(pc == _RUN_TO, f"stopped at the breakpoint (PC={pc:04X}h)")
        kb = emu.call("get_input_keyboard_state")
        check(all(v == 255 for v in kb["virtual_matrix"]),
              f"interrupted key released (virtual matrix {kb['virtual_matrix']})")

        r = emu.call("input_send_keys_with_delays", {"events": [
            {"key": "A", "hold_frames": 20, "gap_frames": 5},
            {"key": "B", "hold_frames": 20}]})
        check(r.get("interrupted") is True and r.get("complete") is False
              and r.get("events_processed") == 0,
              f"send_keys_with_delays cut short by breakpoint ({r})")
        kb = emu.call("get_input_keyboard_state")
        check(all(v == 255 for v in kb["virtual_matrix"]),
              "interrupted sequence released its key")
    finally:
        emu.call("bp_remove", {"id": bp_id})


def _phase_snapshot_load(emu, tmp):
    """F: snapshot_load za běhu = chyba s důvodem; po pauze projde."""
    snap = tmp / "hid_test.mzs"
    emu.call("pause")
    emu.call("snapshot_save", {"path": str(snap)})
    emu.call("run")
    check(not emu.paused(), "emulator running before snapshot_load")

    resp = emu.request("snapshot_load", {"path": str(snap)})
    err = resp.get("error") or ""
    check(not resp.get("success") and "not paused" in err,
          f"snapshot_load while running names the reason ({err!r})")
    check(not emu.paused(), "emulator still running after rejected snapshot_load")

    emu.call("pause")
    r = emu.call("snapshot_load", {"path": str(snap)})
    check(r.get("ok") is True, "snapshot_load from pause succeeds")


def run_platform(exe, has_joy):
    """Všechny fáze pro jednu binárku. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    tmp = Path(tempfile.mkdtemp(prefix="hid_frames_"))
    emu = None
    try:
        emu = PipeEmu(exe, tmp)
        mzf = tmp / "hid_test.mzf"
        _make_mzf(mzf)
        emu.call("pause")
        emu.call("run", {"frames": _BOOT_FRAMES})
        _phase_paused(emu, "normal")
        _phase_paused(emu, "max")
        _phase_running(emu, "max", has_joy)
        _phase_running(emu, "normal", has_joy)
        _phase_landing(emu)
        _phase_snapshot_load(emu, tmp)
        _phase_run_until_addr(emu, mzf)
        _phase_interrupt(emu, mzf)
        return True
    except TestFailure as e:
        print(f"  FAILED: {e}")
        return False
    finally:
        if emu is not None:
            emu.close()
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    emu_test_proc.install(deadline_s=170)
    found = [(exe, joy) for name, joy in _PLATFORMS
             if (exe := _find_exe(name)) is not None]
    if not found:
        print("SKIP: no emulator binary found in repository root")
        return 77
    ok = all([run_platform(exe, joy) for exe, joy in found])
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
