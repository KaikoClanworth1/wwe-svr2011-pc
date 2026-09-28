"""Adapt XenosRecomp (hedge-dev, written for Sonic Unleashed) to SvR 2011.

  * COLOR2-COLOR7 interpolators (the game passes up to COLOR4).
  * Vertex texture fetch: SampleLevel(..., 0) in vertex shaders (Sample needs
    derivatives, which only pixel shaders have).
  * 32 texture fetch slots: descriptor indices come from fixed per-slot
    arrays in the shared constants (g_ResourceIndex(dimension, slot)), so
    samplers above 15 and slots without a constant-table name ("s16") work.
  * Conditional exec blocks honour their bool constant / predicate.

    python patch_xenosrecomp.py <XenosRecomp source dir>   (idempotent)
"""
import os
import sys

def patch(path, edits):
    """Apply each (old, new) edit unless its result is already there, so the
    script can be re-run as edits are added. An `old` starting with "." (or
    an edit with a third element True) is replaced everywhere; others once."""
    text = open(path, encoding="utf-8").read()
    applied = 0
    for edit in edits:
        old, new = edit[0], edit[1]
        everywhere = old.startswith(".") or (len(edit) > 2 and edit[2])
        if new and new in text and not (everywhere and old in text):
            continue
        if not new and old not in text:  # a removal, already done
            continue
        if old not in text:
            # Already applied and then changed by a later edit (the later
            # edit's pattern is checked on its own).
            print(f"{path}: skipped (superseded): {old.splitlines()[0][:70]}")
            continue
        text = text.replace(old, new, -1 if everywhere else 1)
        applied += 1
    open(path, "w", encoding="utf-8", newline="\n").write(text)
    print(f"{path}: {applied} edit(s) applied")


RECOMPILER = [
    (r"""    { DeclUsage::Color, 0 },
    { DeclUsage::Color, 1 }
};""", r"""    { DeclUsage::Color, 0 },
    { DeclUsage::Color, 1 },
    // [svr2011] WWE SmackDown vs. Raw 2011 passes up to COLOR4.
    { DeclUsage::Color, 2 },
    { DeclUsage::Color, 3 },
    { DeclUsage::Color, 4 },
    { DeclUsage::Color, 5 },
    { DeclUsage::Color, 6 },
    { DeclUsage::Color, 7 }
};"""),
    # SPIR-V: no per-sampler defines
    (r"""        case RegisterSet::Sampler:
        {
            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
            {
                println("#define {}_Texture{}DescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",
                    constantName, TEXTURE_DIMENSIONS[j], j * 64 + constantInfo->registerIndex * 4);
            }

            println("#define {}_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",
                constantName, std::size(TEXTURE_DIMENSIONS) * 64 + constantInfo->registerIndex * 4);

            samplers.emplace(constantInfo->registerIndex, constantName);
            break;
        }""", r"""        case RegisterSet::Sampler:
        {
            // [svr2011] descriptor indices come from fixed per-slot arrays
            // (g_ResourceIndex in shader_common.h); the names are #defined
            // after the constant buffers.
            samplers.emplace(constantInfo->registerIndex, constantName);
            break;
        }"""),
    # D3D12: no per-sampler cbuffer fields
    (r"""    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Sampler)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
            {
                println("\tuint {}_Texture{}DescriptorIndex : packoffset(c{}.{});",
                    constantName, TEXTURE_DIMENSIONS[j], j * 4 + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]);
            }

            println("\tuint {}_SamplerDescriptorIndex : packoffset(c{}.{});",
                constantName, 4 * std::size(TEXTURE_DIMENSIONS) + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]);
        }
    }

""", ""),
    (r"""    out += "#endif\n";
""", r"""    out += "#endif\n\n";

    // [svr2011] descriptor index names for all 32 fetch slots: the constant
    // table's sampler names and the "s<slot>" names used for unnamed slots.
    for (uint32_t slot = 0; slot < 32; slot++)
    {
        std::string names[2] = { fmt::format("s{}", slot), {} };
        if (auto found = samplers.find(slot); found != samplers.end())
            names[1] = found->second;
        for (const auto& name : names)
        {
            if (name.empty())
                continue;
            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
                println("#define {}_Texture{}DescriptorIndex g_ResourceIndex({}, {})", name, TEXTURE_DIMENSIONS[j], j, slot);
            println("#define {}_SamplerDescriptorIndex g_ResourceIndex({}, {})", name, std::size(TEXTURE_DIMENSIONS), slot);
        }
    }
"""),
]

RECOMPILER += [
    # (an earlier version tested isPixelShader here, before it is set, so pixel
    # shaders were compiled as vertex shaders: SampleLevel 0, no mipmapping)
    (r"""    if (!isPixelShader)
        out += "#define SVR_VERTEX_SHADER 1\n";""", r"""    if ((shaderContainer->flags & 0x1) != 0)
        out += "#define SVR_VERTEX_SHADER 1\n";"""),
    (r"""    out += include;""", r"""    // [svr2011] lets shader_common.h pick SampleLevel for vertex texture fetch
    if ((shaderContainer->flags & 0x1) != 0)
        out += "#define SVR_VERTEX_SHADER 1\n";
    out += include;"""),
]

RECOMPILER += [
    # Conditional exec blocks (cexec on a bool constant or the predicate) were
    # emitted unconditionally, so e.g. the static and skinned transforms both ran.
    (r"""                sequence = cfInstr.condExec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecEnd || cfInstr.opcode == ControlFlowOpcode::CondExecEnd);
                break;
""", r"""                sequence = cfInstr.condExec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecEnd || cfInstr.opcode == ControlFlowOpcode::CondExecPredCleanEnd);
                // SVR2011: execute the block only when the bool constant equals the condition.
                if (count != 0)
                {
                    indent();
                    auto findResult = boolConstants.find(cfInstr.condExec.boolAddress);
                    if (findResult != boolConstants.end())
                        println("if ((g_Booleans & {}) {}= 0)", findResult->second, cfInstr.condExec.condition ? "!" : "=");
                    else
                        println("if ((g_Booleans & (1 << {})) {}= 0)", (uint32_t(cfInstr.condExec.boolAddress) & 15) + (isPixelShader ? 16 : 0), cfInstr.condExec.condition ? "!" : "=");
                    indent();
                    out += "{\n";
                    ++indentation;
                    shouldCloseCurlyBracket = true;
                }
                break;
"""),
    (r"""                sequence = cfInstr.condExecPred.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecPredEnd);
                break;
""", r"""                sequence = cfInstr.condExecPred.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecPredEnd);
                // SVR2011: execute the block only when the predicate equals the condition.
                if (count != 0)
                {
                    indent();
                    println("if ({}p0)", cfInstr.condExecPred.condition ? "" : "!");
                    indent();
                    out += "{\n";
                    ++indentation;
                    shouldCloseCurlyBracket = true;
                }
                break;
"""),
    (r"""                sequence >>= 2;
                instructionCode += 3;
            }

            if (shouldReturn)""", r"""                sequence >>= 2;
                instructionCode += 3;
            }

            if (shouldCloseCurlyBracket)
            {
                --indentation;
                indent();
                out += "}\n";
                shouldCloseCurlyBracket = false;
            }

            if (shouldReturn)"""),
]

COMMON = [
    (r"""#define g_Booleans                 vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256)
#define g_SwappedTexcoords         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 260)
#define g_HalfPixelOffset          vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 264)
#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 272)""",
     r"""// [svr2011] shared constants: 4 x 32 descriptor indices (2D, 3D, cube
// textures, then samplers - one per fetch slot), then the values below.
#define g_ResourceIndex(DIM, SLOT) vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + ((DIM) * 32 + (SLOT)) * 4)
#define g_Booleans                 vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 512)
#define g_SwappedTexcoords         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 516)
#define g_HalfPixelOffset          vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 520)
#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 528)"""),
    (r"""#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x);""",
     r"""// [svr2011] same layout as the SPIR-V path: c0-c31 descriptor indices.
#define DEFINE_SHARED_CONSTANTS() \
    uint4 g_ResourceIndices[32] : packoffset(c0); \
    uint g_Booleans : packoffset(c32.x); \
    uint g_SwappedTexcoords : packoffset(c32.y); \
    float2 g_HalfPixelOffset : packoffset(c32.z); \
    float g_AlphaThreshold : packoffset(c33.x);

#define g_ResourceIndex(DIM, SLOT) g_ResourceIndices[(DIM) * 8 + ((SLOT) >> 2)][(SLOT) & 3]"""),
    (r"""#define FLT_MIN asfloat(0xff7fffff)""", r"""// [svr2011] vertex shaders have no derivatives: sample mip 0 there.
#ifdef SVR_VERTEX_SHADER
#define SVR_SAMPLE(SAMPLER, COORD) SampleLevel(SAMPLER, COORD, 0)
#else
#define SVR_SAMPLE(SAMPLER, COORD) Sample(SAMPLER, COORD)
#endif

#define FLT_MIN asfloat(0xff7fffff)"""),
    (".Sample(", ".SVR_SAMPLE("),
    # Specialization constants baked in at compile time (-DSVR_SPEC_CONSTANTS=N)
    # instead of linking DXIL libraries at runtime.
    (r"""uint g_SpecConstants();""", r"""// [svr2011] variants are compiled with -DSVR_SPEC_CONSTANTS=<mask>
#ifdef SVR_SPEC_CONSTANTS
uint g_SpecConstants() { return SVR_SPEC_CONSTANTS; }
#else
uint g_SpecConstants();
#endif"""),
    # NDC transform (c33.yz): positions of draws with the viewport transform
    # off (PA_CL_VTE_CNTL) are in pixels; the renderer passes a scale and an
    # offset (g_HalfPixelOffset, which also carries the D3D9 half pixel).
    (r"""    float g_AlphaThreshold : packoffset(c33.x);""",
     r"""    float g_AlphaThreshold : packoffset(c33.x); \
    float2 g_NdcScale : packoffset(c33.y);"""),
    (r"""#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 528)""",
     r"""#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 528)
#define g_NdcScale                 vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 532)"""),
]

RECOMPILER += [
    (r"""oPos.xy += g_HalfPixelOffset * oPos.w;""",
     r"""oPos.xy = oPos.xy * g_NdcScale + g_HalfPixelOffset * oPos.w;""", True),
    # Booleans: one 32-bit word per stage (this game's pixel shaders use b22-b26,
    # which don't fit Unleashed's 16 + 16 packing): vertex shaders test
    # g_Booleans, pixel shaders g_PsBooleans (c33.w).
    (r"""println("\t#define {} (1 << {})", constantName, constantInfo->registerIndex + (isPixelShader ? 16 : 0));""",
     r"""println("\t#define {} (1u << {})", constantName, constantInfo->registerIndex & 31);"""),
    (r"""println("if ((g_Booleans & (1 << {})) {}= 0)", (uint32_t(cfInstr.condExec.boolAddress) & 15) + (isPixelShader ? 16 : 0), cfInstr.condExec.condition ? "!" : "=");""",
     r"""println("if ((g_Booleans & (1u << {})) {}= 0)", uint32_t(cfInstr.condExec.boolAddress) & 31, cfInstr.condExec.condition ? "!" : "=");"""),
    (r"""println("if (b{} {}= 0)", uint32_t(cfInstr.condJmp.boolAddress), cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");""",
     r"""println("if ((g_Booleans & (1u << {})) {}= 0)", uint32_t(cfInstr.condJmp.boolAddress) & 31, cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");"""),
    (r"""    out += "\tDEFINE_SHARED_CONSTANTS();\n";
    out += "};\n\n";

    out += "#endif\n\n";""",
     r"""    out += "\tDEFINE_SHARED_CONSTANTS();\n";
    out += "};\n\n";

    out += "#endif\n\n";

    // [svr2011] pixel shaders test their own booleans
    if (isPixelShader)
        out += "#ifndef __spirv__\n#define g_Booleans g_PsBooleans\n#endif\n\n";"""),
]

RECOMPILER += [
    # Pixel shaders use all 256 constants here (the skin shaders read c224-c230);
    # Unleashed limited them to 224.
    (r"""uint32_t tailCount = (isPixelShader ? 224 : 256) - constantInfo->registerIndex;""",
     r"""uint32_t tailCount = 256 - constantInfo->registerIndex;""", True),
]

COMMON += [
    # Packed normals (k_11_11_10, named MSB first): x is the low 10 bits, y the
    # next 11, z the top 11, all signed normalized (checked on vertex data:
    # this decode gives unit vectors). Unleashed's decode had it reversed.
    (r"""        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);""",
     r"""        // [svr2011] x: bits 0-9, y: 10-20, z: 21-31, signed normalized
        int3 v = int3(int(value.x << 22) >> 22, int(value.x << 11) >> 21, int(value.x) >> 21);
        return float4(max(float3(v) / float3(511.0, 1023.0, 1023.0), -1.0), 0.0);"""),
    (r"""    float2 g_NdcScale : packoffset(c33.y);""",
     r"""    float2 g_NdcScale : packoffset(c33.y); \
    uint g_PsBooleans : packoffset(c33.w);"""),
]


CO_ISSUE_MARKER = "// [svr2011] co-issue: scalar op first"


def reorder_co_issue(path):
    """A Xenos ALU instruction's vector and scalar ops both read the registers
    as they were before the instruction; Unleashed emitted the vector write
    before the scalar op, so a scalar op reading the vector's destination got
    the new value. Emit the scalar op (p0 / ps / a0) before the vector write
    (the scalar write to its register stays last)."""
    text = open(path, encoding="utf-8").read()
    if CO_ISSUE_MARKER in text:
        print(f"{path}: co-issue order already fixed")
        return
    v_start = text.index("    uint32_t vectorWriteMask = instr.vectorWriteMask;")
    s_start = text.index("    if (instr.scalarOpcode != AluScalarOpcode::RetainPrev)", v_start)
    s_end = text.index("    uint32_t scalarWriteMask = instr.scalarWriteMask;", s_start)
    vector_block = text[v_start:s_start]
    scalar_block = text[s_start:s_end]
    text = (text[:v_start] + "    " + CO_ISSUE_MARKER + "\n" + scalar_block + vector_block +
            text[s_end:])
    open(path, "w", encoding="utf-8", newline="\n").write(text)
    print(f"{path}: co-issue order fixed")


# After the reorder: the scalar op's a0 write (MaxAs/MaxAsf) must not be seen
# by the same instruction's vector op either, so it is deferred to after it.
AFTER_REORDER = [
    (r"""    bool closeIfBracket = false;
""", r"""    bool closeIfBracket = false;
    std::string deferredA0;  // [svr2011] scalar a0 write, after the vector op
"""),
    (r"""            println("a0 = (int)clamp(floor({} + 0.5), -256.0, 255.0);", op(SCALAR_0));""",
     r"""            deferredA0 = fmt::format("a0 = (int)clamp(floor({} + 0.5), -256.0, 255.0);\n", op(SCALAR_0));"""),
    (r"""            println("a0 = (int)clamp(floor({}), -256.0, 255.0);", op(SCALAR_0));""",
     r"""            deferredA0 = fmt::format("a0 = (int)clamp(floor({}), -256.0, 255.0);\n", op(SCALAR_0));"""),
    (r"""    uint32_t scalarWriteMask = instr.scalarWriteMask;""",
     r"""    if (!deferredA0.empty())
    {
        indent();
        out += deferredA0;
    }

    uint32_t scalarWriteMask = instr.scalarWriteMask;"""),
]


def main():
    src = sys.argv[1]
    patch(os.path.join(src, "shader_recompiler.cpp"), RECOMPILER)
    reorder_co_issue(os.path.join(src, "shader_recompiler.cpp"))
    patch(os.path.join(src, "shader_recompiler.cpp"), AFTER_REORDER)
    patch(os.path.join(src, "shader_common.h"), COMMON)


if __name__ == "__main__":
    main()
