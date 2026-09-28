"""Convert captured shader containers to HLSL (XenosRecomp) and DXIL (DXC).

    python convert_shaders.py [xsc_dir] [out_dir]

Defaults: runs/d3dtrace/xsc -> runs/shaders_native/{hlsl,dxil}. Output per
shader <hash>.<vs|ps> (hash = FNV-1a of the container, as the native renderer
computes it when the game creates the shader):
  dxil/<hash>.<vs|ps>.dxil        specialization constants = 0
  dxil/<hash>.vs.s1.dxil          packed 11:11:10 normals (if the shader has them)
  dxil/<hash>.ps.s2.dxil          alpha test (if the shader has it)
  dxil/<hash>.vs.inputs           the vertex shader's inputs: SEMANTIC INDEX TYPE
  dxil/<hash>.<vs|ps>.textures    the texture fetch slots it samples: SLOT DIMENSION
                                  (0 2D, 1 3D, 2 cube)
The specialization constants are baked in with -DSVR_SPEC_CONSTANTS (see the
XenosRecomp patch), so the renderer never links shaders at runtime.
Writes out_dir/report.txt with every failure and its first error.
"""
import concurrent.futures
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
XR = os.path.join(ROOT, "recomp", "XenosRecomp", "build", "XenosRecomp", "XenosRecomp.exe")
HEADER = os.path.join(ROOT, "recomp", "XenosRecomp", "XenosRecomp", "shader_common.h")
DXC = os.path.join(ROOT, "recomp", "XenosRecomp", "thirdparty", "dxc-bin", "bin", "x64", "dxc.exe")

INPUT_RE = re.compile(r"in (float4|uint4) i\w+ : ([A-Z]+)(\d+)")
TEXTURE_DEFINE_RE = re.compile(r"#define (\w+)_Texture(2D|3D|Cube)DescriptorIndex g_ResourceIndex\(\d+, (\d+)\)")
TEXTURE_USE_RE = re.compile(r"(\w+)_Texture(2D|3D|Cube)DescriptorIndex")
DIMENSIONS = {"2D": 0, "3D": 1, "Cube": 2}


def compile_dxil(hlsl, dxil, stage, mask):
    r = subprocess.run([DXC, "-nologo", "-HV", "2021", "-T", f"{stage}_6_0", "-E", "main",
                        f"-DSVR_SPEC_CONSTANTS={mask}", "-Fo", dxil, hlsl],
                       capture_output=True, text=True, timeout=120)
    if r.returncode:
        return next((l for l in (r.stderr + r.stdout).splitlines() if "error" in l),
                    r.stderr[:300]).strip()
    return None


def convert(xsc, out):
    name = os.path.basename(xsc)[:-4]          # <hash>.<vs|ps>
    stage = "ps" if name.endswith(".ps") else "vs"
    hlsl = os.path.join(out, "hlsl", name + ".hlsl")
    r = subprocess.run([XR, xsc, hlsl, HEADER], capture_output=True, text=True, timeout=60)
    if r.returncode or not os.path.exists(hlsl) or not os.path.getsize(hlsl):
        return name, "hlsl", (r.stderr or r.stdout).strip()[:300]
    src = open(hlsl, encoding="utf-8", errors="ignore").read()
    body = src[src.find("void main("):]
    variants = [0]
    if stage == "vs" and "tfetchR11G11B10(" in body:
        variants.append(1)
    if stage == "ps" and "SPEC_CONSTANT_ALPHA_TEST" in body:
        variants.append(2)
    for mask in variants:
        suffix = "" if mask == 0 else f".s{mask}"
        err = compile_dxil(hlsl, os.path.join(out, "dxil", f"{name}{suffix}.dxil"), stage, mask)
        if err:
            return name, f"dxil s{mask}", err
    slots = {(name, dim): int(slot) for name, dim, slot in TEXTURE_DEFINE_RE.findall(src)}
    used = sorted({(slots[(name, dim)], DIMENSIONS[dim]) for name, dim in TEXTURE_USE_RE.findall(body)
                   if (name, dim) in slots})
    with open(os.path.join(out, "dxil", name + ".textures"), "w") as f:
        for slot, dim in used:
            f.write(f"{slot} {dim}\n")
    if stage == "vs":
        header = body[:body.find("{")]
        with open(os.path.join(out, "dxil", name + ".inputs"), "w") as f:
            for typ, semantic, index in INPUT_RE.findall(header):
                f.write(f"{semantic} {index} {typ}\n")
    return name, None, ""


def main():
    xsc_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "port", "runs", "d3dtrace", "xsc")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "port", "runs", "shaders_native")
    for d in ("hlsl", "dxil"):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    files = sorted(glob.glob(os.path.join(xsc_dir, "*.xsc")))
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(lambda f: convert(f, out), files))
    # The renderer's own shaders (src/native/shaders/<name>.<vs|ps>.hlsl).
    own = os.path.join(ROOT, "port", "src", "native", "shaders")
    for hlsl in sorted(glob.glob(os.path.join(own, "*.hlsl"))):
        name = os.path.basename(hlsl)[:-5]
        err = compile_dxil(hlsl, os.path.join(out, "dxil", name + ".dxil"), name.split(".")[-1], 0)
        if err:
            results.append((name, "own", err))
    failed = [r for r in results if r[1]]
    with open(os.path.join(out, "report.txt"), "w") as rep:
        rep.write(f"{len(files)} shaders, {len(files) - len(failed)} converted to DXIL\n")
        for name, stage, err in failed:
            rep.write(f"{name}: {stage}: {err}\n")
    vs = sum(1 for f in files if f.endswith(".vs.xsc"))
    variants = len(glob.glob(os.path.join(out, "dxil", "*.s?.dxil")))
    print(f"{len(files)} shaders ({vs} vertex, {len(files) - vs} pixel): "
          f"{len(files) - len(failed)} compiled to DXIL (+{variants} specialized variants), "
          f"{len(failed)} failed -> {out}")


if __name__ == "__main__":
    main()
