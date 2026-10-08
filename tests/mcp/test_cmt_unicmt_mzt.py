#!/usr/bin/env python3
"""E2E test: MZT s hlavičkou CMTSPEED zařízení UniCMT a vlastní délky pulzů (MCP pipe).

Páska MZT = LOADER + hlavička CMTSPEED + DATA. ROM MZ-800 nahraje z boot
menu (volba C) díl LOADER výchozí rychlostí 1:1; LOADER zavolá ROM rutiny
RHEAD (0027h) a RDATA (002Ah) a nahraje díl DATA (512 B na 4000h), který
přichází rychlostí z hlavičky CMTSPEED. Výsledek zapíše na 1300h: 55h =
nahráno, EEh = chyba ROM (CY = 1).

  A  hlavička 1x (470/494/240/278 µs = nominál ROM): seznam bloků hlavičku
     neobsahuje, DATA má vlastní rychlost (block_speed "set", cmt_speed 10,
     pulses_us z hlavičky); ROM DATA nahraje (hlavička se nepřehrála
     a ROM za LOADERem našla DATA).
  B  stejná páska s hlavičkou 3x (156/164/80/92 µs): ROM pevnou rychlostí
     1:1 DATA nenahraje - rychlost z hlavičky se opravdu použila.
  C  páska z B, ale DATA přes MCP cmt_tape_block_speed s pulses_us
     nastavené na 1x: ROM DATA nahraje (per-blok vlastní pulzy).
  D  výchozí vlastní pulzy přes cmt_set_property custom_pulses: stav CMT
     hlásí cmtspeed 10 a pulzy; neplatné délky se odmítnou.
  E  výchozí vlastní pulzy se uloží do INI (SPEED_CUSTOM + 4 klíče
     mz_custom_pulse_*_us) a po restartu se z něj načtou.

Každý proces běží izolovaně (dočasný --cfg-dir/--work-dir, vlastní INI
s vypnutým CMT hackem, --no-save-ini kromě fáze E); zabíjí se jen vlastní
PID.
Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import json
import os
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
_EXE_CANDIDATES = [_REPO_ROOT / "mz800emu.exe", _REPO_ROOT / "mz800emu"]

#: Adresa a délka těla dílu DATA.
_DATA_ADDR = 0x4000
_DATA_SIZE = 512
#: Adresa výsledku LOADERu (55h OK, EEh chyba ROM).
_FLAG_ADDR = 0x1300

#: Délky pulzů hlaviček CMTSPEED (µs) - soubory 1xspeed/3xspeed.mzf UniCMT FW 0.5.
_UNICMT_1X = [470, 494, 240, 278]
_UNICMT_3X = [156, 164, 80, 92]

#: Kolik snímků nechat ROM na nahrání obou dílů (1:1 asi 25 s pásky = 1250 snímků).
_LOAD_FRAMES = 2500


class TestFailure(Exception):
    """Selhání kontroly testu (zpráva je pro uživatele, anglicky)."""


def _find_exe():
    """Najde binárku mz800emu (přednost má proměnná MZ_EMU)."""
    env = os.environ.get("MZ_EMU")
    if env and Path(env).is_file():
        return Path(env)
    for c in _EXE_CANDIDATES:
        if c.is_file():
            return c
    print("ERROR: mz800emu binary not found", file=sys.stderr)
    sys.exit(1)


def _mzf(name, ftype, addr, body, fexec=None):
    """Sestaví MZF (hlavička 128 B + tělo)."""
    hdr = bytearray(128)
    hdr[0] = ftype
    hdr[1:18] = b"\r" * 17
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = len(body).to_bytes(2, "little")
    hdr[0x14:0x16] = addr.to_bytes(2, "little")
    hdr[0x16:0x18] = (addr if fexec is None else fexec).to_bytes(2, "little")
    return bytes(hdr) + bytes(body)


def _loader_body():
    """Kód LOADERu na 1200h: RHEAD + RDATA, výsledek na 1300h, pak smyčka.

    1200 CD 27 00   CALL 0027h      ; RHEAD - hlavička dalšího dílu do 10F0h
    1203 38 09      JR C,120Eh
    1205 CD 2A 00   CALL 002Ah      ; RDATA - tělo podle hlavičky
    1208 38 04      JR C,120Eh
    120A 3E 55      LD A,55h
    120C 18 02      JR 1210h
    120E 3E EE      LD A,0EEh
    1210 32 00 13   LD (1300h),A
    1213 18 FE      JR $
    """
    return bytes([0xCD, 0x27, 0x00, 0x38, 0x09, 0xCD, 0x2A, 0x00, 0x38, 0x04,
                  0x3E, 0x55, 0x18, 0x02, 0x3E, 0xEE, 0x32, 0x00, 0x13, 0x18, 0xFE])


def _data_body():
    """Tělo dílu DATA (pseudonáhodné, aby chyba čtení byla vidět)."""
    return bytes(((i * 73) + 29) & 0xFF for i in range(_DATA_SIZE))


def _cmtspeed_header(us):
    """Hlavička CMTSPEED jako soubory 1x/2x/3xspeed.mzf UniCMT (báze hw/25-unicmt.md)."""
    hdr = bytearray(128)
    hdr[1:10] = b"CMTSPEED\r"
    hdr[0x20] = 0x01
    for i, v in enumerate(us):
        hdr[0x30 + 2 * i:0x32 + 2 * i] = int(v).to_bytes(2, "little")
    return bytes(hdr)


def _make_mzt(path, marker_us):
    """MZT = LOADER + hlavička CMTSPEED + DATA."""
    loader = _mzf(b"UNICMT LOADER", 0x01, 0x1200, _loader_body())
    data = _mzf(b"UNICMT DATA", 0x01, _DATA_ADDR, _data_body())
    path.write_bytes(loader + _cmtspeed_header(marker_us) + data)


class PipeEmu:
    """Emulátor v pipe módu: JSONL request/response přes stdin/stdout."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, cfg_dir, ini, save_ini=False):
        args = [str(exe), "--mcp-pipe", "--headless",
                "--no-first-run-windows", f"--cfg-dir={cfg_dir}",
                f"--work-dir={cfg_dir}", f"--config={ini}"]
        if not save_ini:
            args.insert(3, "--no-save-ini")
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
            if not line.startswith("{"):
                continue
            msg = json.loads(line)
            if pred(msg):
                return msg
        return None

    def request(self, cmd, data=None, timeout=60.0):
        """Pošle request a vrátí celou response (i neúspěšnou)."""
        self.rid += 1
        rid = self.rid
        req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        if resp is None:
            rc = self.proc.poll()
            raise TestFailure(f"no response to {cmd} (emulator exit code: {rc})")
        return resp

    def call(self, cmd, data=None, timeout=60.0):
        """Pošle request a vrátí data úspěšné response (jinak TestFailure)."""
        resp = self.request(cmd, data, timeout)
        if not resp.get("success"):
            raise TestFailure(f"{cmd} failed: {resp.get('error')}")
        return resp.get("data") or {}

    def ram(self, addr, length):
        """Přečte bajty z regionu User RAM."""
        d = self.call("region_read", {"region_id": 1, "offset": addr, "length": length})
        return base64.b64decode(d["data_b64"])

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


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def _pulses_close(got, want):
    """Shoda délek pulzů v µs (tolerance 0,001 µs)."""
    return (isinstance(got, list) and len(got) == 4
            and all(abs(g - w) < 0.001 for g, w in zip(got, want)))


def _boot_and_open(emu, mzt):
    """Nabootuje do menu, vloží pásku (bez přehrávání) a vrátí seznam bloků."""
    emu.call("run", {"frames": 350})
    emu.call("cmt_open", {"path": str(mzt), "play_immediately": False})
    return emu.call("cmt_tape_list").get("blocks") or []


def _rom_load(emu):
    """Volba C v boot menu + PLAY; vrátí bajt výsledku na 1300h (0 = LOADER nedoběhl)."""
    emu.call("region_write", {"region_id": 1, "offset": _FLAG_ADDR,
                              "data_b64": base64.b64encode(b"\x00").decode("ascii")})
    emu.call("input_send_key", {"key": "C", "frames": 5})
    emu.call("cmt_transport", {"action": "play"})
    done = 0
    while done < _LOAD_FRAMES:
        emu.call("run", {"frames": 100})
        done += 100
        flag = emu.ram(_FLAG_ADDR, 1)[0]
        if flag in (0x55, 0xEE):
            return flag
    return emu.ram(_FLAG_ADDR, 1)[0]


def phase_a(new_emu, tmp):
    """Hlavička 1x: zahozená ze seznamu, DATA s pulzy z hlavičky, ROM nahraje."""
    print("Phase A: CMTSPEED 1x header, ROM loads the part after it")
    mzt = tmp / "unicmt_1x.mzt"
    _make_mzt(mzt, _UNICMT_1X)
    emu = new_emu()
    try:
        blocks = _boot_and_open(emu, mzt)
        check([b.get("name") for b in blocks] == ["UNICMT LOADER", "UNICMT DATA"],
              f"CMTSPEED header is not listed as a block: {[b.get('name') for b in blocks]}")
        check(blocks[0].get("block_speed") == "default",
              f"block before the header has the default speed: {blocks[0]}")
        check(blocks[1].get("block_speed") == "set" and blocks[1].get("cmt_speed") == 10,
              f"block after the header has its own custom speed: {blocks[1]}")
        check(_pulses_close(blocks[1].get("pulses_us"), _UNICMT_1X),
              f"block after the header plays the header pulses: {blocks[1].get('pulses_us')}")
        flag = _rom_load(emu)
        check(flag == 0x55, f"ROM loaded the DATA part (result byte {flag:02X}h)")
        check(emu.ram(_DATA_ADDR, _DATA_SIZE) == _data_body(), "DATA body in RAM matches")
    finally:
        emu.close()


def phase_b(new_emu, tmp):
    """Hlavička 3x: ROM s pevným časováním 1:1 DATA nenahraje."""
    print("Phase B: CMTSPEED 3x header, the 1:1 ROM loader cannot read the part")
    mzt = tmp / "unicmt_3x.mzt"
    _make_mzt(mzt, _UNICMT_3X)
    emu = new_emu()
    try:
        blocks = _boot_and_open(emu, mzt)
        check(len(blocks) == 2 and _pulses_close(blocks[1].get("pulses_us"), _UNICMT_3X),
              f"block after the 3x header plays 3x pulses: {blocks[-1].get('pulses_us') if blocks else None}")
        flag = _rom_load(emu)
        check(flag != 0x55, f"ROM did not load the 3x part (result byte {flag:02X}h)")
        check(emu.ram(_DATA_ADDR, _DATA_SIZE) != _data_body(), "DATA body is not in RAM")
    finally:
        emu.close()


def phase_c(new_emu, tmp):
    """Páska 3x, ale DATA přes MCP na pulzy 1x: ROM nahraje."""
    print("Phase C: per-block custom pulses via cmt_tape_block_speed")
    mzt = tmp / "unicmt_3x.mzt"
    emu = new_emu()
    try:
        _boot_and_open(emu, mzt)
        res = emu.call("cmt_tape_block_speed", {"block_id": 1, "pulses_us": _UNICMT_1X})
        check(res.get("speed") == 10 and _pulses_close(res.get("pulses_us"), _UNICMT_1X),
              f"cmt_tape_block_speed accepts pulses_us: {res}")
        blocks = emu.call("cmt_tape_list").get("blocks") or []
        check(_pulses_close(blocks[1].get("pulses_us"), _UNICMT_1X),
              f"tape list reports the new block pulses: {blocks[1].get('pulses_us')}")
        bad = emu.request("cmt_tape_block_speed", {"block_id": 1, "pulses_us": [0, 494, 240, 278]})
        check(not bad.get("success"), "zero pulse length is rejected")
        flag = _rom_load(emu)
        check(flag == 0x55, f"ROM loaded the DATA part (result byte {flag:02X}h)")
        check(emu.ram(_DATA_ADDR, _DATA_SIZE) == _data_body(), "DATA body in RAM matches")
    finally:
        emu.close()


def phase_d(new_emu):
    """Výchozí vlastní pulzy přes cmt_set_property custom_pulses."""
    print("Phase D: default custom pulses via cmt_set_property")
    emu = new_emu()
    try:
        st = emu.call("get_periph_cmt")
        check(st.get("cmtspeed") == 1, f"default speed is 1:1 at start: {st.get('cmtspeed')}")
        res = emu.call("cmt_set_property", {"property": "custom_pulses", "pulses_us": [176, 185, 90, 104]})
        check(_pulses_close(res.get("pulses_us"), [176, 185, 90, 104]), f"custom_pulses echoed: {res}")
        st = emu.call("get_periph_cmt")
        check(st.get("cmtspeed") == 10, f"default speed is custom (10): {st.get('cmtspeed')}")
        check(_pulses_close(st.get("default_pulses_us"), [176, 185, 90, 104])
              and _pulses_close(st.get("custom_pulses_us"), [176, 185, 90, 104]),
              f"periph reports the custom pulses: {st.get('default_pulses_us')} {st.get('custom_pulses_us')}")
        bad = emu.request("cmt_set_property", {"property": "custom_pulses", "pulses_us": [176, 185, 90, 70000]})
        check(not bad.get("success"), "pulse length above 65535 us is rejected")
        emu.call("cmt_set_property", {"property": "speed", "value": 4})
        st = emu.call("get_periph_cmt")
        check(st.get("cmtspeed") == 4 and _pulses_close(st.get("custom_pulses_us"), [176, 185, 90, 104]),
              "back to 3:1, the custom pulses stay stored")
        emu.call("cmt_set_property", {"property": "speed", "value": 10})
        st = emu.call("get_periph_cmt")
        check(st.get("cmtspeed") == 10 and _pulses_close(st.get("default_pulses_us"), [176, 185, 90, 104]),
              "speed 10 switches back to the stored custom pulses")
    finally:
        emu.close()


def phase_e(exe, tmp):
    """Výchozí vlastní pulzy přežijí restart přes INI."""
    print("Phase E: default custom pulses are saved to and loaded from the INI")
    ini = tmp / "save.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                   "[CMTHACK]\nenable = 0\n", encoding="utf-8")
    for i, step in enumerate(("save", "load")):
        cfg = tmp / f"cfg_ini{i}"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini, save_ini=True)
        try:
            if step == "save":
                emu.call("cmt_set_property", {"property": "custom_pulses",
                                              "pulses_us": [235.5, 247, 120, 139]})
            else:
                st = emu.call("get_periph_cmt")
                check(st.get("cmtspeed") == 10
                      and _pulses_close(st.get("default_pulses_us"), [235.5, 247, 120, 139]),
                      f"custom pulses restored from the INI: {st.get('cmtspeed')} "
                      f"{st.get('default_pulses_us')}")
        finally:
            emu.close()
        if step == "save":
            text = ini.read_text(encoding="utf-8")
            check("SPEED_CUSTOM" in text and "mz_custom_pulse_long_high_us" in text,
                  "INI contains SPEED_CUSTOM and the pulse keys")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=280)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_cmt_unicmt_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                       "[CMTHACK]\nenable = 0\n"
                       "[CMT]\nmz_cmtspeed = SPEED_1/1\n", encoding="utf-8")
        counter = [0]

        def new_emu():
            counter[0] += 1
            cfg = tmp / f"cfg{counter[0]}"
            cfg.mkdir()
            return PipeEmu(exe, cfg, ini)

        phase_a(new_emu, tmp)
        phase_b(new_emu, tmp)
        phase_c(new_emu, tmp)
        phase_d(new_emu)
        phase_e(exe, tmp)
        ok = True
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
