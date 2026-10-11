# Software rendering by default

The UI defaults to SDL's software renderer on every platform, including Windows.
Hardware Direct3D11 caused a reported host freeze after initially successful
manual use. A short successful preview does not establish driver stability.

`--renderer=direct3d11` and `--renderer=warp` remain explicit Windows options.
There is no automatic fallback to a GPU renderer. OpenGL also requires explicit
selection. The software default overrides inherited SDL renderer/WARP hints;
no INI renderer selection currently supersedes this CLI/default policy.

The regression qualification uses only software rendering. Hardware D3D11,
WARP and OpenGL are not initialized in that qualification. This restores the
previous rendering choice; it does not diagnose the GPU freeze.
