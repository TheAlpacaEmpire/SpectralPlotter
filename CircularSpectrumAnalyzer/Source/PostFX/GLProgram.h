#pragma once

#include <JuceHeader.h>
#include <memory>

// Shared shader-program compile/link helper for the GPU passes (scene renderer and post-FX
// pipeline both used to carry a private copy). Sources are translated to GLSL v3 to match the
// rest of the pipeline. On failure it logs the driver error via juce::Logger — so the cause is
// visible in Release builds too, not just a DBG that vanishes — optionally records it into
// lastError for on-screen surfacing, and returns nullptr.
namespace GLProgram
{
    inline std::unique_ptr<juce::OpenGLShaderProgram>
    make (juce::OpenGLContext& ctx,
          const char* label,
          const char* vertexSource,
          const char* fragmentSource,
          juce::String* lastError = nullptr)
    {
        auto prog = std::make_unique<juce::OpenGLShaderProgram> (ctx);

        const bool ok = prog->addVertexShader   (juce::OpenGLHelpers::translateVertexShaderToV3   (vertexSource))
                     && prog->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fragmentSource))
                     && prog->link();

        if (! ok)
        {
            const juce::String msg = juce::String (label) + " shader failed to compile: " + prog->getLastError();
            juce::Logger::writeToLog (msg);
            DBG (msg);
            if (lastError != nullptr)
                *lastError = msg;
            jassertfalse;
            prog.reset();
        }

        return prog;
    }
}
