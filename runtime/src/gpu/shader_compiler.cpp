// GLSL -> SPIR-V with glslang (linked statically).
#include "shader_translator.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <mutex>

// SPIR-V for the device's Vulkan version (1.1 drivers do not take SPIR-V 1.6)
static int s_vkMinor = 3;
void ShaderCompilerSetVulkanVersion(int minor) { s_vkMinor = minor; }
static void SetTargets(glslang::TShader& shader)
{
    if (s_vkMinor >= 3) { shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3); shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6); }
    else if (s_vkMinor == 2) { shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2); shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5); }
    else { shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1); shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3); }
}

bool CompileGlsl(bool pixel, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& error)
{
    static std::once_flag init;
    std::call_once(init, [] { glslang::InitializeProcess(); });
    EShLanguage stage = pixel ? EShLangFragment : EShLangVertex;
    glslang::TShader shader(stage);
    const char* src = glsl.c_str();
    shader.setStrings(&src, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    SetTargets(shader);
    EShMessages msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 460, false, msgs))
    {
        error = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs))
    {
        error = program.getInfoLog();
        return false;
    }
    spirv.clear();
    glslang::SpvOptions opt;
    opt.disableOptimizer = false;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &opt);
    return !spirv.empty();
}

// Generic stage compile (0 = vertex, 1 = fragment, 2 = geometry), used for the renderer's
// fixed geometry shaders.
bool CompileGlslStage(int stageIndex, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& error)
{
    static std::once_flag init;
    std::call_once(init, [] { glslang::InitializeProcess(); });
    EShLanguage stage = stageIndex == 3 ? EShLangCompute : stageIndex == 2 ? EShLangGeometry : stageIndex == 1 ? EShLangFragment : EShLangVertex;
    glslang::TShader shader(stage);
    const char* src = glsl.c_str();
    shader.setStrings(&src, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    SetTargets(shader);
    EShMessages msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 460, false, msgs)) { error = shader.getInfoLog(); return false; }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) { error = program.getInfoLog(); return false; }
    spirv.clear();
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
    return !spirv.empty();
}
