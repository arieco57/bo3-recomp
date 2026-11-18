#pragma once
// Xenos (Xbox 360 GPU) shader microcode -> GLSL 4.60 (Vulkan) translator.
#include <cstdint>
#include <string>
#include <vector>

struct TranslatedShader
{
    bool pixel = false;
    std::string glsl;
    std::vector<uint32_t> spirv;
    std::vector<uint32_t> spirvRect, spirvQuad;   // VS without geometry shaders: rect / quad list variants
    std::string error;           // non-empty on failure
    uint32_t texturesUsed = 0;   // bitmask of texture fetch constants sampled
    uint8_t textureDim[32] = {}; // xenos::FetchOpDimension per used fetch constant
    uint8_t texMag[32] = { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };
    uint8_t texMin[32] = { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };
                                 // tfetch filter overrides per fetch constant (3 = use the constant's)
    uint32_t colorExports = 0;   // PS: bitmask of oC0..3 written
    bool writesDepth = false;
    bool kills = false;
    uint32_t memexports = 0;     // count of unsupported memexport instructions
    uint8_t memexportConsts[4] = {};   // float constants read by the eA writes (stream constants: address range)
    uint8_t memexportConstCount = 0;
    uint64_t constChunks = ~0ull;  // float constants read, bit i = c[4i..4i+3] (all with relative addressing)
    uint64_t fetchVec4 = ~0ull;    // fetch constants read, bit i = fetch dwords 4i..4i+3
    // vertex fetches per vertex fetch constant (0..95): bit0 used, bit1 indexed by something else than the
    // vertex index r0.x (bone matrices...); stride and the dword range read within a vertex (copy mode uploads)
    uint8_t vfFlags[96] = {};
    uint16_t vfStride[96] = {}, vfMinOff[96] = {}, vfMaxEnd[96] = {};
};

// Layout shared with the renderer (std140, one block per stage):
//   vec4  c[256];       float constants of this stage
//   uvec4 fetch[48];    32 fetch constants x 6 dwords
//   uvec4 boolc[2];     256 boolean constants (bit i of dword i/32)
//   uvec4 loopc[8];     32 loop constants
//   vec4  ndcScale;     guest clip -> host NDC: xy scale, z scale
//   vec4  ndcOffset;    xy, z offset (multiplied by w)
//   uvec4 drawInfo;     x = index base (bytes, physical), y = flags (bit0 indexed, bit1 32-bit),
//                       z = index endian, w = index offset (VGT_INDX_OFFSET)
//   uvec4 psInfo;       x = param gen register (0xFF = none)
// (ndcScale, ndcOffset, drawInfo, psInfo are push constants: 64 bytes per stage, pixel at 64)
constexpr uint32_t kStageStateSize = 256 * 16 + 48 * 16 + 2 * 16 + 8 * 16;

TranslatedShader TranslateShader(bool pixel, const uint32_t* code, uint32_t dwords);

// GLSL -> SPIR-V (glslang). Returns false and fills `error` on failure.
bool CompileGlsl(bool pixel, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& error);

// Changes whenever the translator is rebuilt (shader disk cache invalidation).
const char* ShaderTranslatorBuildId();
// light shaders: plain IEEE multiplies, no inf/nan preservation (faster; see shader_translator.cpp)
void ShaderTranslatorSetLite(bool on);
bool ShaderTranslatorLite();
// GPUs without geometry shaders: vertex shaders also get rect / quad list variants (set before translating)
void ShaderTranslatorSetPrimEmu(bool on);
bool ShaderTranslatorPrimEmu();
// the device's Vulkan minor version (SPIR-V target) and inf/nan preservation support: part of the cache identity
void ShaderTranslatorSetDevice(int vulkanMinor, bool floatControls);
void ShaderCompilerSetVulkanVersion(int minor);
