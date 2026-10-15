#!/usr/bin/env python3
"""E2E test CLI voleb virtuální kazety: --cmt, --cmt-play, --cmt-play-pause, --cmt-boot.

Pro binárky mz700emu-pal, mz700emu-ntsc, mz800emu a mz1500emu v kořeni repa
(MCP pipe, izolovaný dočasný ``--cfg-dir``/``--work-dir``, vlastní INI):

  A  ``--cmt prog.mzf``: páska vložená ve STOP, CMT hack (ve výchozím INI
     zapnutý) vypnutý, ``image_basename`` = prog.mzf;
  B  ``--cmt tone.wav --cmt-play``: přípona .wav se pozná, transport PLAY.
     Při bootu z ROM ji inicializace ROM (0741h-0744h: 8Ah a 07h do
     0E003h = náběžná hrana PC3, kterou emulátor bere jako přepnutí
     motoru) zastaví do PLAY + PAUSE; s ``--run-mzf`` (ROM se přeskočí)
     páska hraje bez pauzy;
  C  ``--cmt prog.mzf --cmt-play-pause`` (bez i s ``--run-mzf``): po startu
     PLAY + PAUSE; program v RAM přepne motor jako ROM (@MGON: BSR 06h
     a 07h na 0E003h) a pauza se zruší - na všech platformách (ROM MZ-700,
     MZ-800 i MZ-1500 obsahuje na 069Fh stejnou rutinu zapnutí motoru);
  D  jen MZ-800: ``--cmt-boot`` (s přepínačem SW1 v poloze 700 i 800):
     emulátor drží C, IPL zvolí kazetu, ROM zapne motor, nahraje MZF
     z pásky a spustí ho - exec breakpoint na 1200h z ``.bpt`` zastaví CPU
     na první instrukci programu; klávesa C je potom uvolněná;
  E  CMT hack vypnutý volbou ``--cmt`` platí jen pro běh: proces s ukládáním
     INI po ``shutdown`` zapíše ``[CMTHACK] enable = 1`` (jak bylo před
     startem);
  F  chybné použití skončí před startem nenulovým kódem a anglickou
     zprávou: chybějící soubor, neznámá přípona, modifikátor bez --cmt,
     --cmt-play spolu s --cmt-play-pause, --cmt-boot s --run-mzf nebo
     s --cmt-play, --cmt-boot mimo MZ-800.

Zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka nenalezena).
"""

import base64
import json
import queue
import subprocess
import sys
import tempfile
import threading
import time
import wave
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
# --run-mzf v pipe režimu povolené není: varianty s ním běží přes MCP TCP.
from test_run_mzf_state import TcpEmu, TestFailure  # noqa: E402

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent

#: (jméno binárky, jméno .bpt souboru podle architektury, je MZ-800)
_PLATFORMS = [
    ("mz700emu-pal", "mz700-breakpoints.bpt", False),
    ("mz700emu-ntsc", "mz700-breakpoints.bpt", False),
    ("mz800emu", "mz800-breakpoints.bpt", True),
    ("mz1500emu", "mz1500-breakpoints.bpt", False),
]

#: Exec adresa testovacího MZF; program 1200h JR 1200h.
_EXEC = 0x1200
_PROGRAM = bytes([0x18, 0xFE])

#: Program přepnutí motoru (jako ROM @MGON): 3000h LD A,06h; LD (0E003h),A;
#: INC A; LD (0E003h),A; JR $.
_MOTOR_ADDR = 0x3000
_MOTOR_PROG = bytes([0x3E, 0x06, 0x32, 0x03, 0xE0, 0x3C, 0x32, 0x03, 0xE0, 0x18, 0xFE])

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


def _make_mzf(path, body=_PROGRAM):
    """Vytvoří MZF: atribut 01h, fstrt = fexec = 1200h."""
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"CLI CMT TEST\r"
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = len(body).to_bytes(2, "little")
    hdr[0x14:0x16] = _EXEC.to_bytes(2, "little")
    hdr[0x16:0x18] = _EXEC.to_bytes(2, "little")
    path.write_bytes(bytes(hdr) + body)


def _make_wav(path):
    """Vytvoří WAV (8 bit mono 22,05 kHz, 20 s obdélníku ~1 kHz).

    Musí hrát déle, než test (start přes TCP) stav pásky přečte.
    """
    rate = 22050
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(rate)
        w.writeframes(bytes((0xC0 if (i // 11) % 2 else 0x40) for i in range(rate * 20)))


def _write_ini(path, extra=""):
    """INI testu; ``extra`` se připojí (např. sekce [BREAKPOINTS])."""
    path.write_text(extra, encoding="utf-8")


class PipeEmu:
    """Emulátor v pipe módu: JSONL request/response přes stdin/stdout."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, cfg_dir, ini, extra_args=(), save_ini=False):
        args = [str(exe), "--mcp-pipe", "--headless",
                "--no-first-run-windows", f"--cfg-dir={cfg_dir}",
                f"--work-dir={cfg_dir}", f"--config={ini}", *extra_args]
        if not save_ini:
            args.insert(3, "--no-save-ini")
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                     encoding="utf-8", cwd=str(_REPO_ROOT))
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.rid = 0
        if not self._read(20.0, lambda m: m.get("type") == "hello"):
            raise TestFailure(f"no hello from emulator (exit code: {self.proc.poll()})")

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

    def call(self, cmd, data=None, timeout=30.0):
        """Pošle request a vrátí data úspěšné response (jinak TestFailure)."""
        self.rid += 1
        rid = self.rid
        req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        if resp is None:
            raise TestFailure(f"no response to {cmd} (emulator exit code: {self.proc.poll()})")
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def close(self):
        """Ukončí proces přes shutdown, jinak zabije vlastní PID."""
        try:
            self.call("shutdown", timeout=10.0)
        except Exception:  # noqa: BLE001 - úklid nesmí zakrýt výsledek
            pass
        try:
            self.proc.wait(timeout=15.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()  # vlastní PID
            self.proc.wait()


def _start(exe, cfg_dir, ini, cmt_args, mzf_for_run=None):
    """Spustí emulátor: pipe, s ``mzf_for_run`` headless MCP TCP a --run-mzf."""
    if mzf_for_run is None:
        return PipeEmu(exe, cfg_dir, ini, cmt_args)
    port = emu_test_proc.free_tcp_port()
    args = [str(exe), "--headless", "--no-save-ini", "--no-first-run-windows",
            f"--cfg-dir={cfg_dir}", f"--work-dir={cfg_dir}", f"--config={ini}",
            f"--mcp-tcp-port={port}", *cmt_args, "--run-mzf", str(mzf_for_run)]
    return TcpEmu(args, cfg_dir, port)


def _cmt(emu):
    """Stav virtuální kazety (get_periph_cmt)."""
    return emu.call("get_periph_cmt")


def _wait_paused(emu, what, timeout=30.0):
    """Počká, až emulace stojí (breakpoint nebo pauza)."""
    deadline = time.monotonic() + timeout
    while not emu.call("get_state").get("paused"):
        if time.monotonic() > deadline:
            raise TestFailure(f"emulation did not pause ({what})")
        time.sleep(0.1)


def _phase_insert(exe, tmp):
    """A: --cmt jen vloží pásku, hack vypne."""
    d = tmp / "a"
    d.mkdir()
    mzf = d / "prog.mzf"
    _make_mzf(mzf)
    ini = d / "emu.ini"
    _write_ini(ini)
    emu = PipeEmu(exe, d, ini, ("--cmt", str(mzf)))
    try:
        emu.call("pause")
        st = _cmt(emu)
        check(st["filled"] and st["image_basename"] == "prog.mzf",
              f"A: tape inserted ({st['image_basename']!r})")
        check(st["state"] == "stop" and not st["paused"], f"A: transport STOP ({st['state']})")
        check(not st["cmthack_enabled"], "A: CMT hack disabled")
    finally:
        emu.close()


def _phase_play_wav(exe, tmp, run_mzf):
    """B: --cmt s WAV a --cmt-play (bez a s --run-mzf)."""
    d = tmp / ("b_run" if run_mzf else "b")
    d.mkdir()
    wav = d / "tone.wav"
    _make_wav(wav)
    mzf = d / "prog.mzf"
    _make_mzf(mzf)
    ini = d / "emu.ini"
    # Bez cpu_boost: s ním přehrávání zapne MAX SPEED a WAV by dohrál
    # (STOP) dřív, než test stav přečte.
    _write_ini(ini, "[CMT]\ncpu_boost = 0\n")
    emu = _start(exe, d, ini, ("--cmt", str(wav), "--cmt-play"), mzf if run_mzf else None)
    tag = "B (--run-mzf)" if run_mzf else "B"
    try:
        emu.call("pause")
        st = _cmt(emu)
        check(st["filled"] and st["image_basename"] == "tone.wav", f"{tag}: WAV tape inserted")
        # Bez --run-mzf motor zastaví inicializace ROM (viz docstring).
        check(st["state"] == "play" and st["paused"] == (not run_mzf),
              f"{tag}: transport PLAY, paused={not run_mzf} ({st['state']}, paused={st['paused']})")
    finally:
        emu.close()


def _phase_play_pause(exe, tmp, run_mzf):
    """C: --cmt-play-pause; přepnutí motoru programem zruší pauzu."""
    d = tmp / ("c_run" if run_mzf else "c")
    d.mkdir()
    mzf = d / "prog.mzf"
    _make_mzf(mzf)
    ini = d / "emu.ini"
    _write_ini(ini)
    emu = _start(exe, d, ini, ("--cmt", str(mzf), "--cmt-play-pause"), mzf if run_mzf else None)
    tag = "C (--run-mzf)" if run_mzf else "C"
    try:
        emu.call("pause")
        st = _cmt(emu)
        check(st["state"] == "play" and st["paused"],
              f"{tag}: transport PLAY + PAUSE ({st['state']}, paused={st['paused']})")
        emu.call("mem_write", {"addr": _MOTOR_ADDR, "data_hex": _MOTOR_PROG.hex()})
        emu.call("set_register", {"reg": "PC", "value": _MOTOR_ADDR})
        emu.call("run", {"frames": 2})
        _wait_paused(emu, "after motor program")
        st = _cmt(emu)
        check(st["state"] == "play" and not st["paused"],
              f"{tag}: motor toggle releases the pause ({st['state']}, paused={st['paused']})")
    finally:
        emu.close()


def _phase_boot(exe, tmp, bpt_name, mode):
    """D: --cmt-boot nahraje a spustí MZF z pásky (jen MZ-800)."""
    d = tmp / f"d{mode}"
    d.mkdir()
    mzf = d / "prog.mzf"
    _make_mzf(mzf)
    ini = d / "emu.ini"
    # Poloha SW1 z INI (--mode-switch v pipe režimu povolené není).
    _write_ini(ini, "[BREAKPOINTS]\nauto_load=1\nauto_save=0\n"
               f"[MZ800]\nmode_switch = MZ{mode}\n")
    (d / bpt_name).write_text(json.dumps({
        "breakpoints": [{"id": 1, "name": "exec", "addr": _EXEC, "type": "PC_EXEC",
                         "enabled": True, "skip_count": 0, "hit_count": 0,
                         "expr": None, "action": None, "edge_triggered": False}],
        "groups": []}), encoding="utf-8")
    emu = PipeEmu(exe, d, ini, ("--cmt", str(mzf), "--cmt-boot"))
    try:
        # Nahrání MZF z pásky normální rychlostí trvá jednotky sekund.
        _wait_paused(emu, f"exec breakpoint after --cmt-boot (mode {mode})", timeout=90.0)
        pc = emu.call("get_registers")["PC"]
        check(pc == _EXEC, f"D{mode}: ROM loaded the tape and started the program (PC {pc:04X}h)")
        body = base64.b64decode(emu.call("mem_read", {"addr": _EXEC, "len": len(_PROGRAM)})["data_b64"])
        check(body == _PROGRAM, f"D{mode}: program body in RAM ({body.hex()})")
        kbd = emu.call("get_input_keyboard_state")
        check(kbd.get("pressed_count") == 0,
              f"D{mode}: no key held after the motor started ({kbd.get('pressed_keys')})")
    finally:
        emu.close()


def _phase_ini_session_only(exe, tmp):
    """E: vypnutí CMT hacku volbou --cmt se do INI neuloží."""
    d = tmp / "e"
    d.mkdir()
    mzf = d / "prog.mzf"
    _make_mzf(mzf)
    ini = d / "emu.ini"
    _write_ini(ini, "[CMTHACK]\nenable = 1\n")
    emu = PipeEmu(exe, d, ini, ("--cmt", str(mzf)), save_ini=True)
    try:
        emu.call("pause")
        check(not _cmt(emu)["cmthack_enabled"], "E: CMT hack disabled for the session")
    finally:
        emu.close()
    text = ini.read_text(encoding="utf-8", errors="replace")
    section = text.split("[CMTHACK]", 1)[1].split("[", 1)[0] if "[CMTHACK]" in text else ""
    enable = [ln.split("=", 1)[1].strip() for ln in section.splitlines()
              if ln.split("=", 1)[0].strip() == "enable"]
    check(enable == ["1"], f"E: INI keeps [CMTHACK] enable = 1 ({enable})")


def _run_invalid(exe, tmp, args, expect):
    """F: spuštění s chybnými volbami musí skončit chybou se zprávou."""
    d = tmp / "f"
    d.mkdir(exist_ok=True)
    ini = d / "emu.ini"
    _write_ini(ini)
    cmd = [str(exe), "--headless", "--no-save-ini", "--no-first-run-windows",
           f"--cfg-dir={d}", f"--work-dir={d}", f"--config={ini}", *args]
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, encoding="utf-8", errors="replace",
                           timeout=20, cwd=str(_REPO_ROOT))
    except subprocess.TimeoutExpired:
        raise TestFailure(f"F: {' '.join(args)}: emulator did not exit")
    check(r.returncode != 0 and expect in r.stdout,
          f"F: {' '.join(a if not a.endswith(('.mzf', '.xyz')) else Path(a).name for a in args)}"
          f" -> exit {r.returncode}, message {expect!r}")


def _phase_invalid(exe, tmp, is_mz800):
    """F: všechny chybové kombinace."""
    d = tmp / "f"
    d.mkdir(exist_ok=True)
    mzf = d / "prog.mzf"
    _make_mzf(mzf)
    bad = d / "prog.xyz"
    bad.write_bytes(b"x")
    missing = d / "missing.mzf"
    _run_invalid(exe, tmp, ["--cmt", str(missing)], "--cmt: file not found")
    _run_invalid(exe, tmp, ["--cmt", str(bad)], "--cmt: unsupported tape file extension")
    _run_invalid(exe, tmp, ["--cmt-play"], "--cmt-play requires --cmt")
    _run_invalid(exe, tmp, ["--cmt-play-pause"], "--cmt-play-pause requires --cmt")
    _run_invalid(exe, tmp, ["--cmt", str(mzf), "--cmt-play", "--cmt-play-pause"],
                 "--cmt-play and --cmt-play-pause cannot be combined")
    if is_mz800:
        _run_invalid(exe, tmp, ["--cmt-boot"], "--cmt-boot requires --cmt")
        _run_invalid(exe, tmp, ["--cmt", str(mzf), "--cmt-boot", "--run-mzf", str(mzf)],
                     "--cmt-boot cannot be combined with --run-mzf")
        _run_invalid(exe, tmp, ["--cmt", str(mzf), "--cmt-boot", "--cmt-play"],
                     "--cmt-boot cannot be combined with --cmt-play")
    else:
        _run_invalid(exe, tmp, ["--cmt", str(mzf), "--cmt-boot"],
                     "--cmt-boot is only supported on MZ-800")


def run_platform(exe, bpt_name, is_mz800):
    """Všechny fáze pro jednu binárku. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    tmp = Path(tempfile.mkdtemp(prefix="cli_cmt_"))
    try:
        _phase_insert(exe, tmp)
        for run_mzf in (False, True):
            _phase_play_wav(exe, tmp, run_mzf)
            _phase_play_pause(exe, tmp, run_mzf)
        if is_mz800:
            _phase_boot(exe, tmp, bpt_name, 800)
            _phase_boot(exe, tmp, bpt_name, 700)
        _phase_ini_session_only(exe, tmp)
        _phase_invalid(exe, tmp, is_mz800)
        return True
    except TestFailure as e:
        print(f"  FAILED: {e}")
        return False
    finally:
        emu_test_proc.rmtree_retry(tmp)


def main():
    emu_test_proc.install(deadline_s=290)
    found = [(exe, bpt, m) for name, bpt, m in _PLATFORMS
             if (exe := _find_exe(name)) is not None]
    if not found:
        print("SKIP: no emulator binary found in repository root")
        return 77
    ok = all([run_platform(*f) for f in found])
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
