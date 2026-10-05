# Built-in GPU drivers (Android)

`tools/build_apk.py` puts these driver packages into the APK (`assets/drivers/`). The
launcher (`Drivers.java`) offers them on the GPUs they are for: in Settings, under
Graphics driver, and once by itself the first time the launcher opens on such a phone.

| Package | For | Driver |
|---|---|---|
| `Turnip-710-720-722-v4.1.zip` (recommended) | Adreno 710, 720, 722 | Mesa Turnip, Mesa 26.3.0-devel (git-e5f0687867), Vulkan 1.4 |
| `Turnip-710-720-722-v4.0.zip` | Adreno 710, 720, 722 | Mesa Turnip, Mesa 26.3.0, Vulkan 1.4 |

Players reported that both run the game well on these GPUs. The phones' own Qualcomm
drivers for them are often Vulkan 1.1. That version lacks what the native renderer needs
(descriptor indexing, buffer device addresses, 64-bit shader integers), so without
Turnip the game uses the slower emulated renderer.

The packages are builds by **vauzi**, shipped as published: `meta.json` plus
`libvulkan_freedreno.so`. The game loads the driver through libadrenotools
(`src/gpu_driver.cpp`). If a driver fails to start, the game falls back to the
phone's own driver, and the launcher offers to switch back.

## Licence

Turnip is part of Mesa (<https://mesa3d.org>, source at
<https://gitlab.freedesktop.org/mesa/mesa>). Turnip (`src/freedreno/vulkan`) and the
Mesa code it is built from are under the MIT licence, which allows redistributing
binaries as long as this notice comes with them. See
<https://docs.mesa3d.org/license.html>. The APK carries this notice as
`assets/drivers/LICENSE-Mesa.txt`.

```
Copyright © The Mesa authors and contributors

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

To add another built-in driver, put its zip here and add a line to `kBuiltIn` in
`Drivers.java`. That line gives the asset's file name, the name shown to players, and
the Adreno models the driver is for.
