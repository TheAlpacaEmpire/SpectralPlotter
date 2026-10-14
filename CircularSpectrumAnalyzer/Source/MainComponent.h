#pragma once

#include <JuceHeader.h>

#include "Audio/AudioAnalyzer.h"
#include "Terrain/TerrainModel.h"
#include "River/RiverModel.h"
#include "PostFX/PostFXPipeline.h"
#include "PostFX/SceneRenderer.h"
#include "PostFX/Themes.h"
#include "Labels/GlyphAtlas.h"
#include "Labels/LabelPlanner.h"
#include "UI/ScreenLayout.h"
#include "UI/ScreenTextBuilder.h"
#include "UI/AppSettings.h"

class MainComponent : public juce::Component,
                      public juce::Timer,
                      public juce::OpenGLRenderer
{
public:
    MainComponent();
    ~MainComponent() override;

    // The whole window is the CRT screen: every pixel (map, header, telemetry,
    // command line) is GL-rendered through the phosphor pipeline. No JUCE
    // component painting at all — the message thread only handles input and
    // model updates.
    void timerCallback() override;
    bool keyPressed (const juce::KeyPress&) override;   // N/S/L/T/I/F/R/[/]/-/= commands
    void mouseDown (const juce::MouseEvent&) override;  // begin header window-drag
    void mouseDrag (const juce::MouseEvent&) override;  // move the borderless window
    void mouseUp (const juce::MouseEvent&) override;    // command-line / control hotspots
    void mouseMove (const juce::MouseEvent&) override;  // hand cursor over window controls

    // Mouse position (component px) -> the scene-space point actually displayed there.
    // The post-FX composite barrel-curves the whole image toward the centre (most at the
    // corners, where the header controls live), so hit-testing raw mouse coords against
    // layout rects missed the drawn [+]/[x] by ~8-9 px. Same mapping as the shader.
    juce::Point<float> toScenePoint (juce::Point<float> p) const noexcept;

    // Settings persistence (message thread). The app restores the window once it is on
    // the desktop, and saves everything on shutdown.
    void restoreWindowState();
    void saveSettings();

    // OpenGLRenderer (GL thread)
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

private:
    // Everything the GL-thread scene renderer reads, copied per frame on the
    // message thread under snapshotLock. Vectors reuse their capacity after the
    // first frame, so publishing is allocation-free in steady state.
    struct RenderSnapshot
    {
        float heightFieldMax = 1.0f;
        std::vector<juce::Point<float>> riverPts;   // swayed backbone, grid space
        std::vector<float> riverWidths;             // bank half-widths, screen px
        float riverPhase = 0.0f, riverFlash = 0.0f;
        AudioFeatures feats;
        float animTime = 0.0f, currentHue = 0.376f;
        int   themeIndex = 0;                       // active palette (Themes::kThemes)
        float crtRoll = 0.0f, crtDegauss = 0.0f;    // beat-reactive CRT envelopes
        // Crossfaded theme state (eased on the message thread; GL reads these
        // instead of Themes::get so a palette switch fades smoothly).
        float liveSat  = 1.0f, liveCaLo = 0.0f, liveCaHi = 0.62f;
        float liveConv = 1.0f, liveHal  = 1.0f;
        float liveElevSpread = 0.015f, liveElevFill = 0.0f;   // elevation hue ramp
        Themes::Tint liveBg        { 0.012f, 0.031f, 0.012f };   // map-box background wash
        Themes::Tint liveDecayBias { 0.0f, 0.03f, 0.0f };      // per-channel persistence bias
        float liveThermal = 0.0f, liveLowDim = 0.0f;
        Themes::Tint livePhosphor { 0.93f, 1.04f, 0.95f };
        Themes::Tint liveGrain    { 0.55f, 1.00f, 0.62f };
        uint32_t seed = 0;
        int logicalW = 600, logicalH = 600;
        int sampleRate = 0;                         // for the header readout

        // GPU height-compose inputs (Landscape): per-peak amps + river depression
        std::array<float, TerrainModel::numPeaks> peakAmps {};
        std::vector<float> minDist2;                // gridSize^2, per frame
        float dynDepth = 0.0f, riverSigma = 1.0f;

        // In-scene map labels (grid space); GL thread turns them into glyph quads
        // + contour gaps. Small enough to copy every frame; clutter is static per
        // seed but rides along here for simplicity.
        std::vector<Labels::ContourLabel> contourLabels;
        std::vector<Labels::SpotLabel>    spotLabels;
        std::vector<Labels::ClutterMark>  clutter;
    };

    // Heavier GPU upload payloads, staged by the message thread and consumed by
    // the GL thread under the same snapshotLock. Kept out of RenderSnapshot so
    // the per-frame glLocal = shared copy doesn't duplicate 10 clean planes.
    struct GpuUploads
    {
        juce::uint32 planeDirtyBits = 0;   // bit pk => planes[pk] holds fresh data
        std::array<std::vector<float>, TerrainModel::numPeaks> planes;
        bool baseDirty = false;            // baseField + peakMask re-upload (reseed)
        std::vector<float> baseField, peakMask;
    };

    // River geometry in logical screen coords — shared by the GPU mask pass and
    // the ribbon builder, so the contour clip and the drawn banks agree exactly.
    struct RiverGeometry
    {
        std::vector<juce::Point<float>> cPts, lPts, rPts;
        float avgBankHalfW = 4.0f;
        int   nPts = 0;
    };

    void reseed();       // picks a new random seed and rebuilds terrain + river from scratch
    void loadSettings(); // applies saved knobs / palette / audio source (constructor)

    // Two-step factory reset (R key / [ RESET ] control): the first call arms it for
    // resetConfirmMs, a second call within that window restores the default look.
    void requestReset();
    void resetToDefaults();
    static constexpr juce::uint32 resetConfirmMs = 3000;
    std::atomic<juce::uint32> resetArmedUntil { 0 };   // ms counter; read by the GL thread
    std::array<float, ScreenLayout::numCtrl> knobDefaults {};   // captured before loadSettings

    AppSettings settings;

    void publishSnapshot();                                        // message thread
    // These GL-thread builders share one ScreenLayout, computed once per frame in
    // renderOpenGL and passed in (it was formerly recomputed inside each — identical inputs).
    RiverGeometry buildRiverGeometry (const RenderSnapshot& s, const ScreenLayout& sl) const;
    SceneRenderer::RibbonBatch buildRiverRibbons (const RenderSnapshot& s,
                                                  const RiverGeometry& rg,
                                                  const ScreenLayout& sl,
                                                  float renderScale) const;
    // Turns the snapshot's label records into glyph quads and fills f.gap* with the
    // contour-gap capsules (map-local logical px). GL thread.
    SceneRenderer::GlyphBatch buildLabelGlyphs (const RenderSnapshot& s,
                                                const ScreenLayout& sl,
                                                SceneRenderer::Frame& f) const;
    // Maps this frame's snapshot + control-panel trims into the post-FX parameter block.
    // Non-const: also publishes the effective decay/bloom back to the telemetry atomics.
    // GL thread.
    PostFXPipeline::Params mapPostFXParams (const RenderSnapshot& s, float renderScale,
                                            int wPx, int hPx);
    void saveSceneScreenshot (juce::OpenGLFrameBuffer& fbo);                        // GL thread

    AudioAnalyzer      audio;
    TerrainModel       terrain;
    RiverModel         river { TerrainModel::gridSize };
    GlyphAtlas         glyphAtlas;
    Labels::LabelPlanner labelPlanner;
    int                plannerCounter = 0;

    // Animation state
    float animTime   = 0.0f;
    float currentHue = 0.376f;  // neon-green in HSV (135°)
    int   themeIndex = 0;       // active palette; cycled by T / [ THEME ] button

    // Live (crossfaded) theme state, eased toward the active palette each frame so a
    // palette switch fades rather than snaps (the hue already eases via currentHue).
    float liveSat  = 1.0f, liveCaLo = 0.0f, liveCaHi = 0.62f;
    float liveConv = 1.0f, liveHal  = 1.0f;
    float liveElevSpread = 0.015f, liveElevFill = 0.0f;   // elevation hue ramp
    Themes::Tint liveBg        { 0.012f, 0.031f, 0.012f };   // map-box background wash
    Themes::Tint liveDecayBias { 0.0f, 0.03f, 0.0f };      // per-channel persistence bias
    float liveThermal = 0.0f, liveLowDim = 0.0f;
    Themes::Tint livePhosphor { 0.93f, 1.04f, 0.95f };
    Themes::Tint liveGrain    { 0.55f, 1.00f, 0.62f };

    void cycleTheme (int step = 1);   // message thread: step palette (+1 next, -1 previous)
    void cycleInput (int step = 1);   // message thread: step audio source (+1 next, -1 previous)

    // True (kiosk) fullscreen toggle — borderless, covers the whole display. Bound to the
    // header [+]/[-] control, the F / F11 keys, and Esc (exit only).
    void toggleFullscreen();
    bool isFullscreen() const noexcept;

    // Beat-reactive CRT state (message thread): a bass onset detector drives the
    // vertical-hold roll and degauss ripple envelopes, published to the GL thread.
    float bassBaseline = 0.0f;  // slow floor tracking bassEnergy
    float onsetTimer   = 0.0f;  // refractory countdown between beats (s)
    float rollTarget   = 0.0f;  // impulse target set on a beat, then decays
    float degaussTarget= 0.0f;
    float rollEnv      = 0.0f;  // live envelope easing toward rollTarget (smooth attack)
    float degaussEnv   = 0.0f;  // live envelope easing toward degaussTarget

    bool hasGrabbedFocus  = false;   // one-shot keyboard-focus grab after startup

    // 80s stepped refresh: the terrain/river/label state is published to the GL thread
    // at terrainStepHz (the models still simulate at 60 Hz), so contours advance in
    // discrete jumps like an old machine redrawing its display, not a smooth glide.
    // Everything else in the snapshot (hue, CRT envelopes, clock) stays at 60 Hz.
    static constexpr float terrainStepHz = 20.0f;
    float terrainStepAcc = 1.0f;     // >= 1 => publish terrain this frame

    double lastGLFrameMs = 0.0;      // GL thread: previous frame time, for dt-correct persistence

    // Command state crossing to the GL thread (renderOpenGL applies the trims),
    // and effective post-FX values mirrored back for the on-screen telemetry.
    std::atomic<bool>  hueLocked      { false };
    std::atomic<float> persistTrimVal { 0.0f };
    std::atomic<float> bloomTrimVal   { 1.0f };
    std::atomic<float> bloomThreshVal { 0.58f };   // post-FX bloom threshold
    std::atomic<float> scanSpacingVal { 3.0f };    // CRT scanline pitch (logical px)
    std::atomic<float> crtTrimVal     { 0.5f };    // master CRT-FX (convergence + halation)
    std::atomic<float> shadeVal       { 0.18f };   // scene: hillshade strength
    std::atomic<float> hatchVal       { 0.65f };   // scene: slope-hachure strength
    std::atomic<float> stippleVal     { 0.045f };  // scene: hypsometric stipple strength
    std::atomic<float> slopeVal       { 12.0f };   // scene: slope exaggeration (uSlopeK)
    std::atomic<float> effDecay       { 0.0f };
    std::atomic<float> effBloom       { 0.0f };

    void stepPersist (float delta);   // message thread: clamp + store
    void stepBloom   (float delta);
    void stepThresh  (float delta);
    void stepScan    (float delta);
    void stepCrt     (float delta);

    // Control-panel value sliders share one range table so the '< >' steppers and the
    // click-to-set value bar both stay consistent. knobFor maps a ScreenLayout ctrl
    // index to its backing atomic + range; only slider rows return a non-null val.
    struct Knob { std::atomic<float>* val = nullptr; float lo = 0.0f, hi = 1.0f, step = 0.05f; };
    Knob knobFor    (int ctrlIndex) noexcept;
    void setKnobFrac (int ctrlIndex, float frac);   // click-to-set from 0..1 bar position
    void stepKnob    (int ctrlIndex, float dir);    // '< >' stepper, dir = -1 / +1

    // CRT post-processing (GL thread) + cross-thread scene snapshot
    juce::OpenGLContext glContext;
    PostFXPipeline      postFX;
    SceneRenderer       sceneRenderer;
    ScreenTextBuilder   screenText;    // GL thread: terminal UI glyph batches
    SceneRenderer::GlyphBatch uiGlyphs;   // GL thread: reused per-frame UI batch

    // Debug: S key dumps the pre-postFX scene FBO to a PNG
    std::atomic<bool>  screenshotRequested { false };

    // Borderless-windowed-fullscreen state, set on the message thread in toggleFullscreen()
    // and read lock-free by the GL thread for the header [+]/[-] glyph. windowedBounds holds
    // the pre-fullscreen window rectangle so it can be restored.
    std::atomic<bool>      fullscreenFlag { false };
    juce::Rectangle<int>   windowedBounds;

    // Borderless-window move: dragging the CRT header repositions the top-level window
    // (there's no OS title bar to grab). Message thread only.
    juce::ComponentDragger windowDragger;
    bool draggingWindow = false;

    // Rolling frame-time averages for the HUD (accumulators are single-thread-local)
    std::atomic<float> sceneMsAvg  { 0.0f };
    std::atomic<float> updateMsAvg { 0.0f };
    double sceneMsAccum  = 0.0; int sceneMsCount  = 0;   // GL thread only
    double updateMsAccum = 0.0; int updateMsCount = 0;   // message thread only

    juce::SpinLock      snapshotLock;
    RenderSnapshot      shared;          // written by message thread under lock
    RenderSnapshot      glLocal;         // GL thread's working copy
    GpuUploads          sharedUploads;   // staged by message thread under lock
    GpuUploads          glUploads;       // GL thread's working set (dirty payloads swapped in)
    bool terrainStaticsDirty = true;     // message thread: base field / peak mask changed (reseed)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
