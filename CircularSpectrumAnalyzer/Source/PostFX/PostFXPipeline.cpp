#include "PostFXPipeline.h"
#include "Shaders.h"
#include "GLProgram.h"

using namespace juce::gl;

//==============================================================================
static void bindTarget (juce::OpenGLFrameBuffer& fbo)
{
    glBindFramebuffer (GL_FRAMEBUFFER, fbo.getFrameBufferID());
    glViewport (0, 0, fbo.getWidth(), fbo.getHeight());
}

static void bindTex (int unit, const juce::OpenGLFrameBuffer& fbo)
{
    glActiveTexture ((GLenum) (GL_TEXTURE0 + unit));
    glBindTexture (GL_TEXTURE_2D, fbo.getTextureID());
}

//==============================================================================
void PostFXPipeline::initialise (juce::OpenGLContext& ctx)
{
    glGenVertexArrays (1, &quadVAO);
    glGenBuffers (1, &quadVBO);

    glBindVertexArray (quadVAO);
    glBindBuffer (GL_ARRAY_BUFFER, quadVBO);
    static const float quad[] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f };
    glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_STATIC_DRAW);
    glBindVertexArray (0);
    glBindBuffer (GL_ARRAY_BUFFER, 0);

    auto makeProgram = [&ctx] (const char* label, const char* frag)
    {
        return GLProgram::make (ctx, label, PostFXShaders::quadVertex, frag);
    };

    persistence = makeProgram ("Persistence", PostFXShaders::persistenceFrag);
    brightPass  = makeProgram ("BrightPass",  PostFXShaders::brightPassFrag);
    gaussian    = makeProgram ("Gaussian",    PostFXShaders::gaussianFrag);
    copy        = makeProgram ("Copy",        PostFXShaders::copyFrag);
    composite   = makeProgram ("Composite",   PostFXShaders::compositeFrag);

    allocatedW = allocatedH = 0;   // force FBO (re)allocation in the new context
}

void PostFXPipeline::shutdown()
{
    persistence.reset();
    brightPass.reset();
    gaussian.reset();
    copy.reset();
    composite.reset();

    sceneFBO.release();
    for (auto& f : accumFBO)       f.release();
    brightFBO.release();
    for (auto& f : blurHalfFBO)    f.release();
    for (auto& f : blurQuarterFBO) f.release();
    halBrightFBO.release();
    for (auto& f : halBlurFBO)     f.release();

    if (quadVBO != 0) { glDeleteBuffers (1, &quadVBO);      quadVBO = 0; }
    if (quadVAO != 0) { glDeleteVertexArrays (1, &quadVAO); quadVAO = 0; }

    allocatedW = allocatedH = 0;
}

juce::OpenGLFrameBuffer& PostFXPipeline::getSceneBuffer (juce::OpenGLContext& ctx, int wPx, int hPx)
{
    if (wPx != allocatedW || hPx != allocatedH)
    {
        allocatedW = wPx;
        allocatedH = hPx;

        const int hw = juce::jmax (1, wPx / 2), hh = juce::jmax (1, hPx / 2);
        const int qw = juce::jmax (1, wPx / 4), qh = juce::jmax (1, hPx / 4);
        const int ew = juce::jmax (1, wPx / 8), eh = juce::jmax (1, hPx / 8);

        sceneFBO.initialise (ctx, wPx, hPx);
        for (auto& f : accumFBO)
        {
            f.initialise (ctx, wPx, hPx);
            f.clear (juce::Colours::black);   // fresh persistence history, no garbage frames
        }
        brightFBO.initialise (ctx, hw, hh);
        for (auto& f : blurHalfFBO)    f.initialise (ctx, hw, hh);
        for (auto& f : blurQuarterFBO) f.initialise (ctx, qw, qh);
        halBrightFBO.initialise (ctx, ew, eh);
        for (auto& f : halBlurFBO)     f.initialise (ctx, ew, eh);

        accumIndex = 0;
    }

    return sceneFBO;
}

//==============================================================================
void PostFXPipeline::drawQuad (juce::OpenGLShaderProgram& prog)
{
    glBindBuffer (GL_ARRAY_BUFFER, quadVBO);
    const GLint loc = glGetAttribLocation (prog.getProgramID(), "aPosition");
    glEnableVertexAttribArray ((GLuint) loc);
    glVertexAttribPointer ((GLuint) loc, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray ((GLuint) loc);
}

juce::OpenGLFrameBuffer& PostFXPipeline::blurChain (juce::OpenGLFrameBuffer& input,
                                                    juce::OpenGLFrameBuffer* pingPong,
                                                    int iterations)
{
    // Alternating horizontal/vertical separable Gaussian passes. The input must
    // not be pingPong[0] (the first destination).
    auto* src = &input;
    const float iw = 1.0f / (float) juce::jmax (1, pingPong[0].getWidth());
    const float ih = 1.0f / (float) juce::jmax (1, pingPong[0].getHeight());

    int flip = 0;
    for (int pass = 0; pass < iterations * 2; ++pass)
    {
        auto& dst = pingPong[flip];
        bindTarget (dst);
        gaussian->use();
        bindTex (0, *src);
        gaussian->setUniform ("uTex", 0);
        if ((pass & 1) == 0) gaussian->setUniform ("uDelta", iw, 0.0f);
        else                 gaussian->setUniform ("uDelta", 0.0f, ih);
        drawQuad (*gaussian);

        src = &dst;
        flip = 1 - flip;
    }

    return *src;
}

void PostFXPipeline::process (juce::OpenGLContext& ctx, const Params& p, int wPx, int hPx)
{
    if (persistence == nullptr || brightPass == nullptr || gaussian == nullptr
        || copy == nullptr || composite == nullptr || allocatedW <= 0)
        return;

    // JUCE's 2D renderer leaves blend/scissor state behind — post passes are all
    // pure texture ops, so run them with everything off.
    glDisable (GL_BLEND);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_SCISSOR_TEST);
    glDisable (GL_STENCIL_TEST);
    glBindVertexArray (quadVAO);

    // --- 1. phosphor persistence: accum = max (scene, prevAccum * decay) ---
    const int dst = 1 - accumIndex;
    bindTarget (accumFBO[dst]);
    persistence->use();
    bindTex (0, sceneFBO);
    bindTex (1, accumFBO[accumIndex]);
    persistence->setUniform ("uScene", 0);
    persistence->setUniform ("uPrev", 1);
    persistence->setUniform ("uDecay", p.decay, p.decayGreen, p.decay);
    drawQuad (*persistence);
    accumIndex = dst;
    auto& accum = accumFBO[accumIndex];

    // --- 2. bright pass into the half-res buffer ---
    bindTarget (brightFBO);
    brightPass->use();
    bindTex (0, accum);
    brightPass->setUniform ("uTex", 0);
    brightPass->setUniform ("uThreshold", p.bloomThreshold);
    drawQuad (*brightPass);

    // --- 3. tight halo: Gaussian ping-pong at half res ---
    const int iters = juce::jmax (1, p.blurIterations);
    auto& tight = blurChain (brightFBO, blurHalfFBO, iters);

    // --- 4. wide haze: downsample the halo to quarter res, blur again ---
    bindTarget (blurQuarterFBO[1]);
    copy->use();
    bindTex (0, tight);
    copy->setUniform ("uTex", 0);
    drawQuad (*copy);
    auto& wide = blurChain (blurQuarterFBO[1], blurQuarterFBO, iters);

    // --- 4b. halation: a second, low-threshold bright pass blurred very wide at
    // 1/8 res. Catches midtones (not just bright cores) and spreads them into a
    // soft glass-scatter halo. Reuses brightPass + blurChain. ---
    bindTarget (halBrightFBO);
    brightPass->use();
    bindTex (0, accum);
    brightPass->setUniform ("uTex", 0);
    brightPass->setUniform ("uThreshold", p.halationThreshold);
    drawQuad (*brightPass);
    auto& halo = blurChain (halBrightFBO, halBlurFBO, iters);

    // --- 5. composite to the context's default framebuffer ---
    glBindFramebuffer (GL_FRAMEBUFFER, (GLuint) ctx.getFrameBufferID());
    glViewport (0, 0, wPx, hPx);
    composite->use();
    bindTex (0, accum);
    bindTex (1, tight);
    bindTex (2, wide);
    bindTex (3, halo);
    composite->setUniform ("uAccum",      0);
    composite->setUniform ("uBloomTight", 1);
    composite->setUniform ("uBloomWide",  2);
    composite->setUniform ("uHalation",   3);
    composite->setUniform ("uResolution", (float) wPx, (float) hPx);
    composite->setUniform ("uTime",           p.time);
    composite->setUniform ("uBloomIntensity", p.bloomIntensity);
    composite->setUniform ("uBarrel",         p.barrel);
    composite->setUniform ("uDriftAmpPx",     p.driftAmpPx);
    composite->setUniform ("uJitterAmpPx",    p.jitterAmpPx);
    composite->setUniform ("uJitterGate",     p.jitterGate);
    composite->setUniform ("uFlicker",        p.flicker);
    composite->setUniform ("uScanSpacingPx",  p.scanSpacingPx);
    composite->setUniform ("uScanAlpha",      p.scanAlpha);
    composite->setUniform ("uMaskStrength",   p.maskStrength);
    composite->setUniform ("uGrain",          p.grain);
    composite->setUniform ("uEnergy",         p.energy);
    composite->setUniform ("uSparkAmount",    p.sparkAmount);
    composite->setUniform ("uSparkGain",      p.sparkGain);
    composite->setUniform ("uVigStart",       p.vigStart);
    composite->setUniform ("uVigEdge",        p.vigEdge);
    composite->setUniform ("uBoxHalfDiagPx",  p.boxHalfDiagPx);
    composite->setUniform ("uTubeCentreUV",   p.tubeCentreU, p.tubeCentreV);
    composite->setUniform ("uPhosphorTint",   p.phosphorTint.r, p.phosphorTint.g, p.phosphorTint.b);
    composite->setUniform ("uGrainTint",      p.grainTint.r,    p.grainTint.g,    p.grainTint.b);
    composite->setUniform ("uConvergencePx",  p.convergencePx);
    composite->setUniform ("uHalationIntensity", p.halationIntensity);
    composite->setUniform ("uHalationTint",   p.halationTint.r, p.halationTint.g, p.halationTint.b);
    composite->setUniform ("uRollOffset",     p.rollOffset);
    composite->setUniform ("uDegauss",        p.degauss);
    composite->setUniform ("uRetrace",        p.retrace);
    drawQuad (*composite);

    // leave GL in a tame state for JUCE's component-layer compositing
    glActiveTexture (GL_TEXTURE0);
    glBindVertexArray (0);
    glBindBuffer (GL_ARRAY_BUFFER, 0);
}
