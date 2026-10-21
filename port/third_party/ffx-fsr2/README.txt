AMD FidelityFX Super Resolution 2.2.1 - the API and its shaders, unchanged.

Source: https://github.com/GPUOpen-Effects/FidelityFX-FSR2, tag v2.2.1
        (FidelityFX-FSR2-2.2.1.zip, sha256
        b1f7f5c138a230caa5d40609fa739b239800794604b62a1a667c1e45f2f1e9ad).
Kept:   src/ffx-fsr2-api (without its dx12 / vk backends) and LICENSE.txt (MIT).
Not used: AMD's backends and prebuilt tools (FidelityFX_SC, dxcompiler) - the
native renderer has its own FSR 2 backend on plume and builds the passes with
the port's shader compiler (tools/convert_shaders.py).
