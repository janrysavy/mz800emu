"""Hidden SDL renderer/readback smoke check. Never creates an OpenGL context.

Usage: python check_sdl_renderers.py --sdl path/to/SDL3.dll --renderer software
Windows WARP: use --renderer warp. Hardware D3D11 is opt-in, never a fallback.
This tests the bundled SDL device, not the emulator UI or host stability.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdl', type=Path, required=True)
    p.add_argument('--renderer', choices=('software', 'warp', 'direct3d11'), required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if a.renderer == 'software': os.environ['SDL_VIDEO_DRIVER'] = 'dummy'
    else:
        os.environ.pop('SDL_VIDEO_DRIVER', None)
        os.environ.pop('SDL_VIDEODRIVER', None)
    dll_dir = os.add_dll_directory(str(a.sdl.resolve().parent)) if os.name == 'nt' else None
    sdl = C.CDLL(str(a.sdl.resolve()))
    def api(name, result, *args):
        f = getattr(sdl, name); f.restype = result; f.argtypes = args
        return f
    ptr = C.c_void_p; b = C.c_bool; i = C.c_int; u8 = C.c_uint8
    init = api('SDL_Init', b, C.c_uint32)
    hint = api('SDL_SetHintWithPriority', b, C.c_char_p, C.c_char_p, i)
    version = api('SDL_GetVersion', i)
    create_window = api('SDL_CreateWindow', ptr, C.c_char_p, i, i, C.c_uint64)
    create_renderer = api('SDL_CreateRenderer', ptr, ptr, C.c_char_p)
    name = api('SDL_GetRendererName', C.c_char_p, ptr)
    error = api('SDL_GetError', C.c_char_p)
    color = api('SDL_SetRenderDrawColor', b, ptr, u8, u8, u8, u8)
    clear = api('SDL_RenderClear', b, ptr)
    pixels = api('SDL_RenderReadPixels', ptr, ptr, ptr)
    pixel = api('SDL_ReadSurfacePixel', b, ptr, i, i, *([C.POINTER(u8)] * 4))
    destroy_surface = api('SDL_DestroySurface', None, ptr)
    destroy_renderer = api('SDL_DestroyRenderer', None, ptr)
    destroy_window = api('SDL_DestroyWindow', None, ptr)
    quit_sdl = api('SDL_Quit', None)
    report = dict(renderer=a.renderer, sdl_version=version(), passed=False,
                  opengl_created=False, hidden_window=True, host_stability_proof=False)
    window = renderer = None
    def check(result):
        if not result: raise RuntimeError(error().decode())
    try:
        if a.renderer == 'warp' and version() < 3004000:
            raise RuntimeError('WARP requires SDL 3.4+')
        check(hint(b'SDL_RENDER_DIRECT3D11_WARP', b'1' if a.renderer == 'warp' else b'0', 2))
        check(init(0x20))
        window = create_window(b'SDL renderer smoke check', 64, 64, 8); check(window)
        requested = b'software' if a.renderer == 'software' else b'direct3d11'
        renderer = create_renderer(window, requested); check(renderer)
        report['actual_renderer'] = name(renderer).decode()
        assert report['actual_renderer'] == requested.decode(), 'Unexpected fallback renderer'
        for rgb in ((255, 0, 0), (0, 255, 0), (0, 0, 255), (17, 83, 149)):
            check(color(renderer, *rgb, 255)); check(clear(renderer))
            surface = pixels(renderer, None); check(surface)
            try:
                values = [u8() for _ in range(4)]
                check(pixel(surface, 23, 31, *(C.byref(v) for v in values)))
                assert tuple(v.value for v in values[:3]) == rgb
            finally: destroy_surface(surface)
        report['readback_checks'] = 4
        report['passed'] = True
    except Exception as e: report['error'] = repr(e)
    finally:
        if renderer: destroy_renderer(renderer)
        if window: destroy_window(window)
        quit_sdl()
        if dll_dir: dll_dir.close()
        a.output.parent.mkdir(parents=True, exist_ok=True)
        a.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report))
    return 0 if report['passed'] else 1


if __name__ == '__main__': raise SystemExit(main())
