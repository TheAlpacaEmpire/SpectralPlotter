#include "MainComponent.h"
#include "Terrain/NoiseUtils.h"

#include <cstring>
#include <random>

//==============================================================================
MainComponent::MainComponent()
{
    setSize (624, 724);   // ~600 px map plot + header band + telemetry/command rows
    setWantsKeyboardFocus (true);   // single-key commands (see keyPressed)

    terrain.reseed ((uint32_t) std::random_device{}());
    river.buildPath (terrain.getBaseHeightField(), terrain.getSeed());
    river.carveChannel (terrain.getBaseHeightFieldMutable());

    // Glyph atlas for the in-scene labels: rasterize once here (message thread —
    // JUCE font rendering isn't GL-thread-safe) and stage it for GL upload.
    glyphAtlas.build();
    sceneRenderer.setAtlas (glyphAtlas.getWidth(), glyphAtlas.getHeight(), glyphAtlas.getPixels());
    labelPlanner.rebuildClutter (terrain.getSeed(), river,
                                 GlyphAtlas::GDot, GlyphAtlas::GTick, GlyphAtlas::GCross);

    screenText.setAdvanceFrac (glyphAtlas.getAdvanceFrac());

    publishSnapshot();   // GL thread may render before the first timer tick

    // Everything on screen is GL-rendered through the phosphor pipeline; there
    // is no JUCE overlay at all, so component painting is off and the GL thread
    // never has to take the MessageManager lock.
    glContext.setRenderer (this);
    glContext.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);
    glContext.setComponentPaintingEnabled (false);
    // Continuous repainting lets renderOpenGL() run WITHOUT holding the MessageManager
    // lock (JUCE only takes that lock on frames where a component repaint is pending).
    // Driving frames with repaint()/triggerRepaint() instead would lock the message
    // thread for the whole heavy scene render every frame — spinning cursor, dead clicks.
    glContext.setContinuousRepainting (true);
    glContext.attachTo (*this);

    for (int i = ScreenLayout::ctFirstSlider; i < ScreenLayout::numCtrl; ++i)
        if (const Knob k = knobFor (i); k.val != nullptr)
            knobDefaults[(size_t) i] = k.val->load();

    loadSettings();   // knobs, palette, audio source — before capture starts

    audio.start();
    startTimerHz (60);
}

MainComponent::~MainComponent()
{
    glContext.detach();   // blocks until the GL thread is done — must precede member teardown
    stopTimer();
}

//==============================================================================
void MainComponent::reseed()
{
    const uint32_t newSeed = (uint32_t) std::random_device{}();

    audio.resetFastEnvelope();
    terrain.reseed (newSeed);
    river.buildPath (terrain.getBaseHeightField(), terrain.getSeed());
    river.carveChannel (terrain.getBaseHeightFieldMutable());
    river.resetAnimationState();
    terrainStaticsDirty = true;   // base field + peak mask need a GPU re-upload

    // Labels are terrain-specific: drop the old ones and re-scatter the clutter.
    labelPlanner.clearDynamic();
    labelPlanner.rebuildClutter (newSeed, river,
                                 GlyphAtlas::GDot, GlyphAtlas::GTick, GlyphAtlas::GCross);
}

//==============================================================================
// Command input. The screen's command line mirrors these: N=[NEW SEED],
// L=hue lock, T / Shift+T=next / previous palette, I / Shift+I=next / previous
// audio source, [ ]=persist trim down/up,
// - ==bloom trim down/up.
void MainComponent::stepPersist (float delta)
{
    persistTrimVal = juce::jlimit (-0.05f, 0.05f, persistTrimVal.load() + delta);
}

void MainComponent::stepBloom (float delta)
{
    bloomTrimVal = juce::jlimit (0.5f, 1.5f, bloomTrimVal.load() + delta);
}

void MainComponent::stepThresh (float delta)
{
    bloomThreshVal = juce::jlimit (0.20f, 0.90f, bloomThreshVal.load() + delta);
}

void MainComponent::stepScan (float delta)
{
    scanSpacingVal = juce::jlimit (1.0f, 6.0f, scanSpacingVal.load() + delta);
}

void MainComponent::stepCrt (float delta)
{
    crtTrimVal = juce::jlimit (0.0f, 1.0f, crtTrimVal.load() + delta);
}

MainComponent::Knob MainComponent::knobFor (int ctrlIndex) noexcept
{
    switch (ctrlIndex)
    {
        case ScreenLayout::ctPersist: return { &persistTrimVal, -0.05f, 0.05f, 0.005f };
        case ScreenLayout::ctBloom:   return { &bloomTrimVal,    0.50f, 1.50f, 0.05f  };
        case ScreenLayout::ctThresh:  return { &bloomThreshVal,  0.20f, 0.90f, 0.02f  };
        case ScreenLayout::ctScan:    return { &scanSpacingVal,  1.00f, 6.00f, 0.50f  };
        case ScreenLayout::ctCrt:     return { &crtTrimVal,      0.00f, 1.00f, 0.05f  };
        case ScreenLayout::ctShade:   return { &shadeVal,        0.00f, 0.50f, 0.02f  };
        case ScreenLayout::ctHatch:   return { &hatchVal,        0.00f, 1.00f, 0.05f  };
        case ScreenLayout::ctStipple: return { &stippleVal,      0.00f, 0.12f, 0.005f };
        case ScreenLayout::ctSlope:   return { &slopeVal,        4.00f, 24.0f, 1.00f  };
        case ScreenLayout::ctGain:    return { &gainVal,       -10.0f, 50.0f, 2.00f  };
        default:                      return {};
    }
}

void MainComponent::setKnobFrac (int ctrlIndex, float frac)
{
    const Knob k = knobFor (ctrlIndex);
    if (k.val != nullptr)
        k.val->store (juce::jlimit (k.lo, k.hi, k.lo + juce::jlimit (0.0f, 1.0f, frac) * (k.hi - k.lo)));
}

void MainComponent::stepKnob (int ctrlIndex, float dir)
{
    const Knob k = knobFor (ctrlIndex);
    if (k.val != nullptr)
        k.val->store (juce::jlimit (k.lo, k.hi, k.val->load() + dir * k.step));
}

void MainComponent::cycleTheme (int step)
{
    themeIndex = (themeIndex + step + Themes::numThemes) % Themes::numThemes;

    // Ease to the new palette the short way round the hue circle (VAPOR 95 at 0.92 ->
    // LIME P1 at 0.25 would otherwise sweep the whole rainbow): shift the eased value by
    // a full turn so the plain lerp in timerCallback crosses the 0/1 seam. It is
    // re-wrapped when published, so consumers always see 0..1.
    const float d = Themes::get (themeIndex).baseHue - currentHue;
    if      (d >  0.5f) currentHue += 1.0f;
    else if (d < -0.5f) currentHue -= 1.0f;
}

void MainComponent::cycleInput (int step)
{
    audio.cycleInput (step);     // capture thread restarts on the next / previous device
    audio.resetFastEnvelope();   // drop stale transient state from the old source
}

bool MainComponent::isFullscreen() const noexcept
{
    return fullscreenFlag.load();
}

void MainComponent::toggleFullscreen()
{
    auto* window = getTopLevelComponent();
    if (window == nullptr)
        return;

    // Borderless *windowed* fullscreen: resize the (already title-bar-less) window to cover the
    // whole display and back. Deliberately NOT kiosk / exclusive fullscreen — Windows auto-
    // minimizes an exclusive-fullscreen window when it loses focus, which we don't want.
    if (! fullscreenFlag.load())
    {
        windowedBounds = window->getBounds();   // remember to restore on exit

        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* disp = displays.getDisplayForRect (window->getScreenBounds());
        if (disp == nullptr)
            disp = displays.getPrimaryDisplay();

        if (disp != nullptr)
            window->setBounds (disp->logicalBounds.toNearestInt());   // whole screen, taskbar included

        fullscreenFlag.store (true);
    }
    else
    {
        if (! windowedBounds.isEmpty())
            window->setBounds (windowedBounds);
        fullscreenFlag.store (false);
    }
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    // F11 toggles fullscreen; Esc leaves it (but is ignored when windowed so it
    // doesn't swallow other Escape handling).
    if (key.isKeyCode (juce::KeyPress::F11Key))                      { toggleFullscreen(); return true; }
    if (key.isKeyCode (juce::KeyPress::escapeKey) && isFullscreen()) { toggleFullscreen(); return true; }

    switch (juce::CharacterFunctions::toLowerCase (key.getTextCharacter()))
    {
        case 's': screenshotRequested = true;   return true;   // dump scene FBO to PNG
        case 'n': reseed();                     return true;   // same as [ NEW SEED ]
        case 'l': hueLocked = ! hueLocked;      return true;
        case 'g': autoGainOn = ! autoGainOn;    return true;   // auto-gain on / off
        case 't': cycleTheme (key.getModifiers().isShiftDown() ? -1 : 1);
                  return true;                                 // next / previous palette
        case 'i': cycleInput (key.getModifiers().isShiftDown() ? -1 : 1);
                  return true;                                 // next / previous audio source
        case 'r': requestReset();               return true;   // factory reset (press twice)
        case 'f': toggleFullscreen();           return true;   // true (kiosk) fullscreen
        case '[': stepPersist (-0.005f);        return true;
        case ']': stepPersist (+0.005f);        return true;
        case '-': stepBloom (-0.05f);           return true;
        case '=': case '+': stepBloom (0.05f);  return true;
        default:  return false;
    }
}

juce::Point<float> MainComponent::toScenePoint (juce::Point<float> p) const noexcept
{
    // Mirrors the composite shader: uv = centre + cc * (1 + barrel * |cc|^2), cc in
    // normalised [0,1] coords about the tube centre (0.5, 0.5). Symmetric in y, so the
    // shader's y-up convention doesn't matter here.
    const float w = juce::jmax (1.0f, (float) getWidth());
    const float h = juce::jmax (1.0f, (float) getHeight());
    const float barrel = PostFXPipeline::Params {}.barrel;
    const float cx = p.x / w - 0.5f, cy = p.y / h - 0.5f;
    const float k  = 1.0f + barrel * (cx * cx + cy * cy);
    // The curve maps the window's own corners slightly outside the image; clamp so a
    // pointer slammed into a screen corner (fullscreen) still hits the corner controls.
    return { juce::jlimit (0.0f, w - 0.5f, (0.5f + cx * k) * w),
             juce::jlimit (0.0f, h - 0.5f, (0.5f + cy * k) * h) };
}

void MainComponent::mouseMove (const juce::MouseEvent& e)
{
    const auto L = ScreenLayout::compute ((float) getWidth(), (float) getHeight(),
                                          TerrainModel::gridSize, glyphAtlas.getAdvanceFrac());
    const auto p = toScenePoint (e.position);
    const bool overControl = L.fullscreenHotspot().contains (p) || L.closeHotspot().contains (p);
    setMouseCursor (overControl ? juce::MouseCursor::PointingHandCursor
                                : juce::MouseCursor::NormalCursor);
}

void MainComponent::mouseDown (const juce::MouseEvent& e)
{
    // Dragging the header moves the borderless window (there's no OS title bar). Not the
    // [+]/[-] control (that's a click), and never while fullscreen.
    const auto L = ScreenLayout::compute ((float) getWidth(), (float) getHeight(),
                                          TerrainModel::gridSize, glyphAtlas.getAdvanceFrac());
    const auto p = toScenePoint (e.position);
    draggingWindow = ! isFullscreen()
                     && L.titleBar.contains (p)
                     && ! L.fullscreenHotspot().contains (p)
                     && ! L.closeHotspot().contains (p);
    if (draggingWindow)
        windowDragger.startDraggingComponent (getTopLevelComponent(), e);
}

void MainComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingWindow)
        windowDragger.dragComponent (getTopLevelComponent(), e, nullptr);
}

void MainComponent::mouseUp (const juce::MouseEvent& e)
{
    if (draggingWindow) { draggingWindow = false; return; }   // a move, not a click

    const auto L = ScreenLayout::compute ((float) getWidth(), (float) getHeight(),
                                          TerrainModel::gridSize, glyphAtlas.getAdvanceFrac());
    const auto p = toScenePoint (e.position);   // hit-test where things are actually drawn

    // Header window controls ([+]/[-] and [x]) — present in every UI mode.
    if (L.fullscreenHotspot().contains (p)) { toggleFullscreen(); return; }
    if (L.closeHotspot().contains (p))
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
        return;
    }

    // Narrow (stacked) mode: the clickable controls live on the command line below the
    // map. Wide mode has no command line — its controls are the right-band panel below.
    if (! L.sidePanels)
    {
        if      (L.seedHotspot()       .contains (p)) reseed();
        else if (L.lockHotspot()       .contains (p)) hueLocked = ! hueLocked;
        else if (L.resetHotspot()      .contains (p)) requestReset();
        else if (L.persistDownHotspot().contains (p)) stepPersist (-0.005f);
        else if (L.persistUpHotspot()  .contains (p)) stepPersist (+0.005f);
        else if (L.bloomDownHotspot()  .contains (p)) stepBloom (-0.05f);
        else if (L.bloomUpHotspot()    .contains (p)) stepBloom (+0.05f);
    }
    else if (L.ctrlValid)   // right-band control panel (wide windows)
    {
        if      (L.ctrlAction[ScreenLayout::ctReseed].contains (p)) reseed();
        else if (L.ctrlAction[ScreenLayout::ctLock]  .contains (p)) hueLocked = ! hueLocked;
        else if (L.ctrlDn[ScreenLayout::ctTheme]    .contains (p)) cycleTheme (-1);
        else if (L.ctrlUp[ScreenLayout::ctTheme]    .contains (p)) cycleTheme (+1);
        else if (L.ctrlDn[ScreenLayout::ctInput]    .contains (p)) cycleInput (-1);
        else if (L.ctrlUp[ScreenLayout::ctInput]    .contains (p)) cycleInput (+1);
        else if (L.ctrlAction[ScreenLayout::ctReset] .contains (p)) requestReset();
        else if (L.ctrlAction[ScreenLayout::ctAutoGain].contains (p)) autoGainOn = ! autoGainOn;
        else                                                        // value sliders
        {
            for (int i = ScreenLayout::ctFirstSlider; i < ScreenLayout::numCtrl; ++i)
            {
                if (i == ScreenLayout::ctGain && autoGainOn.load())   // dimmed: auto-gain drives it
                    continue;
                if      (L.ctrlDn[i].contains (p)) { stepKnob (i, -1.0f); break; }
                else if (L.ctrlUp[i].contains (p)) { stepKnob (i, +1.0f); break; }
                else if (L.ctrlBar[i].contains (p))   // click anywhere on the bar to set
                {
                    setKnobFrac (i, (p.x - L.ctrlBar[i].getX())
                                        / juce::jmax (1.0f, L.ctrlBar[i].getWidth()));
                    break;
                }
            }
        }
    }
}

//==============================================================================
void MainComponent::publishSnapshot()
{
    // 80s stepped refresh (see terrainStepHz): the map state below only advances on
    // step frames. terrainStaticsDirty (reseed) also waits for a step so a new base
    // field never pairs with the previous seed's peak amplitudes.
    terrainStepAcc += terrainStepHz / 60.0f;
    const bool terrainStep = terrainStepAcc >= 1.0f;
    if (terrainStep)
        terrainStepAcc -= 1.0f;

    const juce::SpinLock::ScopedLockType sl (snapshotLock);

    if (terrainStep)
    {
        shared.heightFieldMax = terrain.getHeightFieldMax();
        shared.riverPts       = river.getSwayedPoints();
        shared.riverWidths    = river.getBankWidths();
        shared.riverPhase     = river.getPhase();
        shared.riverFlash     = river.getFlash();
    }
    shared.feats          = audio.getFeatures();
    shared.animTime       = animTime;
    shared.currentHue     = currentHue - std::floor (currentHue);   // may sit outside 0..1 mid-switch
    shared.themeIndex     = themeIndex;
    shared.crtRoll        = rollEnv;
    shared.crtDegauss     = degaussEnv;
    shared.liveSat        = liveSat;
    shared.liveCaLo       = liveCaLo;
    shared.liveCaHi       = liveCaHi;
    shared.liveConv       = liveConv;
    shared.liveHal        = liveHal;
    shared.liveElevSpread = liveElevSpread;
    shared.liveElevFill   = liveElevFill;
    shared.liveBg         = liveBg;
    shared.liveDecayBias  = liveDecayBias;
    shared.liveRampMix    = liveRampMix;
    shared.liveRampGamma  = liveRampGamma;
    for (int i = 0; i < 5; ++i) shared.liveRamp[i] = liveRamp[i];
    shared.liveLowDim     = liveLowDim;
    shared.livePhosphor   = livePhosphor;
    shared.liveGrain      = liveGrain;
    shared.seed           = terrain.getSeed();
    shared.logicalW       = getWidth();
    shared.logicalH       = getHeight();
    shared.sampleRate     = (int) audio.getSampleRate();

    if (! terrainStep)
        return;

    // The terrain composes on the GPU: publish the compose-pass inputs.
    shared.peakAmps   = terrain.getPeakAmps();
    shared.minDist2   = river.getMinDist2();
    shared.dynDepth   = river.getDynDepth();
    shared.riverSigma = river.getSigma();

    shared.contourLabels = labelPlanner.getContourLabels();
    shared.spotLabels    = labelPlanner.getSpotLabels();
    shared.clutter       = labelPlanner.getClutter();

    // Stage dirty upload payloads. Bits accumulate in sharedUploads until the GL
    // thread consumes them, so a missed GL frame never drops an upload.
    {
        const juce::uint32 newBits = terrain.fetchAndClearDirtyPlaneBits();
        constexpr int N = TerrainModel::gridSize * TerrainModel::gridSize;
        for (int pk = 0; pk < TerrainModel::numPeaks; ++pk)
            if ((newBits & (1u << pk)) != 0)
            {
                const float* plane = terrain.getPeakPlane (pk);
                sharedUploads.planes[(size_t) pk].assign (plane, plane + N);
            }
        sharedUploads.planeDirtyBits |= newBits;

        if (terrainStaticsDirty)
        {
            terrainStaticsDirty = false;
            sharedUploads.baseField = terrain.getBaseHeightField();
            sharedUploads.peakMask  = river.getPeakMask();
            sharedUploads.baseDirty = true;
        }
    }
}

//==============================================================================
void MainComponent::newOpenGLContextCreated()
{
    postFX.initialise (glContext);
    sceneRenderer.initialise (glContext);
}

void MainComponent::openGLContextClosing()
{
    sceneRenderer.shutdown();
    postFX.shutdown();
}

void MainComponent::renderOpenGL()
{
    {
        const juce::SpinLock::ScopedLockType sl (snapshotLock);
        glLocal = shared;

        // Move dirty upload payloads to the GL side. Swaps keep both sides
        // allocation-free in steady state; |= preserves bits this frame can't
        // consume (e.g. compose skipped before the first river update).
        const juce::uint32 newBits = sharedUploads.planeDirtyBits;
        sharedUploads.planeDirtyBits = 0;
        glUploads.planeDirtyBits |= newBits;
        for (int pk = 0; pk < TerrainModel::numPeaks; ++pk)
            if ((newBits & (1u << pk)) != 0)
                std::swap (glUploads.planes[(size_t) pk], sharedUploads.planes[(size_t) pk]);
        if (sharedUploads.baseDirty)
        {
            sharedUploads.baseDirty = false;
            glUploads.baseDirty = true;
            std::swap (glUploads.baseField, sharedUploads.baseField);
            std::swap (glUploads.peakMask,  sharedUploads.peakMask);
        }
    }

    const float rs  = (float) glContext.getRenderingScale();
    const int   wPx = juce::roundToInt (rs * (float) glLocal.logicalW);
    const int   hPx = juce::roundToInt (rs * (float) glLocal.logicalH);
    if (wPx <= 0 || hPx <= 0)
        return;

    // --- scene passes into the scene FBO (all GPU) ---
    auto& fbo = postFX.getSceneBuffer (glContext, wPx, hPx);
    const auto sceneT0 = juce::Time::getHighResolutionTicks();
    constexpr int gridN = TerrainModel::gridSize * TerrainModel::gridSize;

    // One layout for the whole GL frame — the river/label builders and the map-box mapping
    // below all share it (formerly each recomputed the same pure result 5+ times per frame).
    const auto sl = ScreenLayout::compute ((float) glLocal.logicalW,
                                           (float) glLocal.logicalH,
                                           TerrainModel::gridSize,
                                           glyphAtlas.getAdvanceFrac());

    {
        const RiverGeometry rg = buildRiverGeometry (glLocal, sl);
        const SceneRenderer::RibbonBatch batch = buildRiverRibbons (glLocal, rg, sl, rs);

        SceneRenderer::Frame f;
        f.gridSize       = TerrainModel::gridSize;
        f.heightFieldMax = glLocal.heightFieldMax;
        f.hue            = glLocal.currentHue;
        f.saturation     = glLocal.liveSat;
        f.caLowHue       = glLocal.liveCaLo;
        f.caHighHue      = glLocal.liveCaHi;
        f.elevHueSpread  = glLocal.liveElevSpread;
        f.elevFill       = glLocal.liveElevFill;
        f.bgTint         = glLocal.liveBg;
        f.rampMix        = glLocal.liveRampMix;
        f.rampGamma      = glLocal.liveRampGamma;
        for (int i = 0; i < 5; ++i) f.ramp[i] = glLocal.liveRamp[i];
        f.lowDim         = glLocal.liveLowDim;
        f.shadeStrength  = shadeVal.load();
        f.slopeK         = slopeVal.load();
        f.hatchStrength  = hatchVal.load();
        f.stippleStrength = stippleVal.load();
        f.bassEnergy     = glLocal.feats.bassEnergy;
        f.overallEnergy  = glLocal.feats.overallEnergy;
        f.centroidNorm   = glLocal.feats.centroidNorm;
        f.animTime       = glLocal.animTime;
        f.logicalW      = (float) glLocal.logicalW;
        f.logicalH       = (float) glLocal.logicalH;
        f.renderScale    = rs;
        f.hasRiver       = rg.nPts >= 2;

        f.mapScale = sl.mapScale;
        f.mapOffX  = sl.mapOffX;
        f.mapOffY  = sl.mapOffY;

        // Compose base + peaks + river depression on the GPU.
        if ((int) glLocal.minDist2.size() >= gridN)
        {
            SceneRenderer::ComposeInputs ci;
            ci.gridSize      = TerrainModel::gridSize;
            ci.numPeaks      = TerrainModel::numPeaks;
            ci.baseDirty     = glUploads.baseDirty;
            ci.base          = (int) glUploads.baseField.size() >= gridN ? glUploads.baseField.data() : nullptr;
            ci.peakMask      = (int) glUploads.peakMask.size()  >= gridN ? glUploads.peakMask.data()  : nullptr;
            ci.planeDirtyBits = glUploads.planeDirtyBits;
            ci.planes        = glUploads.planes.data();
            ci.peakAmps      = glLocal.peakAmps.data();
            ci.minDist2      = glLocal.minDist2.data();
            ci.dynDepth      = glLocal.dynDepth;
            ci.sigma         = glLocal.riverSigma;
            sceneRenderer.composeHeightField (ci);
            glUploads.baseDirty      = false;
            glUploads.planeDirtyBits = 0;
        }

        sceneRenderer.uploadRibbons (batch);

        // River-interior mask: the isoline shader multiplies contours by (1 - mask),
        // replacing the old even-odd path clip. Rendered from the same fill strip
        // the river drawing uses, so the clip edge and the banks always agree.
        auto& mask = sceneRenderer.getMaskBuffer (glContext, wPx, hPx);
        sceneRenderer.renderRiverMask (f, batch, mask);

        // Labels: quads + contour-gap capsules both derive from f, so the punched
        // gaps and the drawn digits always agree. Gaps must be set before renderScene.
        const SceneRenderer::GlyphBatch glyphs = buildLabelGlyphs (glLocal, sl, f);

        fbo.clear (juce::Colours::black);   // covers the pre-first-compose frame
        sceneRenderer.renderScene (f, fbo, mask);
        sceneRenderer.drawRibbons (f, batch, fbo);
        sceneRenderer.drawGlyphs (f, glyphs, fbo);

        // Terminal screen UI (header, telemetry, command line, map chrome):
        // drawn into the scene FBO pre-post-FX so the text persists, blooms and
        // curves with the tube like everything else on the screen.
        {
            const auto& slUI = sl;   // same layout as the scene builders above
            ScreenTextBuilder::Inputs in;
            in.feats      = glLocal.feats;
            in.hue        = glLocal.currentHue;
            in.animTime   = glLocal.animTime;
            in.seed       = glLocal.seed;
            in.sampleRate = glLocal.sampleRate;
            in.sceneMs    = sceneMsAvg.load();
            in.updMs      = updateMsAvg.load();
            in.decay      = effDecay.load();
            in.bloom      = effBloom.load();
            in.hueLocked  = hueLocked.load();
            in.themeName  = Themes::get (glLocal.themeIndex).name;
            in.themeIndex = glLocal.themeIndex;
            in.themeCount = Themes::numThemes;
            in.inputName  = audio.getCurrentInputName();
            audio.getCurrentInputPosition (in.inputIndex, in.inputCount);
            in.sourceDenied = audio.isAccessDenied();
            in.saturation = glLocal.liveSat;
            switch (audio.getCaptureState())   // atomic load — safe from the GL thread
            {
                case AudioAnalyzer::CaptureState::Running:      in.audioStatus = "LIVE";         break;
                case AudioAnalyzer::CaptureState::Reconnecting: in.audioStatus = "RECONNECTING"; break;
                case AudioAnalyzer::CaptureState::Starting:     in.audioStatus = "STARTING";     break;
                case AudioAnalyzer::CaptureState::Failed:       in.audioStatus = "NO SIGNAL";    break;
            }
            if (in.sourceDenied) in.audioStatus = "MIC DENIED";   // retrying, but only the user can fix it
            in.shaderError = sceneRenderer.getLastShaderError().isNotEmpty();
            in.fullscreen  = fullscreenFlag.load();   // published by the message thread
            in.resetArmed  = juce::Time::getMillisecondCounter() < resetArmedUntil.load();
            in.autoGain    = autoGainOn.load();
            screenText.persistShown = persistTrimVal.load();
            screenText.bloomShown   = bloomTrimVal.load();
            screenText.threshShown  = bloomThreshVal.load();
            screenText.scanShown    = scanSpacingVal.load();
            screenText.crtShown     = crtTrimVal.load();
            screenText.shadeShown   = shadeVal.load();
            screenText.hatchShown   = hatchVal.load();
            screenText.stippleShown = stippleVal.load();
            screenText.slopeShown   = slopeVal.load();
            screenText.gainShown    = gainVal.load();

            uiGlyphs.clear();
            screenText.build (uiGlyphs, slUI, in);
            sceneRenderer.drawGlyphs (f, uiGlyphs, fbo, false);
        }
    }

    // Rolling scene-section timing for the HUD (CPU cost only — GL calls are async)
    sceneMsAccum += juce::Time::highResolutionTicksToSeconds (
                        juce::Time::getHighResolutionTicks() - sceneT0) * 1000.0;
    if (++sceneMsCount >= 60)
    {
        sceneMsAvg = (float) (sceneMsAccum / (double) sceneMsCount);
        sceneMsAccum = 0.0;
        sceneMsCount = 0;
    }

    if (screenshotRequested.exchange (false))
        saveSceneScreenshot (fbo);

    // --- post passes: persistence -> bloom -> composite ---
    const PostFXPipeline::Params p = mapPostFXParams (glLocal, rs, wPx, hPx);

    postFX.process (glContext, p, wPx, hPx);
}

//==============================================================================
// Snapshot + control-panel trims -> post-FX parameters. Extracted from renderOpenGL to
// keep that method an orchestrator; behaviour is unchanged. GL thread.
PostFXPipeline::Params MainComponent::mapPostFXParams (const RenderSnapshot& snap, float rs,
                                                       int wPx, int hPx)
{
    PostFXPipeline::Params p;
    const float energy = snap.feats.overallEnergy;
    p.time           = snap.animTime;
    p.phosphorTint   = snap.livePhosphor;
    p.grainTint      = snap.liveGrain;
    // Master CRT-FX slider scales convergence + halation; per-theme multipliers and
    // the halo tint ride the crossfaded live-theme values.
    const float crt  = crtTrimVal.load();
    p.convergencePx    = 4.0f * rs * crt * snap.liveConv;   // ~2px edge split at 0.5, per-theme scaled
    p.halationIntensity = 0.55f * crt * snap.liveHal;
    p.halationTint     = snap.livePhosphor;
    // Beat-reactive effects, also under the master CRT-FX slider. Kept subtle.
    p.rollOffset       = snap.crtRoll * 0.025f * crt;     // gentle vertical-hold nudge
    p.degauss          = snap.crtDegauss * crt;
    p.retrace          = 0.010f * crt;                       // peak warp of the slow sweep band
    // PERSIST trim shifts the audio-driven decay; BLOOM trim scales intensity.
    // Decay is specified per 60 Hz frame and converted to this GL frame's real delta, so
    // the trail length in SECONDS no longer depends on the monitor's refresh rate. Short
    // persistence (was 0.84 base, green +0.04) keeps moving thin contours crisp — an 80s
    // vector-display look rather than a long smeared afterglow.
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const float  glDt  = (lastGLFrameMs > 0.0)
                       ? juce::jlimit (1.0f / 240.0f, 1.0f / 20.0f, (float) ((nowMs - lastGLFrameMs) * 0.001))
                       : 1.0f / 60.0f;
    lastGLFrameMs = nowMs;
    const float decay60 = juce::jlimit (0.0f, 0.985f,
                                        juce::jlimit (0.0f, 0.970f, 0.72f + 0.06f * energy)
                                        + persistTrimVal.load());
    // Per-theme channel bias (P31 keeps its old +0.03 green; P7 dumps blue fast).
    auto chanDecay = [&] (float bias) { return std::pow (juce::jlimit (0.0f, 0.985f, decay60 + bias), glDt * 60.0f); };
    p.decayR         = chanDecay (snap.liveDecayBias.r);
    p.decayG         = chanDecay (snap.liveDecayBias.g);
    p.decayB         = chanDecay (snap.liveDecayBias.b);
    p.bloomThreshold = bloomThreshVal.load();    // only bright cores bloom; fine contours stay sharp
    p.bloomIntensity = (0.45f + 0.65f * energy) * bloomTrimVal.load();
    effDecay.store (decay60);                    // telemetry shows the per-60 Hz value
    effBloom.store (p.bloomIntensity);
    p.energy         = energy;                                                // excites the phosphor grain
    p.jitterGate     = 0.9875f - snap.feats.bassEnergy * 0.01f;            // bass = more line tears
    p.sparkAmount    = juce::jlimit (0.0f, 0.04f,                             // louder/bassier = more discharges
                                     0.006f + energy * 0.018f + snap.feats.bassEnergy * 0.016f);
    p.driftAmpPx     = 0.6f * rs;
    p.jitterAmpPx    = 4.0f * rs;
    p.scanSpacingPx  = scanSpacingVal.load() * rs;

    // The whole window is the tube now: barrel + vignette pivot on the window
    // centre. The vignette radius overshoots the screen diagonal and starts
    // late so the header/command text at the edges stays readable.
    p.tubeCentreU   = 0.5f;
    p.tubeCentreV   = 0.5f;
    p.boxHalfDiagPx = 0.575f * std::hypot ((float) wPx, (float) hPx);
    p.vigStart      = 0.45f;

    return p;
}

//==============================================================================
// River backbone -> screen-space geometry (jittered centre, bank outlines).
// Runs on the GL thread from snapshot data only.
MainComponent::RiverGeometry MainComponent::buildRiverGeometry (const RenderSnapshot& s,
                                                                const ScreenLayout& sl) const
{
    RiverGeometry rg;

    const int nPts = (int) s.riverPts.size();
    if (nPts < 2)
        return rg;
    rg.nPts = nPts;

    const float scale   = sl.mapScale;
    const float offsetX = sl.mapOffX;
    const float offsetY = sl.mapOffY;

    const auto&  riverSwayedPts  = s.riverPts;
    const auto&  riverBankWidths = s.riverWidths;
    const float  sceneTime       = s.animTime;
    const float  avgEnergy       = s.feats.overallEnergy;
    const uint32_t terrainSeed   = s.seed;
    auto valueNoiseSample = [terrainSeed] (float x, float y) { return Noise::valueNoiseSample (terrainSeed, x, y); };

    // Average bank half-width drives the thalweg glow stroke.
    float avgBankHalfW = 4.0f + s.feats.bassEnergy * 14.0f;
    if ((int) riverBankWidths.size() == nPts)
    {
        float sum = 0.0f;
        for (float w : riverBankWidths) sum += w;
        avgBankHalfW = sum / (float) nPts;
    }
    rg.avgBankHalfW = avgBankHalfW;

    auto& cPts = rg.cPts;
    auto& lPts = rg.lPts;
    auto& rPts = rg.rPts;
    cPts.resize (nPts);  lPts.resize (nPts);  rPts.resize (nPts);

    for (int i = 0; i < nPts; ++i)
        cPts[i] = { riverSwayedPts[i].x * scale + offsetX,
                    riverSwayedPts[i].y * scale + offsetY };

    // Per-point jitter: makes the river edge vibrate like marching-squares contours
    {
        const float jitterAmp = 1.2f + avgEnergy * 2.8f;
        for (int i = 0; i < nPts; ++i)
        {
            const float dx = valueNoiseSample (i * 0.35f,         sceneTime * 1.8f) * 2.0f - 1.0f;
            const float dy = valueNoiseSample (i * 0.35f + 97.0f, sceneTime * 1.8f) * 2.0f - 1.0f;
            cPts[i].x += dx * jitterAmp;
            cPts[i].y += dy * jitterAmp;
        }
    }

    for (int i = 0; i < nPts; ++i)
    {
        const int i0 = juce::jmax (0, i - 1);
        const int i1 = juce::jmin (nPts - 1, i + 1);
        float tx = cPts[i1].x - cPts[i0].x;
        float ty = cPts[i1].y - cPts[i0].y;
        const float tlen = std::hypot (tx, ty);
        if (tlen > 1e-5f) { tx /= tlen; ty /= tlen; }
        const float w = ((int) riverBankWidths.size() == nPts) ? riverBankWidths[i] : avgBankHalfW;
        lPts[i] = { cPts[i].x - ty * w, cPts[i].y + tx * w };
        rPts[i] = { cPts[i].x + ty * w, cPts[i].y - tx * w };
    }

    return rg;
}

//==============================================================================
// GPU river + glitch geometry: every stroke the old JUCE Graphics overlay drew,
// expressed as feathered triangle-strip ribbons in logical coords, preserving
// the original stroke widths, colours, and noise-driven jitter exactly.
SceneRenderer::RibbonBatch MainComponent::buildRiverRibbons (const RenderSnapshot& s,
                                                             const RiverGeometry& rg,
                                                             const ScreenLayout& sl,
                                                             float renderScale) const
{
    SceneRenderer::RibbonBatch b;

    // Glitch bands span the map box (the interference lives in the plot signal).
    const float boxSide = sl.mapScale * (float) (TerrainModel::gridSize - 1);
    const float sceneTime = s.animTime;
    const float avgEnergy = s.feats.overallEnergy;
    const uint32_t terrainSeed = s.seed;
    auto valueNoiseSample = [terrainSeed] (float x, float y) { return Noise::valueNoiseSample (terrainSeed, x, y); };

    const juce::Colour kNeon = juce::Colour::fromHSV (s.currentHue, 1.0f * s.liveSat, 1.0f, 1.0f);

    auto pushV = [&b] (float x, float y, float across, float halfWPx)
    {
        b.verts.push_back ({ x, y, across, halfWPx });
    };

    // Stroked polyline -> extruded strip, butt caps, central-difference normals
    // (same tangent scheme the bank extrusion in buildRiverGeometry uses).
    auto addStroke = [&] (const std::vector<juce::Point<float>>& poly,
                          float strokeW, juce::Colour col)
    {
        const int n = (int) poly.size();
        if (n < 2)
            return;
        const float halfW = strokeW * 0.5f;
        const int first = (int) b.verts.size();
        for (int i = 0; i < n; ++i)
        {
            const int i0 = juce::jmax (0, i - 1);
            const int i1 = juce::jmin (n - 1, i + 1);
            float tx = poly[i1].x - poly[i0].x;
            float ty = poly[i1].y - poly[i0].y;
            const float tl = std::hypot (tx, ty);
            if (tl > 1e-5f) { tx /= tl; ty /= tl; }
            pushV (poly[i].x - ty * halfW, poly[i].y + tx * halfW, -1.0f, halfW * renderScale);
            pushV (poly[i].x + ty * halfW, poly[i].y - tx * halfW, +1.0f, halfW * renderScale);
        }
        b.draws.push_back ({ first, (int) b.verts.size() - first, col });
    };

    // Flattens the quadratic-midpoint smoothing used by buildSmooth / the river
    // polygon (3 line segments per curve span).
    auto flattenSmooth = [] (const std::vector<juce::Point<float>>& pts)
    {
        std::vector<juce::Point<float>> out;
        const int n = (int) pts.size();
        out.reserve ((size_t) n * 3 + 2);
        out.push_back (pts[0]);
        juce::Point<float> cur = pts[0];
        for (int i = 1; i < n - 1; ++i)
        {
            const juce::Point<float> c = pts[i];
            const juce::Point<float> e = { (pts[i].x + pts[i + 1].x) * 0.5f,
                                           (pts[i].y + pts[i + 1].y) * 0.5f };
            for (int k = 1; k <= 3; ++k)
            {
                const float t = (float) k / 3.0f, u = 1.0f - t;
                out.push_back ({ u * u * cur.x + 2.0f * u * t * c.x + t * t * e.x,
                                 u * u * cur.y + 2.0f * u * t * c.y + t * t * e.y });
            }
            cur = e;
        }
        out.push_back (pts[n - 1]);
        return out;
    };

    if (rg.nPts >= 2)
    {
        const int   nPts       = rg.nPts;
        const auto& cPts       = rg.cPts;
        const auto& lPts       = rg.lPts;
        const auto& rPts       = rg.rPts;
        const float riverPhase = s.riverPhase;
        const float riverFlash = s.riverFlash;
        const juce::Colour kGreen = kNeon;

        const auto flatC = flattenSmooth (cPts);
        const auto flatL = flattenSmooth (lPts);
        const auto flatR = flattenSmooth (rPts);

        // Fill strip: zip of the two smoothed bank outlines = the river polygon.
        {
            const int n = (int) juce::jmin (flatL.size(), flatR.size());
            b.fillFirst = (int) b.verts.size();
            for (int i = 0; i < n; ++i)
            {
                const float halfW = flatL[i].getDistanceFrom (flatR[i]) * 0.5f * renderScale;
                pushV (flatL[i].x, flatL[i].y, -1.0f, halfW);
                pushV (flatR[i].x, flatR[i].y, +1.0f, halfW);
            }
            b.fillCount = (int) b.verts.size() - b.fillFirst;

            // Semi-transparent fill so terrain bleeds through, then the hue tint.
            b.draws.push_back ({ b.fillFirst, b.fillCount, juce::Colour (0x99000000) });
            b.draws.push_back ({ b.fillFirst, b.fillCount, kGreen.withAlpha (0.05f) });
        }

        // Interior depth ripples: 5 thin neon polylines parallel to flow. (The CPU
        // path clipped these to the polygon; jitter can poke them ~1px past the
        // bank at high energy, invisible at these alphas.)
        {
            constexpr float kFracs[] = { -0.60f, -0.28f, 0.0f, 0.28f, 0.60f };
            std::vector<juce::Point<float>> ripple ((size_t) nPts);
            for (float frac : kFracs)
            {
                for (int i = 0; i < nPts; ++i)
                {
                    const float jn = valueNoiseSample (i * 0.32f + frac * 11.3f,
                                                       sceneTime * 1.6f + riverPhase) * 2.0f - 1.0f;
                    const float ja = (1.0f + avgEnergy * 2.0f) * 0.9f;
                    const juce::Point<float>& bk = (frac < 0.0f) ? lPts[i] : rPts[i];
                    const float t = std::abs (frac);
                    ripple[(size_t) i] = { cPts[i].x + (bk.x - cPts[i].x) * t + jn * ja,
                                           cPts[i].y + (bk.y - cPts[i].y) * t + jn * ja * 0.5f };
                }
                const float a = (frac == 0.0f ? 0.22f : 0.09f) + avgEnergy * 0.18f;
                addStroke (ripple, 0.6f, kGreen.withAlpha (juce::jlimit (0.0f, 0.6f, a)));
            }
        }

        // Thalweg glow along the flow axis
        const float flicker = valueNoiseSample (sceneTime * 9.0f, 73.1f) * 0.06f;
        addStroke (flatC, rg.avgBankHalfW * 0.65f,
                   kGreen.withAlpha (juce::jlimit (0.0f, 0.50f,
                       0.07f + avgEnergy * 0.14f + riverFlash * 0.22f + flicker)));

        // Bank borders: soft outer glow + sharp bright core
        const juce::Colour glowCol = kGreen.withAlpha (juce::jlimit (0.0f, 0.60f,
            0.38f + avgEnergy * 0.15f + riverFlash * 0.10f));
        const juce::Colour coreCol = kGreen.withAlpha (juce::jlimit (0.0f, 1.0f,
            0.82f + avgEnergy * 0.14f + riverFlash * 0.08f));
        addStroke (flatL, 5.0f, glowCol);
        addStroke (flatR, 5.0f, glowCol);
        addStroke (flatL, 1.5f, coreCol);
        addStroke (flatR, 1.5f, coreCol);

        // Chromatic aberration on the bank lines
        auto shifted = [] (std::vector<juce::Point<float>> p, float dx)
        {
            for (auto& q : p) q.x += dx;
            return p;
        };
        const juce::Colour redC  = juce::Colour::fromHSV (s.liveCaLo, 0.7f * s.liveSat, 1.0f, 0.07f);
        const juce::Colour blueC = juce::Colour::fromHSV (s.liveCaHi, 0.7f * s.liveSat, 1.0f, 0.07f);
        addStroke (shifted (flatL,  1.5f), 1.2f, redC);
        addStroke (shifted (flatR,  1.5f), 1.2f, redC);
        addStroke (shifted (flatL, -1.5f), 1.2f, blueC);
        addStroke (shifted (flatR, -1.5f), 1.2f, blueC);
    }

    // === Electronic interference — drawn last, over the river, like the CPU path ===
    auto addRect = [&] (float x, float y, float w, float h, juce::Colour col)
    {
        const int first = (int) b.verts.size();
        pushV (x,     y,     0.0f, 1000.0f);
        pushV (x,     y + h, 0.0f, 1000.0f);
        pushV (x + w, y,     0.0f, 1000.0f);
        pushV (x + w, y + h, 0.0f, 1000.0f);
        b.draws.push_back ({ first, 4, col });
    };
    for (int band = 0; band < 4; ++band)
    {
        const float trigger = valueNoiseSample ((float) band * 13.7f,
                                                sceneTime * 3.1f + (float) band * 6.3f);
        const float thresh  = 0.94f - avgEnergy * 0.08f;
        if (trigger > thresh)
        {
            const float by = sl.mapOffY
                           + valueNoiseSample ((float) band * 29.1f,
                                               std::floor (sceneTime * 0.8f) + (float) band)
                           * boxSide;
            const float bh = 1.0f + valueNoiseSample ((float) band * 7.3f, sceneTime * 1.4f) * 3.0f;
            const float ba = juce::jlimit (0.0f, 0.08f,
                                           (trigger - thresh) * 1.2f + avgEnergy * 0.04f);
            addRect (sl.mapOffX, by, boxSide, bh, kNeon.withAlpha (ba));
        }
    }

    return b;
}

//==============================================================================
// Snapshot label records -> textured glyph quads + contour-gap capsules. Runs on
// the GL thread from snapshot data only.
SceneRenderer::GlyphBatch MainComponent::buildLabelGlyphs (const RenderSnapshot& s,
                                                           const ScreenLayout& sl,
                                                           SceneRenderer::Frame& f) const
{
    SceneRenderer::GlyphBatch b;
    f.gapCount = 0;

    const float scale = sl.mapScale;
    const float offX  = sl.mapOffX;
    const float offY  = sl.mapOffY;
    const float adv   = glyphAtlas.getAdvanceFrac();

    // One quad (2 triangles) per glyph, centred at (cx,cy) with baseline direction
    // (dx,dy) (unit). perp = (-dy,dx) points to the glyph's "down" (v increasing).
    auto pushGlyph = [&b] (float cx, float cy, float dx, float dy,
                           float hw, float hh, int glyphId, float alpha)
    {
        float u0, v0, u1, v1;
        GlyphAtlas::getUV (glyphId, u0, v0, u1, v1);
        const float px = -dy, py = dx;                       // perpendicular (down)
        const auto corner = [&] (float sx, float sy)
        {
            return juce::Point<float> (cx + dx * sx + px * sy, cy + dy * sx + py * sy);
        };
        const auto TL = corner (-hw, -hh), TR = corner (hw, -hh);
        const auto BR = corner (hw,  hh),  BL = corner (-hw, hh);
        auto v = [&] (juce::Point<float> p, float u, float w)
        { b.verts.push_back ({ p.x, p.y, u, w, alpha }); };
        v (TL, u0, v0); v (TR, u1, v0); v (BR, u1, v1);
        v (TL, u0, v0); v (BR, u1, v1); v (BL, u0, v1);
    };

    // --- contour elevation labels (rotated, riding the index contours) ---
    const int labelFirst = (int) b.verts.size();
    {
        constexpr float drawH = 15.0f;                       // glyph cell height, logical px
        const float hw = drawH * GlyphAtlas::cellAspect * 0.5f;
        const float hh = drawH * 0.5f;
        const float advPx = drawH * adv;

        for (const auto& lab : s.contourLabels)
        {
            if (lab.alpha < 0.02f) continue;
            const juce::String text (lab.level * 20);
            const int n = text.length();
            if (n == 0) continue;

            const float dx = std::cos (lab.angle), dy = std::sin (lab.angle);
            const float cxL = offX + lab.gx * scale;
            const float cyL = offY + lab.gy * scale;
            const float half = advPx * (float) (n - 1) * 0.5f;

            for (int i = 0; i < n; ++i)
            {
                const int id = GlyphAtlas::idForChar (text[i]);
                if (id < 0) continue;
                const float o = (float) i * advPx - half;
                pushGlyph (cxL + dx * o, cyL + dy * o, dx, dy, hw, hh, id, lab.alpha);
            }

            // Punch the contour line out under this label so the digits read cleanly.
            // Capsule endpoints are map-local logical px (= full logical minus the box
            // offset), which is the space the scene shader's gap loop works in. Gated on
            // a mostly-faded-in label so a gap never appears before its text does.
            if (lab.alpha > 0.5f && f.gapCount < SceneRenderer::Frame::maxGaps)
            {
                const float ext = half + hw;            // span the end glyphs, not just centres
                const float lcx = lab.gx * scale;       // == cxL - offX
                const float lcy = lab.gy * scale;       // == cyL - offY
                const int   g   = f.gapCount++;
                f.gapSeg[(size_t) (4 * g + 0)] = lcx - dx * ext;
                f.gapSeg[(size_t) (4 * g + 1)] = lcy - dy * ext;
                f.gapSeg[(size_t) (4 * g + 2)] = lcx + dx * ext;
                f.gapSeg[(size_t) (4 * g + 3)] = lcy + dy * ext;
                f.gapHalfW[(size_t) g] = hh;            // text box; shader pads the own-contour break
                f.gapLevel[(size_t) g] = (float) lab.level;
            }
        }
    }

    // --- spot elevations at peaks: triangle + number, axis-aligned ---
    {
        constexpr float drawH = 16.0f;
        const float hw = drawH * GlyphAtlas::cellAspect * 0.5f;
        const float hh = drawH * 0.5f;
        const float advPx = drawH * adv;

        for (const auto& sp : s.spotLabels)
        {
            if (sp.alpha < 0.02f) continue;
            const juce::String text (sp.meters);
            const int n = text.length();
            const float cxL = offX + sp.gx * scale;
            const float cyL = offY + sp.gy * scale;

            // triangle marker on the summit, number starting to its right
            pushGlyph (cxL, cyL, 1.0f, 0.0f, hw, hh, GlyphAtlas::GTri, sp.alpha);
            float x = cxL + advPx;
            for (int i = 0; i < n; ++i)
            {
                const int id = GlyphAtlas::idForChar (text[i]);
                if (id >= 0)
                    pushGlyph (x, cyL, 1.0f, 0.0f, hw, hh, id, sp.alpha);
                x += advPx;
            }
        }
    }
    const int labelCount = (int) b.verts.size() - labelFirst;
    if (labelCount >= 3)
        b.draws.push_back ({ labelFirst, labelCount,
                             juce::Colour::fromHSV (s.currentHue, 0.85f * s.liveSat, 0.98f, 1.0f) });

    // --- clutter marks: tiny symbols, faint audio twinkle (no gaps) ---
    const int clutFirst = (int) b.verts.size();
    {
        constexpr float drawH = 8.0f;
        const float hw = drawH * GlyphAtlas::cellAspect * 0.5f;
        const float hh = drawH * 0.5f;
        const float energy = s.feats.overallEnergy;
        for (const auto& m : s.clutter)
        {
            const float tw = 0.55f + 0.45f * std::sin (s.animTime * 1.7f + m.phase);
            const float a  = m.baseAlpha * tw * (1.0f + 0.8f * energy);
            pushGlyph (offX + m.gx * scale, offY + m.gy * scale,
                       1.0f, 0.0f, hw, hh, m.glyph, juce::jlimit (0.0f, 0.5f, a));
        }
    }
    const int clutCount = (int) b.verts.size() - clutFirst;
    if (clutCount >= 3)
        b.draws.push_back ({ clutFirst, clutCount,
                             juce::Colour::fromHSV (s.currentHue, 0.80f * s.liveSat, 0.62f, 1.0f) });

    return b;
}

//==============================================================================
// Debug: dump the pre-postFX scene FBO to a PNG for CPU/GPU pixel diffing.
void MainComponent::saveSceneScreenshot (juce::OpenGLFrameBuffer& fbo)
{
    const int w = fbo.getWidth(), h = fbo.getHeight();
    if (w <= 0 || h <= 0)
        return;

    juce::Image img (juce::Image::ARGB, w, h, false);
    {
        std::vector<juce::PixelARGB> px ((size_t) w * (size_t) h);
        if (! fbo.readPixels (px.data(), juce::Rectangle<int> (0, 0, w, h),
                              juce::OpenGLFrameBuffer::RowOrder::fromTopDown))
            return;
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < h; ++y)
            std::memcpy (bd.getLinePointer (y),
                         px.data() + (size_t) y * (size_t) w,
                         (size_t) w * sizeof (juce::PixelARGB));
    }

    // Pictures\Spectral Plotter — the exe's own folder may be read-only (Program Files).
    auto dir = juce::File::getSpecialLocation (juce::File::userPicturesDirectory)
                   .getChildFile ("Spectral Plotter");
    dir.createDirectory();
    auto file = dir.getChildFile ("scene_"
                    + juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S") + ".png")
                   .getNonexistentSibling();

    juce::FileOutputStream os (file);
    if (os.openedOk())
    {
        juce::PNGImageFormat png;
        png.writeImageToStream (img, os);
    }
}

//==============================================================================
void MainComponent::timerCallback()
{
    const auto updT0 = juce::Time::getHighResolutionTicks();

    // Route key presses (debug/shortcut keys) here; retries until the grab
    // sticks (it no-ops while the window isn't active, e.g. right after launch).
    if (! hasGrabbedFocus && isShowing())
    {
        grabKeyboardFocus();
        hasGrabbedFocus = hasKeyboardFocus (true);
    }

    audio.setAutoGain (autoGainOn.load());
    audio.setManualGainDb (gainVal.load());
    audio.update();

    const auto& feats = audio.getFeatures();

    constexpr float dt = 1.0f / 60.0f;

    // animTime is passed at its PRE-increment value: TerrainModel::update() uses it
    // for peak drift, then internally uses (animTime + dt) for the periodic weight
    // rebuild, mirroring the point in the frame where animTime used to be advanced.
    terrain.update (feats, river.getPeakMask(), animTime);

    // Advance animation clock (shape breathing)
    animTime += dt;

    // Update hue from bass vs. high-frequency energy (colour shifting).
    // Bass pulls warm (yellow = "hot"), highs pull cool (blue = "cold").
    // The bass coefficient is raised from the original 0.40 to compensate for the perceptual
    // A-weighting now applied to feats.bassEnergy (which cuts bass magnitude to ~0.6x the old
    // unweighted model); without this the warm/cool balance skews toward blue.
    if (! hueLocked.load())
    {
        const Themes::Theme& th = Themes::get (themeIndex);
        const float targetHue = juce::jlimit (th.hueMin, th.hueMax,
                                              th.baseHue - feats.bassEnergy * th.warmPush
                                                         + feats.highEnergy * th.coolPush);
        currentHue += (targetHue - currentHue) * 0.05f;  // ~0.4-second time constant
    }

    // Crossfade the rest of the palette (saturation, CA hues, phosphor/grain tints,
    // per-theme CRT multipliers) toward the active theme so a switch fades in ~0.3 s
    // instead of snapping. Hue is handled separately above.
    {
        const Themes::Theme& t = Themes::get (themeIndex);
        auto ease = [] (float& v, float target) { v += (target - v) * 0.06f; };  // ~0.3 s crossfade
        ease (liveSat,  t.saturation);
        ease (liveCaLo, t.caLowHue);
        ease (liveCaHi, t.caHighHue);
        ease (liveConv, t.convergence);
        ease (liveHal,  t.halation);
        ease (liveElevSpread, t.elevHueSpread);
        ease (liveElevFill,   t.elevFill);
        ease (liveRampMix,    t.rampMix);
        if (t.rampMix > 0.0f)   // only ramp themes steer the stops, so switching away never
        {                       // fades through a meaningless (all-zero) ramp
            ease (liveRampGamma, t.rampGamma);
            for (int i = 0; i < 5; ++i)
            {
                ease (liveRamp[i].r, t.ramp[i].r); ease (liveRamp[i].g, t.ramp[i].g); ease (liveRamp[i].b, t.ramp[i].b);
            }
        }
        ease (liveLowDim,     t.lowDim);
        ease (liveBg.r, t.bgTint.r); ease (liveBg.g, t.bgTint.g); ease (liveBg.b, t.bgTint.b);
        ease (liveDecayBias.r, t.decayBias.r); ease (liveDecayBias.g, t.decayBias.g); ease (liveDecayBias.b, t.decayBias.b);
        ease (livePhosphor.r, t.phosphorTint.r); ease (livePhosphor.g, t.phosphorTint.g); ease (livePhosphor.b, t.phosphorTint.b);
        ease (liveGrain.r,    t.grainTint.r);    ease (liveGrain.g,    t.grainTint.g);    ease (liveGrain.b,    t.grainTint.b);
    }

    // Bass onset detection -> beat-reactive CRT envelopes. A slow floor tracks the
    // baseline; a transient rising well above it (past a refractory gap) fires a
    // degauss ripple, and only the strongest hits slip the vertical hold. Both
    // envelopes decay each frame so the effects are brief punctuation, not constant.
    {
        const float bass = feats.bassEnergy;
        bassBaseline += (bass - bassBaseline) * 0.08f;          // ~0.2 s floor
        const float excess = bass - bassBaseline;

        onsetTimer = juce::jmax (0.0f, onsetTimer - dt);

        // A beat sets an impulse TARGET (which then decays); the live envelopes ease
        // toward the targets with a smooth attack, so nothing ever changes
        // instantaneously — the effect swells in and out, it never snaps.
        degaussTarget *= std::exp (-dt / 0.18f);
        rollTarget    *= std::exp (-dt / 0.30f);

        if (onsetTimer <= 0.0f && excess > 0.06f)               // a beat
        {
            onsetTimer = 0.11f;                                 // ~110 ms refractory
            degaussTarget = juce::jmax (degaussTarget, juce::jmin (0.45f, 0.18f + excess * 1.3f));
            if (excess > 0.16f)                                 // only strong hits nudge the hold
                rollTarget = juce::jmax (rollTarget, juce::jmin (0.45f, 0.15f + excess * 1.1f));
        }

        const float atkD = 1.0f - std::exp (-dt / 0.07f);       // ~70 ms rise
        const float atkR = 1.0f - std::exp (-dt / 0.10f);       // ~100 ms rise
        degaussEnv += (degaussTarget - degaussEnv) * atkD;
        rollEnv    += (rollTarget    - rollEnv)    * atkR;
    }

    // The river only derives its grid->screen scale from these dims, so pass the
    // square map-box side (not the window) to keep bank widths in map pixels.
    {
        const auto sl = ScreenLayout::compute ((float) getWidth(), (float) getHeight(),
                                               TerrainModel::gridSize,
                                               glyphAtlas.getAdvanceFrac());
        const int mapSide = juce::roundToInt (sl.mapBox.getWidth());
        river.update (feats,
                      [this] (float x, float y)
                      { return terrain.sampleComposedHeight (x, y, river.getPeakMask()); },
                      mapSide, mapSide);
    }

    // Map labels: track existing labels + spot summits EVERY frame so they stay
    // glued to the moving terrain (cheap: a few hundred height samples). Only the
    // expensive seeding scan for new contour labels is throttled to a few Hz.
    {
        const float hMax = terrain.getHeightFieldMax();
        labelPlanner.advanceFades (dt);
        labelPlanner.trackContourLabels (terrain, river, hMax);
        labelPlanner.updateSpotLabels (terrain, river, hMax);
        if (++plannerCounter >= 20)
        {
            plannerCounter = 0;
            labelPlanner.seedContourLabels (terrain, river, hMax);
        }
    }

    // Publish this frame's scene state for the free-running GL renderer. Do NOT call
    // repaint() every frame: with the GL context attached that would force each frame
    // to render under the MessageManager lock, starving the message thread. Instead the
    // scene free-runs (continuous repainting) and we only refresh the HUD/button layer a
    // few times a second — enough for the neon hue to track the music.
    publishSnapshot();

    // Rolling message-thread update cost for the HUD
    updateMsAccum += juce::Time::highResolutionTicksToSeconds (
                         juce::Time::getHighResolutionTicks() - updT0) * 1000.0;
    if (++updateMsCount >= 60)
    {
        updateMsAvg = (float) (updateMsAccum / (double) updateMsCount);
        updateMsAccum = 0.0;
        updateMsCount = 0;
    }

    // No repaint: component painting is disabled — every visible pixel comes
    // from the free-running GL thread.
}

//==============================================================================
// Settings persistence. Each knob is stored under a stable key and clamped back into its
// knobFor() range on load, so ranges can change between versions without breaking old files.
namespace
{
    struct KnobKey { int ctrl; const char* key; };
    constexpr KnobKey kKnobKeys[] = {
        { ScreenLayout::ctPersist, "persist" }, { ScreenLayout::ctBloom,   "bloom"   },
        { ScreenLayout::ctThresh,  "thresh"  }, { ScreenLayout::ctScan,    "scan"    },
        { ScreenLayout::ctCrt,     "crt"     }, { ScreenLayout::ctShade,   "shade"   },
        { ScreenLayout::ctHatch,   "hatch"   }, { ScreenLayout::ctStipple, "stipple" },
        { ScreenLayout::ctSlope,   "slope"   },
        { ScreenLayout::ctGain,    "gain"    },
    };
}

void MainComponent::loadSettings()
{
    auto& p = settings.props();

    for (const auto& kk : kKnobKeys)
    {
        const Knob k = knobFor (kk.ctrl);
        if (k.val != nullptr)
            k.val->store (settings.getFloat (kk.key, k.val->load(), k.lo, k.hi));
    }

    // Theme is saved by name (the table is ordered round the hue wheel and may be
    // reordered); 1.0.0 saved an index into its original 9-theme table instead.
    if (p.containsKey ("themeName"))
    {
        themeIndex = juce::jmax (0, Themes::findByName (p.getValue ("themeName").toRawUTF8()));
    }
    else if (p.containsKey ("theme"))
    {
        const int legacy = p.getIntValue ("theme");
        constexpr int numLegacy = (int) (sizeof (Themes::kLegacyNames) / sizeof (Themes::kLegacyNames[0]));
        themeIndex = (legacy >= 0 && legacy < numLegacy)
                   ? juce::jmax (0, Themes::findByName (Themes::kLegacyNames[legacy])) : 0;
    }
    hueLocked  = p.getBoolValue ("hueLocked", hueLocked.load());
    autoGainOn = p.getBoolValue ("autoGain", autoGainOn.load());
    audio.setPreferredDeviceId (p.getValue ("audioSource"));
}

void MainComponent::requestReset()
{
    const auto now = juce::Time::getMillisecondCounter();
    if (now < resetArmedUntil.load())
    {
        resetArmedUntil = 0;
        resetToDefaults();
    }
    else
    {
        resetArmedUntil = now + resetConfirmMs;
    }
}

// Restores the factory look: every slider, the palette, the hue lock and auto-gain. The audio source
// and window placement are left alone — they're environment, not style.
void MainComponent::resetToDefaults()
{
    for (int i = ScreenLayout::ctFirstSlider; i < ScreenLayout::numCtrl; ++i)
        if (const Knob k = knobFor (i); k.val != nullptr)
            k.val->store (knobDefaults[(size_t) i]);

    themeIndex = 0;
    hueLocked  = false;
    autoGainOn = true;
}

void MainComponent::saveSettings()
{
    auto& p = settings.props();

    for (const auto& kk : kKnobKeys)
    {
        const Knob k = knobFor (kk.ctrl);
        if (k.val != nullptr)
            p.setValue (kk.key, k.val->load());
    }

    p.setValue ("themeName",   juce::String (Themes::get (themeIndex).name));
    p.removeValue ("theme");                                  // legacy 1.0.0 index key
    p.setValue ("hueLocked",   hueLocked.load());
    p.setValue ("autoGain",    autoGainOn.load());
    p.setValue ("audioSource", audio.getCurrentDeviceId());

    if (auto* window = getTopLevelComponent())
    {
        const auto bounds = isFullscreen() ? windowedBounds : window->getBounds();
        if (! bounds.isEmpty())
            p.setValue ("windowBounds", bounds.toString());
        p.setValue ("fullscreen", isFullscreen());
    }

    settings.save();
}

void MainComponent::restoreWindowState()
{
    auto* window = getTopLevelComponent();
    if (window == nullptr)
        return;

    auto& p = settings.props();
    const auto saved = juce::Rectangle<int>::fromString (p.getValue ("windowBounds"));

    // Only restore a rectangle that still lands mostly on a connected display (monitor
    // layouts change); otherwise keep the default centred placement.
    if (! saved.isEmpty())
    {
        const auto* disp = juce::Desktop::getInstance().getDisplays().getDisplayForRect (saved);
        if (disp != nullptr)
        {
            const auto visible = disp->userBounds.toNearestInt().getIntersection (saved);
            if (visible.getWidth() >= saved.getWidth() / 2 && visible.getHeight() >= saved.getHeight() / 2)
                window->setBounds (saved);
        }
    }

    if (p.getBoolValue ("fullscreen") && ! isFullscreen())
        toggleFullscreen();
}
