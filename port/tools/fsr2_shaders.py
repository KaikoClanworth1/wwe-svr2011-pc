"""The native renderer's FSR 2 passes (AMD FidelityFX Super Resolution 2.2.1, port/third_party/ffx-fsr2): each
pass compiled once - the one permutation the renderer's FSR 2 context asks for - to DXIL (Direct3D 12) and SPIR-V
(Vulkan), and the passes' resource bindings (name -> register, as AMD's own build reflects them) written to
src/native/fsr2_bindings.inc for the renderer's FSR 2 backend (native/fsr2.cpp).

    py fsr2_shaders.py <out dir>      (convert_shaders.py runs it too: <out>/dxil, <out>/spirv)

The permutation: SDR colour, render-size motion vectors (not jittered), depth 0 near, the reference Lanczos, full
precision, wave32 - FFX_FSR2_OPTION_* below; accumulate twice (sharpening off / on). Vulkan: one descriptor set,
the register kinds apart - t at 0, u at 32, b at 48, s at 56 (fsr2.cpp's layout).
"""
import os
import re
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
from convert_shaders import DXC  # noqa: E402

SHADERS = os.path.join(PORT, "third_party", "ffx-fsr2", "shaders")
BINDINGS = os.path.join(PORT, "src", "native", "fsr2_bindings.inc")

# FfxFsr2Pass order (ffx_fsr2_interface.h): name, file, extra defines.
PASSES = [
    ("depth_clip", "ffx_fsr2_depth_clip_pass.hlsl", {}),
    ("reconstruct_previous_depth", "ffx_fsr2_reconstruct_previous_depth_pass.hlsl", {}),
    ("lock", "ffx_fsr2_lock_pass.hlsl", {}),
    ("accumulate", "ffx_fsr2_accumulate_pass.hlsl", {}),
    ("accumulate_sharpen", "ffx_fsr2_accumulate_pass.hlsl", {"FFX_FSR2_OPTION_APPLY_SHARPENING": 1}),
    ("rcas", "ffx_fsr2_rcas_pass.hlsl", {}),
    ("compute_luminance_pyramid", "ffx_fsr2_compute_luminance_pyramid_pass.hlsl", {}),
    ("autogen_reactive", "ffx_fsr2_autogen_reactive_pass.hlsl", {}),
    ("tcr_autogen", "ffx_fsr2_tcr_autogen_pass.hlsl", {}),
]

DEFINES = {
    "FFX_GPU": 1, "FFX_HLSL": 1, "FFX_HLSL_6_2": 1, "FFX_HALF": 0,
    "FFX_FSR2_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF": 0,
    "FFX_FSR2_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF": 0,
    "FFX_FSR2_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF": 1,
    "FFX_FSR2_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF": 0,
    "FFX_FSR2_OPTION_UPSAMPLE_USE_LANCZOS_TYPE": 2,
    "FFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE": 0,
    "FFX_FSR2_OPTION_HDR_COLOR_INPUT": 0,
    "FFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS": 1,
    "FFX_FSR2_OPTION_JITTERED_MOTION_VECTORS": 0,
    "FFX_FSR2_OPTION_INVERTED_DEPTH": 0,
    "FFX_FSR2_OPTION_APPLY_SHARPENING": 0,
}
VK_SHIFT = {"t": 0, "u": 32, "b": 48, "s": 56}

DECL_RE = re.compile(r"\b(\w+)\s*:\s*register\(\s*([tub])(\d+)\s*\)")


def args(extra):
    d = dict(DEFINES, **extra)
    return ["-nologo", "-HV", "2021", "-E", "CS", "-Wno-for-redefinition", "-Wno-ambig-lit-shift",
            "-I", SHADERS] + [f"-D{k}={v}" for k, v in d.items()]


def run(cmd):
    r = subprocess.run([DXC] + cmd, capture_output=True, text=True, timeout=300)
    if r.returncode:
        return next((l for l in (r.stderr + r.stdout).splitlines() if "error" in l), r.stderr[:300]).strip()
    return None


def bindings(src, extra, tmp):
    """The pass's resources: [(kind t/u/b, register, name)], from the preprocessed source."""
    err = run(args(extra) + ["-P", tmp, src])
    if err:
        return None, err
    with open(tmp, encoding="utf-8", errors="ignore") as f:
        text = f.read()
    os.remove(tmp)
    out = []
    for name, kind, reg in DECL_RE.findall(text):
        if name.startswith("s_"):
            continue
        out.append((kind, int(reg), name))
    return out, None


def build(out):
    """Compiles every pass into out/dxil and out/spirv (fsr2_<pass>.cs.dxil / .cs.spv) and writes the bindings
    table; returns [(pass, what, error)]."""
    for d in ("dxil", "spirv"):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    errors, table = [], []
    for name, file, extra in PASSES:
        src = os.path.join(SHADERS, file)
        base = "fsr2_" + name + ".cs"
        err = run(args(extra) + ["-T", "cs_6_2", "-Fo", os.path.join(out, "dxil", base + ".dxil"), src])
        if err:
            errors.append((name, "dxil", err))
        shifts = []
        for kind, at in VK_SHIFT.items():
            shifts += [f"-fvk-{kind}-shift", str(at), "0"]
        err = run(args(extra) + ["-T", "cs_6_2", "-spirv", "-fspv-target-env=vulkan1.1", "-fvk-use-dx-layout",
                                 *shifts, "-Fo", os.path.join(out, "spirv", base + ".spv"), src])
        if err:
            errors.append((name, "spirv", err))
        b, err = bindings(src, extra, os.path.join(out, base + ".i"))
        if err:
            errors.append((name, "bindings", err))
            b = []
        table.append((name, b))
    # The backend's clear job (src/native/fsr2_clear.hlsl), on the same layout.
    clear = os.path.join(PORT, "src", "native", "fsr2_clear.hlsl")
    base = "fsr2_clear.cs"
    err = run(["-nologo", "-HV", "2021", "-E", "CS", "-T", "cs_6_2", "-Fo", os.path.join(out, "dxil", base + ".dxil"),
               clear])
    if err:
        errors.append(("clear", "dxil", err))
    shifts = []
    for kind, at in VK_SHIFT.items():
        shifts += [f"-fvk-{kind}-shift", str(at), "0"]
    err = run(["-nologo", "-HV", "2021", "-E", "CS", "-T", "cs_6_2", "-spirv", "-fspv-target-env=vulkan1.1",
               "-fvk-use-dx-layout", *shifts, "-Fo", os.path.join(out, "spirv", base + ".spv"), clear])
    if err:
        errors.append(("clear", "spirv", err))
    # (Vulkan with compact tables reads .spvc: the same code - one set of its own either way)
    for name in [p[0] for p in PASSES] + ["clear"]:
        spv = os.path.join(out, "spirv", "fsr2_" + name + ".cs.spv")
        if os.path.exists(spv):
            with open(spv, "rb") as f, open(spv[:-4] + ".spvc", "wb") as g:
                g.write(f.read())
    lines = ["// Generated by tools/fsr2_shaders.py - the FSR 2 passes' resources (FfxFsr2Pass order):",
             "// {kind 't' / 'u' / 'b', register, name} - the names ffx_fsr2.cpp matches its resources by.",
             ""]
    for name, b in table:
        lines.append(f"static const Fsr2Binding kFsr2Bindings_{name}[] = {{")
        for kind, reg, var in b:
            lines.append(f"    {{'{kind}', {reg}, L\"{var}\"}},")
        if not b:
            lines.append("    {0, 0, L\"\"},")
        lines.append("};")
    lines.append("")
    lines.append("static const Fsr2Pass kFsr2Passes[] = {")
    for name, b in table:
        lines.append(f"    {{\"fsr2_{name}.cs\", kFsr2Bindings_{name}, {len(b)}}},")
    lines.append("};")
    with open(BINDINGS, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return errors


if __name__ == "__main__":
    errs = build(sys.argv[1])
    for e in errs:
        print(*e, sep=": ")
    print(f"{len(PASSES)} FSR 2 passes, {len(errs)} errors -> {sys.argv[1]}")
