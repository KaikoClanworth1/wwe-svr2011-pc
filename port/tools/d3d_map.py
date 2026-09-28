"""Build the D3D library map (docs/d3d_map.csv) from the census and call graph.

    python d3d_map.py

Inputs: runs/callgraph.json (tools/callgraph.py), runs/d3dtrace/calls_{title,
menu,ring}.csv (tools/d3d_census.ps1). For every function of the game's D3D
library: identified name and evidence (NAMES below), calls per frame in each
phase, argument ranges, PM4 packets it builds, imports, and its caller.
"""
import csv
import json
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RUNS = os.path.join(ROOT, "runs")
DEVICE = "AD977E80"   # the D3D device object (r3 of device methods)

# address -> (name, confidence, evidence)
NAMES = {
    "8291AED0": ("D3DDevice_Present (Swap)", "certain",
                 "only caller of VdSwap / VdGetSystemCommandBuffer; once per frame"),
    "82921548": ("D3DDevice_CreateVertexShader", "certain",
                 "receives every vertex shader container (flags bit0=1) in r3"),
    "82921360": ("D3DDevice_CreatePixelShader", "certain",
                 "receives every pixel shader container (flags bit0=0) in r3"),
    "82921B58": ("D3DDevice_DrawIndexedVertices", "high",
                 "emits PM4 DRAW_INDX; r4=6 (D3DPT_TRIANGLESTRIP), r5=base 0, r6=start 0, "
                 "r7=index count 5..12413; ~629/frame in ring"),
    "82921698": ("D3DDevice_DrawVerticesUP (quads)", "medium",
                 "emits PM4 DRAW_INDX; r4=13 (D3DPT_QUADLIST), r5=4, r6=stride 12..24, "
                 "r7=vertex data; ~54/frame (2D/UI)"),
    "829208D8": ("D3DDevice_SetVertexShaderConstantF", "high",
                 "r4=start register 0..248, r5=data, r6=vector4 count 1..12; ~5900/frame"),
    "82920800": ("D3DDevice_SetPixelShaderConstantF", "high",
                 "r4=start register 0..223 (pixel shaders have 224), r5=data, r6=count; "
                 "~5300/frame"),
    "829209B0": ("D3DDevice_SetVertexShaderConstantB", "medium",
                 "packs r6 BOOLs from r5 into a device bitfield at bit r4 (0..16)"),
    "82920A10": ("D3DDevice_SetPixelShaderConstantB", "medium",
                 "same as 829209B0 for registers 16..26"),
    "8291DD70": ("D3DDevice_SetStreamSource", "high",
                 "r4=stream 0..4, r5=vertex buffer, r6=offset 0, r7=stride 0..44; once per draw"),
    "8291DE90": ("D3DDevice_SetIndices", "medium",
                 "r4=index buffer or 0, writes its GPU address; once per draw"),
    "82917EC8": ("D3DDevice_SetTexture", "high",
                 "r4=sampler 0..25, r5=texture or NULL; ~1250/frame"),
    "82920F78": ("D3DDevice_SetVertexDeclaration", "medium",
                 "stores r4 (pointer) at device+11992, sets dirty bit 0x80000"),
    "8291B960": ("D3DDevice_SetRenderState_* (3-bit state at +10568)", "medium",
                 "stores r4 (0..6) into bits 0-2 of device+10568, dirty bit 0x40"),
    "82925D78": ("D3D internal: bind shaders at draw (ALU constants)", "medium",
                 "PM4 LOAD_ALU_CONSTANT; r4=shader object (starts with its container); "
                 "~600/frame"),
    "82926000": ("D3D internal: load shader program (IM_LOAD)", "medium",
                 "PM4 IM_LOAD + SET_CONSTANT; ~311/frame"),
    "82919F18": ("D3D internal: GPU wait / ring-space poll", "medium",
                 "~430,000 calls/frame with constant args - a spin loop waiting on the GPU"),
    "8291F168": ("D3DDevice_Clear", "high",
                 "r4=flags 0x0F (colour) / 0x3F (colour+depth+stencil); once per render "
                 "target at the start of each pass (frame log), each followed by a full-surface "
                 "rect draw (8291EC48); helpers 8291FEB0, 8291FD88, 8291F708 (clear shaders)"),
    "82918A88": ("D3DDevice_Resolve", "high",
                 "called at the end of each pass (frame log): r4=flags (0 colour, 4 depth), "
                 "r8=destination texture; EVENT_WRITE + IM_LOAD_IMMEDIATE (copy shaders); "
                 "helper 82919BD8 gets the same texture"),
    "8291EC48": ("D3D internal: rect-list draw (Clear quad)", "high",
                 "DRAW_INDX_2; follows every Clear, sized to the render target"),
    "8291EF00": ("D3D internal: Clear setup (width, height)", "medium",
                 "between Clear and its rect draw; r6/r7 = render target size"),
    "82919BD8": ("D3D internal: Resolve setup", "medium", "precedes each Resolve with its texture"),
    "82921100": ("D3DDevice_SetShaderGPRAllocation", "high",
                 "r5 + r6 always = 128 (48+80, 32+96, 16+112, 64+64): the Xenos 128 GPRs "
                 "split between vertex and pixel shaders; before each batch of draws"),
    "82920678": ("D3D internal: GPR allocation packet", "medium", "follows every 82921100"),
    "8291B670": ("D3D internal: set surface extent (width, height)", "medium",
                 "r6/r7 = active render target size (1280x720, 512x512, 1120x1120)"),
    "82920B58": ("D3DDevice_SetPixelShader", "high",
                 "r4 = pixel shader object (type 7, embeds its 0x102A1100 container header); "
                 "stored at device+0x3244; ~287/frame"),
    "82920D60": ("D3DDevice_SetVertexShader", "high",
                 "r4 = vertex shader object (type 6); stored at device+0x3248; ~287/frame"),
    "82924E40": ("D3D internal: write register group to command buffer", "high",
                 "r5 = first GPU register of a group, r6 = its mirror in the device "
                 "(e.g. 0x2200 <- device+0x2934); gives the register mirror layout"),
    "82925080": ("D3D internal: write register sub-range", "high",
                 "r5 = GPU register inside a group, r6 = the group's device mirror base"),
    "829251D8": ("D3D internal: write shader constants", "high",
                 "r5 = 0x4000 (vertex) / 0x4400 (pixel) constants, r6 = device+0x780 / +0x1780"),
    "8291E618": ("D3DDevice_SetRenderTarget", "medium",
                 "r4=render target index 0, r5=surface (resource type 4); starts every pass"),
    "8291E588": ("D3D internal: render target setup (surface)", "low",
                 "r4=r5=surface; follows SetRenderTarget"),
    "8291E4E8": ("D3D internal: viewport from surface", "low",
                 "r4=default viewport {0,0,65535,65535,0,1.0}, r5=surface"),
    "8291E280": ("D3D internal: viewport from surface (2)", "low", "same arguments as 8291E4E8"),
    "8291DC28": ("D3D internal: scissor/window from surface", "low",
                 "r4=rect {0,0,65535,65535}, r5=surface, r7=surface height"),
}

# Families: several small functions with the same shape.
for _a in ("8291D200", "8291D3A8", "8291D548", "8291D978", "8291D9C8"):
    NAMES[_a] = ("D3DDevice_SetSamplerState_* (one state)", "medium",
                 "r4=sampler 0..25, r5=small value; ~98/frame, once per sampler change")
for _a in ("8291BA88", "8291BB18", "8291BBA8", "8291B9C0", "8291B9F8", "8291BE20",
           "8291C088", "8291C0C8", "8291BE80", "8291C0F8", "8291C170", "8291C130"):
    NAMES[_a] = ("D3DDevice_SetRenderState_* (one state)", "medium",
                 "leaf of 7-32 instructions: stores r4 (small value) into the device's "
                 "render state block and sets a dirty bit; called from the engine's state "
                 "cache (0x8270E5xx)")

PM4 = {0x22: "DRAW_INDX", 0x36: "DRAW_INDX_2", 0x2F: "LOAD_ALU_CONSTANT", 0x27: "IM_LOAD",
       0x2B: "IM_LOAD_IMMEDIATE", 0x2D: "SET_CONSTANT", 0x46: "EVENT_WRITE",
       0x3C: "WAIT_REG_MEM", 0x3D: "MEM_WRITE", 0x58: "EVENT_WRITE_SHD", 0x54: "INTERRUPT",
       0x10: "NOP", 0x3E: "REG_RMW", 0x44: "COND_WRITE", 0x48: "ME_INIT",
       0x3F: "INDIRECT_BUFFER", 0x2E: "LOAD_CONSTANT_CONTEXT", 0x25: "VIZ_QUERY",
       0x4A: "CONTEXT_UPDATE", 0x59: "EVENT_WRITE_EXT", 0x5A: "EVENT_WRITE_ZPD"}


def load(name):
    path = os.path.join(RUNS, "d3dtrace", name)
    with open(path) as f:
        rows = list(csv.DictReader(l for l in f if not l.startswith("#")))
    return {r["address"]: r for r in rows}


def main():
    graph = json.load(open(os.path.join(RUNS, "callgraph.json")))
    phases = {p: load(f"calls_{p}.csv") for p in ("title", "menu", "ring")}
    ring = phases["ring"]
    out = os.path.join(ROOT, "docs", "d3d_map.csv")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    rows = []
    for addr in sorted(set(ring) | {"8291AED0"}):
        f = graph.get("sub_" + addr, {})
        r = ring.get(addr, {})
        name, conf, why = NAMES.get(addr, ("", "", ""))
        packets = sorted({PM4[(c >> 8) & 0x7F] for c in f.get("consts", [])
                          if (c >> 30) == 3 and ((c >> 8) & 0x7F) in PM4})
        args = " ".join(f"r{k}={r.get(f'r{k}_min','')}..{r.get(f'r{k}_max','')}"
                        for k in range(3, 9)) if r and int(r["calls_total"]) else ""
        rows.append({
            "address": addr, "name": name, "confidence": conf,
            "device_method": "yes" if r.get("r3_min") == r.get("r3_max") == DEVICE else "",
            "title_per_frame": phases["title"].get(addr, {}).get("per_frame", ""),
            "menu_per_frame": phases["menu"].get(addr, {}).get("per_frame", ""),
            "ring_per_frame": r.get("per_frame", ""),
            "calls_total": r.get("calls_total", ""),
            "caller": r.get("caller", ""),
            "instructions": f.get("insns", ""),
            "pm4": " ".join(packets),
            "imports": " ".join(f.get("imports", [])),
            "args": args,
            "evidence": why,
        })
    with open(out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    used = [x for x in rows if x["calls_total"] not in ("", "0")]
    per_frame = [x for x in rows if x["ring_per_frame"] not in ("", "0.00")]
    print(f"{len(rows)} functions, {len(used)} used, {len(per_frame)} per frame in the ring, "
          f"{sum(1 for x in rows if x['name'])} named -> {out}")


if __name__ == "__main__":
    main()
