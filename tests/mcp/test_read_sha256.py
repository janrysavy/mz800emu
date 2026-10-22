#!/usr/bin/env python3
"""E2E test: SHA-256 u čtení paměti, regionů a obrazu (MCP pipe).

Ověřuje:

  - ``mem_read`` a ``region_read`` vracejí ``sha256`` přečtených bajtů
    (stejný jako ``hashlib.sha256`` ze stažených dat); s ``want_data:
    false`` jen hash bez ``data_b64``,
  - ``frame_hash`` dvakrát v pauze vrátí stejný ``image_sha256``
    i ``vram_sha256``; ``get_frame_screenshot_raw`` nese stejný
    ``image_sha256`` a ten jde spočítat z RGBA dat a palety
    (indexy po řádcích + paleta R, G, B),
  - po zápisu do VRAM se změní ``vram_sha256`` a po vykreslení snímku
    i ``image_sha256``.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import hashlib
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402


def case_mem_read(emu):
    """mem_read: sha256 odpovídá datům, want_data false vrátí jen hash."""
    print("Case: mem_read sha256")
    emu.call("mem_write", {"addr": 0x3000, "data_hex": "".join(f"{i:02X}" for i in range(64))})
    r = emu.call("mem_read", {"addr": 0x3000, "len": 64})
    data = base64.b64decode(r["data_b64"])
    check(r.get("sha256") == hashlib.sha256(data).hexdigest(),
          f"mem_read sha256 matches hashlib ({r.get('sha256')})")
    r2 = emu.call("mem_read", {"addr": 0x3000, "len": 64, "want_data": False})
    check("data_b64" not in r2 and r2.get("sha256") == r["sha256"],
          f"want_data false: hash only, same hash ({sorted(r2)})")
    emu.call("mem_write", {"addr": 0x3000, "data_hex": "FF"})
    r3 = emu.call("mem_read", {"addr": 0x3000, "len": 64, "want_data": False})
    check(r3.get("sha256") != r["sha256"], "hash changes after a write")


def case_region_read(emu):
    """region_read: sha256 odpovídá datům, want_data false."""
    print("Case: region_read sha256")
    regs = emu.call("regions_list").get("regions") or []
    ram = next((x for x in regs if x.get("kind") == "ram"), None)
    check(ram is not None, "RAM region found")
    r = emu.call("region_read", {"region_id": ram["id"], "offset": 0x3000, "length": 256})
    data = base64.b64decode(r["data_b64"])
    check(r.get("sha256") == hashlib.sha256(data).hexdigest(), "region_read sha256 matches hashlib")
    r2 = emu.call("region_read", {"region_id": ram["id"], "offset": 0x3000, "length": 256,
                                  "want_data": False})
    check("data_b64" not in r2 and r2.get("sha256") == r["sha256"], "region_read want_data false")


def _image_hash_from_rgba(raw, palette):
    """Spočítá image_sha256 z RGBA screenshotu a palety (index8 & 0Fh + paleta RGB)."""
    rgb_to_idx = {}
    for i, c in enumerate(palette):
        rgb_to_idx.setdefault(((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF), i)
    px = base64.b64decode(raw["data_b64"])
    idx = bytearray(len(px) // 4)
    for i in range(len(idx)):
        idx[i] = rgb_to_idx[(px[4 * i], px[4 * i + 1], px[4 * i + 2])]
    pal = bytearray()
    for c in palette:
        pal += bytes(((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF))
    return hashlib.sha256(bytes(idx) + bytes(pal)).hexdigest(), len(rgb_to_idx)


def case_frame_hash(emu):
    """frame_hash: stabilní v pauze, shoda se screenshotem, změna po zápisu do VRAM."""
    print("Case: frame_hash and screenshot image_sha256")
    emu.call("run", {"frames": 3})
    h1 = emu.call("frame_hash")
    h2 = emu.call("frame_hash")
    check(h1.get("available") is True and len(h1.get("image_sha256", "")) == 64,
          f"frame_hash available ({h1})")
    check(h1["image_sha256"] == h2["image_sha256"] and h1["vram_sha256"] == h2["vram_sha256"],
          "same hashes twice in a pause")
    raw = emu.call("get_frame_screenshot_raw")
    check(raw.get("image_sha256") == h1["image_sha256"],
          "get_frame_screenshot_raw carries the same image_sha256")
    pal = emu.call("get_frame_framebuffer_info").get("palette")
    calc, distinct = _image_hash_from_rgba(raw, pal)
    if distinct == 16:
        check(calc == h1["image_sha256"], "image_sha256 recomputed from RGBA + palette")
    else:
        print(f"  NOTE: palette has {distinct} distinct colours, recompute skipped")
    raw2 = emu.call("get_frame_screenshot_raw", {"want_data": False})
    check("data_b64" not in raw2 and raw2.get("image_sha256") == h1["image_sha256"],
          "screenshot with want_data false: hash only")
    png = emu.call("get_frame_screenshot", {"want_data": False})
    check("data_b64" not in png and png.get("image_sha256") == h1["image_sha256"],
          "PNG screenshot carries the same image_sha256")

    regs = emu.call("regions_list").get("regions") or []
    planes = [x for x in regs if x.get("kind", "").startswith("vram")]
    check(planes, f"VRAM regions found ({[p.get('kind') for p in planes]})")
    for p in planes:
        emu.call("region_write", {"region_id": p["id"], "offset": 0,
                                  "data_b64": base64.b64encode(b"\xff" * 2000).decode()})
    h3 = emu.call("frame_hash")
    check(h3["vram_sha256"] != h1["vram_sha256"], "vram_sha256 changes after a VRAM write")
    emu.call("run", {"frames": 2})
    h4 = emu.call("frame_hash")
    check(h4["image_sha256"] != h1["image_sha256"], "image_sha256 changes after the frame is drawn")


def run(emu):
    """Spustí všechny případy nad jedním procesem emulátoru."""
    emu.call("pause")
    case_mem_read(emu)
    case_region_read(emu)
    case_frame_hash(emu)


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=100)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_read_sha256_"))
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
