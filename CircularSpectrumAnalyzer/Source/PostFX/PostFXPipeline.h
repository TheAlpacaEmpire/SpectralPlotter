#pragma once

#include <JuceHeader.h>
#include "Themes.h"

// GPU post-processing chain giving the 2D scene an analog CRT/phosphor character:
//   scene FBO -> phosphor persistence (ping-pong accumulation, per-channel decay)
//             -> bright pass + two-tier Gaussian bloom (half + quarter res)
//             -> composite to screen (barrel, drift/jitter, scanlines, slot mask,
//                grain, vignette, green bias, flicker).
// Every method must be called on the GL render thread, between
// newOpenGLContextCreated() and openGLContextClosing().
class PostFXPipeline
{
public:
    PostFXPipeline() = default;

    struct Params
    {
        float time = 0.0f;   // scene animation clock, drives all shader motion

        // phosphor persistence: per-frame decay of the accumulation buffer.
        // Green lingers longest (P31 phosphor feel).
        float decay      = 0.86f;
        float decayGreen = 0.90f;

        // bloom — threshold gates what glows: keep it high enough that only the
        // bright river/index lines bloom, so the fine contour lines stay crisp.
        float bloomThreshold = 0.58f;
        float bloomIntensity = 0.85f;

        // composite / CRT character (px values are physical pixels)
        float barrel        = 0.05f;
        float driftAmpPx    = 0.6f;
        float jitterAmpPx   = 4.0f;
        float jitterGate    = 0.9875f;  // hash threshold; lower = more line tears
        float flicker       = 0.03f;
        float scanSpacingPx = 3.0f;     // parity with the old CPU scanlines
        float scanAlpha     = 0.30f;
        float maskStrength  = 0.05f;
        float grain         = 0.16f;    // baked phosphor speckle amplitude in the dark field
        float energy        = 0.0f;     // overall song energy: excites the phosphor grain

        // Phosphor colour character (per-theme). Defaults reproduce the original
        // green-tube look; the composite pass multiplies the final image by
        // phosphorTint and tints grain + sparks by grainTint.
        Themes::Tint phosphorTint { 0.93f, 1.04f, 0.95f };
        Themes::Tint grainTint    { 0.55f, 1.00f, 0.62f };

        // CRT convergence + halation (both scaled live by the master CRT-FX slider).
        // convergencePx: max R/B raster split at the tube edge, physical px.
        // halation: a wide low-threshold glow-diffusion (light scattering in the
        // glass); tint follows the active theme's phosphor colour.
        float convergencePx     = 0.0f;
        float halationThreshold = 0.14f;
        float halationIntensity = 0.0f;
        Themes::Tint halationTint { 0.93f, 1.04f, 0.95f };

        // Beat-reactive CRT effects (roll + degauss are audio-driven envelopes;
        // retrace is an ambient sweep). All pre-scaled by the master CRT-FX slider.
        float rollOffset = 0.0f;   // vertical-hold slip, fraction of screen height
        float degauss    = 0.0f;   // degauss ripple amplitude, decaying on a beat
        float retrace    = 0.0f;   // beam-retrace sweep-line intensity

        // background analog noise
        float sparkAmount   = 0.06f;    // fraction of cells that ever spark (audio-driven)
        float sparkGain     = 1.1f;     // brightness of each glowing dot

        // vignette (parity with the old CPU 12-stop radial gradient)
        float vigStart      = 0.30f;
        float vigEdge       = 0.97f;
        float boxHalfDiagPx = 424.0f;   // map-box half diagonal, sets vignette radius

        // CRT tube (map box) centre in UV space (y-up); barrel + vignette pivot
        // here now that the bezel makes the tube off-centre in the window.
        float tubeCentreU   = 0.5f;
        float tubeCentreV   = 0.5f;

        int blurIterations = 2;         // perf knob: H+V pairs per bloom tier
    };

    // (Re)creates shaders and geometry for a fresh GL context.
    void initialise (juce::OpenGLContext& ctx);
    // Releases all GL resources; call from openGLContextClosing().
    void shutdown();

    // Returns the scene framebuffer, (re)allocating every FBO when the physical
    // pixel size changes. Draw the 2D scene into this, then call process().
    juce::OpenGLFrameBuffer& getSceneBuffer (juce::OpenGLContext& ctx, int wPx, int hPx);

    // Runs persistence -> bloom -> composite, ending on the context's default
    // framebuffer at the given viewport size.
    void process (juce::OpenGLContext& ctx, const Params& p, int wPx, int hPx);

private:
    void drawQuad (juce::OpenGLShaderProgram& prog);
    juce::OpenGLFrameBuffer& blurChain (juce::OpenGLFrameBuffer& input,
                                        juce::OpenGLFrameBuffer* pingPong,
                                        int iterations);

    juce::OpenGLFrameBuffer sceneFBO;          // full res
    juce::OpenGLFrameBuffer accumFBO[2];       // full res, persistence ping-pong
    juce::OpenGLFrameBuffer brightFBO;         // half res
    juce::OpenGLFrameBuffer blurHalfFBO[2];    // half res ping-pong (tight halo)
    juce::OpenGLFrameBuffer blurQuarterFBO[2]; // quarter res ping-pong (wide haze)
    juce::OpenGLFrameBuffer halBrightFBO;      // eighth res (low-threshold bright pass)
    juce::OpenGLFrameBuffer halBlurFBO[2];     // eighth res ping-pong (wide halation)

    std::unique_ptr<juce::OpenGLShaderProgram> persistence, brightPass, gaussian, copy, composite;

    unsigned int quadVAO = 0, quadVBO = 0;
    int accumIndex = 0, allocatedW = 0, allocatedH = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PostFXPipeline)
};
