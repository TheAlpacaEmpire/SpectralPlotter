#pragma once

#include <JuceHeader.h>
#include "../Audio/AudioAnalyzer.h"
#include "../Labels/GlyphAtlas.h"
#include "../PostFX/SceneRenderer.h"
#include "ScreenLayout.h"

// Builds the terminal screen — header bar, telemetry rows, spectrum strip,
// command line, and the map's chrome text — as one GlyphBatch per frame,
// drawn into the scene FBO before post-FX so every character gets phosphor
// persistence, bloom, barrel curvature and scanlines like the map itself.
//
// Runs on the GL thread from snapshot data (same pattern as buildLabelGlyphs).
class ScreenTextBuilder
{
public:
    ScreenTextBuilder() = default;

    struct Inputs
    {
        AudioFeatures feats;
        float hue        = 0.376f;
        float saturation = 1.0f;   // per-theme saturation multiplier (matches the map)
        float animTime = 0.0f;
        juce::uint32 seed = 0;
        int   sampleRate  = 0;
        float sceneMs = 0.0f, updMs = 0.0f;
        float decay = 0.0f, bloom = 0.0f;
        bool  hueLocked = false;
        const char* themeName = "GREEN P31";   // active palette (Themes::kThemes[i].name)
        int   themeIndex = 0, themeCount = 1;  // position in the palette list (PALETTE < nn/nn >)
        juce::String inputName;                 // current audio source (device friendly name)
        int   inputIndex = -1, inputCount = 0;  // source position in the device list (SOURCE < nn/nn >)
        bool  sourceDenied = false;             // selected source refused access (mic privacy): red warning sign
        // WASAPI loopback state shown in the header — nullptr/"LIVE" when capturing normally,
        // "RECONNECTING" / "NO SIGNAL" when the analyzer has lost the output device.
        const char* audioStatus = "LIVE";
        // Set when a GPU shader failed to compile (a pass is silently no-op'ing). Surfaced in
        // the header so a black/partial scene has an on-screen cause, not just a log line.
        bool shaderError = false;
        // True when the window is in kiosk (true) fullscreen — drives the header's [-]/[+]
        // window-control glyph.
        bool fullscreen = false;
        // Factory-reset is two-step: the first press arms it and the control reads CONFIRM
        // until a second press (or a few seconds pass).
        bool resetArmed = false;
    };

    void setAdvanceFrac (float f) noexcept { adv = f; }

    void build (SceneRenderer::GlyphBatch& b, const ScreenLayout& L, const Inputs& in)
    {
        // energy history ring (GL-thread state, ~2 s at 60 fps)
        ring[(size_t) ringIdx] = juce::jlimit (0.0f, 1.0f, in.feats.overallEnergy);
        ringIdx = (ringIdx + 1) % kRingLen;

        // Smoothed render-rate estimate from the wall-clock gap between frames (build() runs
        // once per GL frame), used by the STATUS readout.
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (lastBuildMs > 0.0)
            {
                const double dt = now - lastBuildMs;
                if (dt > 0.0)
                    fpsEst += (float) ((1000.0 / dt) - fpsEst) * 0.1f;
            }
            lastBuildMs = now;
        }

        const float hue = in.hue;
        const float sat = in.saturation;   // per-theme; desaturates chrome to match the map
        const auto cBar     = juce::Colour::fromHSV (hue, 0.95f * sat, 0.95f, 1.00f);
        const auto cBarText = juce::Colour::fromHSV (hue, 0.9f  * sat, 0.05f, 1.00f);
        const auto cBody    = juce::Colour::fromHSV (hue, 0.9f  * sat, 0.95f, 0.78f);
        const auto cBright  = juce::Colour::fromHSV (hue, 0.85f * sat, 1.00f, 1.00f);
        const auto cBlocks  = juce::Colour::fromHSV (hue, 1.0f  * sat, 0.95f, 0.70f);
        const auto cChrome  = juce::Colour::fromHSV (hue, 1.0f  * sat, 1.00f, 0.50f);

        const bool blinkOn = std::fmod (in.animTime, 1.1f) < 0.66f;

        // --- header: inverse-video bar + dark title text ---------------------
        begin (b);
        solid (b, L.titleBar.getX(), L.titleBar.getY(),
               L.titleBar.getWidth(), L.titleBar.getHeight(), 1.0f);
        end (b, cBar);

        begin (b);
        {
            const float ty = L.titleBar.getY() + (L.titleH - L.textH) * 0.5f;
            const float cw = charW (L.textH);
            // Window controls as retro glyphs. [+] expands to fullscreen / [-] restores at the
            // far left; [x] quits at the far right. Their hotspots reach into the screen corners
            // (ScreenLayout::fullscreenHotspot / closeHotspot) so they're easy to hit, especially
            // in fullscreen where the pointer naturally lands in the corner.
            text (b, L.titleBar.getX() + cw, ty, L.textH, in.fullscreen ? "[-]" : "[+]");
            text (b, L.titleBar.getRight() - 4.0f * cw, ty, L.textH, "[x]");
            // Title shifted right past the toggle (toggle + one space = 4 cells).
            text (b, L.titleBar.getX() + 5.0f * cw, ty, L.textH,
                  "SPECTRAL PLOTTER MK-IV // INTERFACE 2037");
            const juce::String audio = "AUD " + juce::String (in.audioStatus != nullptr ? in.audioStatus : "LIVE");
            const juce::String right = juce::String (in.shaderError ? "GL-ERR  " : "")
                                     + audio
                                     + "  SEED " + juce::String::toHexString ((int) in.seed).toUpperCase()
                                     + "  SR " + juce::String (in.sampleRate);
            // Right-aligned info ends just left of the [x] control (which owns the last 4 cells).
            text (b, L.titleBar.getRight() - 5.0f * cw - (float) right.length() * cw,
                  ty, L.textH, right);
        }
        end (b, cBarText);

        // --- spectrum strip: 64 columns of eighth-blocks under the map -------
        // Stacked mode only: wide mode has no below-map chrome (empty spectrum rect),
        // and the left telemetry band carries its own full-height spectrum ladder.
        if (! L.sidePanels)
        {
            begin (b);
            {
                constexpr int numBands = AudioFeatures::numBands;
                const float colW = L.spectrum.getWidth() / (float) numBands;
                for (int i = 0; i < numBands; ++i)
                {
                    const int id = GlyphAtlas::idForBlockFrac (in.feats.level[i]);
                    if (id < 0) continue;
                    quad (b, L.spectrum.getX() + (float) i * colW, L.spectrum.getY(),
                          colW, L.spectrum.getHeight(), id, 1.0f);
                }
            }
            end (b, cBlocks);
        }

        // --- telemetry + history ----------------------------------------------
        // Narrow/default windows lay these out in stacked rows under the map;
        // wide windows reflow them into the empty side bands (sidePanels).
        if (L.sidePanels)
        {
            buildTelemetryPanel (b, L, in, cBody, cBright, cBlocks);
            buildControlPanel   (b, L, in, cBody, cBright, cBlocks, blinkOn);
        }
        else
        {
            const float x0 = L.margin;
            float rowY = L.telemetryY;

            begin (b);
            text (b, x0, rowY, L.textH,
                  juce::String::formatted ("SCENE %5.2fMS  UPD %5.2fMS  DECAY %.3f  BLOOM %.2f",
                                           in.sceneMs, in.updMs, in.decay, in.bloom));
            end (b, cBody);

            rowY += L.rowH;
            {
                const float vals[3] = { in.feats.bassEnergy, in.feats.overallEnergy, in.feats.highEnergy };
                const char* names[3] = { "BASS", "OVR ", "HIGH" };
                float x = x0;
                for (int i = 0; i < 3; ++i)
                {
                    begin (b);
                    x = text (b, x, rowY, L.textH,
                              juce::String (names[i]) + juce::String::formatted (" %.2f ", vals[i]));
                    end (b, cBody);
                    begin (b);
                    x = meterBar (b, x, rowY, 8, vals[i], L.textH);
                    end (b, cBlocks);
                    x += 2.0f * charW (L.textH);
                }
            }

            rowY += L.rowH;
            {
                begin (b);
                float x = text (b, x0, rowY, L.textH,
                                juce::String::formatted ("HUE %03d ", (int) (hue * 360.0f) % 360));
                end (b, cBody);

                begin (b);
                if (! in.hueLocked || blinkOn)   // LOCKED blinks, TRACK is steady
                    x = text (b, x, rowY, L.textH, in.hueLocked ? "LOCKED" : "TRACK ");
                else
                    x += 6.0f * charW (L.textH);
                end (b, cBright);

                begin (b);
                x = text (b, x + 2.0f * charW (L.textH), rowY, L.textH, "HIST ");
                end (b, cBody);

                begin (b);
                drawHistBars (b, x, rowY, L.textH, 32);
                end (b, cBlocks);
            }
        }

        // --- command line ------------------------------------------------------
        // Stacked (narrow) mode only. Wide mode has no below-map chrome; its clickable
        // controls live in the right control panel instead of on this line.
        if (! L.sidePanels)
        {
            const float y = L.cmdY;
            const float th = L.textH;

            begin (b);
            textAt (b, L, ScreenLayout::colSeed, y, "[ NEW SEED ]");
            textAt (b, L, ScreenLayout::colPersist, y, "PERSIST <");
            textAt (b, L, ScreenLayout::colPersistR, y, ">");
            textAt (b, L, ScreenLayout::colBloom, y, "BLOOM <");
            textAt (b, L, ScreenLayout::colBloomR, y, ">");
            end (b, cBody);

            // live state (lock word, trim values) — bright
            begin (b);
            textAt (b, L, ScreenLayout::colLock, y,
                    in.hueLocked ? "LOCK:ON " : "LOCK:OFF");
            textAt (b, L, ScreenLayout::colPersist + 9, y,
                    juce::String::formatted ("%+.3f", persistShown));
            textAt (b, L, ScreenLayout::colBloom + 7, y,
                    juce::String::formatted ("%4.2f", bloomShown));
            if (blinkOn)
                quad (b, L.colX (ScreenLayout::colCursor), y, charW (th), th,
                      GlyphAtlas::GBlock8, 1.0f);
            end (b, cBright);

            // no control panel in narrow mode, so surface the palette here
            begin (b);
            textAt (b, L, ScreenLayout::colTheme, y,
                    juce::String ("THEME:") + juce::String (in.themeName));
            if (! in.resetArmed)
                textAt (b, L, ScreenLayout::colReset, y, "[ RESET ]");
            else if (blinkOn)
                textAt (b, L, ScreenLayout::colReset, y, "[ CONFIRM ]");
            end (b, cBright);
        }
        ++frameCount;

        // --- map chrome: tick coordinates, grid ref, scale bar, CI legend ------
        begin (b);
        {
            const float th = 9.0f * L.scale;
            const float offX = L.mapOffX, offY = L.mapOffY;
            const float boxSize = L.mapBox.getWidth();

            for (int i = 1; i * 360.0f < boxSize - 40.0f; ++i)
            {
                const float p = i * 360.0f;
                if (p < 40.0f) continue;
                text (b, offX + p + 3.0f, offY + 5.0f,  th, juce::String (4200 + 36 * i));
                text (b, offX + 5.0f, offY + p + 2.0f,  th, juce::String (7800 + 36 * i));
            }

            const juce::String gridRef = juce::String::formatted ("GRID 34SNK %04d %04d",
                                                                  (int) (in.seed % 10000u),
                                                                  (int) ((in.seed / 10000u) % 10000u));
            text (b, offX + boxSize - 14.0f - (float) gridRef.length() * charW (th),
                  offY + boxSize - 36.0f, th, gridRef);

            // scale bar (solid hairlines) + legend
            const float bx = offX + 14.0f, by = offY + boxSize - 42.0f;
            solid (b, bx, by, 72.0f, 1.0f, 1.0f);
            solid (b, bx,         by - 3.0f, 1.0f, 4.0f, 1.0f);
            solid (b, bx + 71.0f, by - 3.0f, 1.0f, 4.0f, 1.0f);
            text (b, bx - 2.0f,  by + 4.0f, th, "0");
            text (b, bx + 72.0f - 4.0f * charW (th), by + 4.0f, th, "1 KM");
            text (b, bx + 72.0f + 16.0f, by - 3.0f, th, "CI 20 M   INDEX 140 M");
        }
        end (b, cChrome);
    }

    // Values shown on the command line / control panel, set from the
    // message-thread atomics before build() (plain members: written+read on GL).
    float persistShown = 0.0f;
    float bloomShown   = 1.0f;
    float threshShown  = 0.58f;
    float scanShown    = 3.0f;
    float crtShown     = 0.5f;
    float shadeShown   = 0.18f;
    float hatchShown   = 0.65f;
    float stippleShown = 0.045f;
    float slopeShown   = 12.0f;

private:
    float adv = 0.45f;

    static constexpr int kRingLen = 128;
    std::array<float, kRingLen> ring {};
    int ringIdx = 0;

    int frameCount = 0;
    int sectionFirst = 0;
    double lastBuildMs = 0.0;   // wall clock of the previous build() (FPS estimate)
    float  fpsEst = 0.0f;       // smoothed frames/sec shown in the STATUS block

    float charW (float textHeight) const noexcept { return textHeight * adv; }

    void begin (SceneRenderer::GlyphBatch& b) noexcept
    {
        sectionFirst = (int) b.verts.size();
    }
    void end (SceneRenderer::GlyphBatch& b, juce::Colour c)
    {
        const int n = (int) b.verts.size() - sectionFirst;
        if (n >= 3)
            b.draws.push_back ({ sectionFirst, n, c });
    }

    // one glyph quad, top-left anchored, stretched to w x h
    static void quad (SceneRenderer::GlyphBatch& b, float x, float y, float w, float h,
                      int id, float alpha)
    {
        float u0, v0, u1, v1;
        GlyphAtlas::getUV (id, u0, v0, u1, v1);
        pushUV (b, x, y, w, h, u0, v0, u1, v1, alpha);
    }

    // solid rectangle through the atlas's white cell; UVs sample only the
    // interior so the cell's transparent guard border never stretches into
    // a visible edge fade
    static void solid (SceneRenderer::GlyphBatch& b, float x, float y, float w, float h,
                       float alpha)
    {
        float u0, v0, u1, v1;
        GlyphAtlas::getUV (GlyphAtlas::GSolid, u0, v0, u1, v1);
        const float um = (u0 + u1) * 0.5f, ud = (u1 - u0) * 0.15f;
        pushUV (b, x, y, w, h, um - ud, 0.35f, um + ud, 0.65f, alpha);
    }

    static void pushUV (SceneRenderer::GlyphBatch& b, float x, float y, float w, float h,
                        float u0, float v0, float u1, float v1, float alpha)
    {
        const float x1 = x + w, y1 = y + h;
        auto v = [&b, alpha] (float px, float py, float u, float vv)
        { b.verts.push_back ({ px, py, u, vv, alpha }); };
        v (x, y, u0, v0);  v (x1, y, u1, v0);  v (x1, y1, u1, v1);
        v (x, y, u0, v0);  v (x1, y1, u1, v1); v (x, y1, u0, v1);
    }

    // monospace text run, top-left anchored; returns the pen x after the run.
    // Glyph quads are wider than the advance (like the map labels), so
    // characters overlap slightly — normal for this atlas.
    float text (SceneRenderer::GlyphBatch& b, float x, float y, float textHeight,
                const juce::String& s)
    {
        const float advPx = charW (textHeight);
        const float gw    = textHeight * GlyphAtlas::cellAspect;
        const float gx0   = (advPx - gw) * 0.5f;
        for (const auto c : s)
        {
            const int id = GlyphAtlas::idForChar (c);
            if (id >= 0)
                quad (b, x + gx0, y, gw, textHeight, id, 1.0f);
            x += advPx;
        }
        return x;
    }

    void textAt (SceneRenderer::GlyphBatch& b, const ScreenLayout& L, int col, float y,
                 const juce::String& s)
    {
        text (b, L.colX (col), y, L.textH, s);
    }

    // dither-block level bar, `cells` characters wide; returns the pen x after
    float meterBar (SceneRenderer::GlyphBatch& b, float x, float y, int cells, float frac,
                    float textHeight)
    {
        const float advPx = charW (textHeight);
        const int lit = (int) std::round (juce::jlimit (0.0f, 1.0f, frac) * (float) cells);
        for (int i = 0; i < cells; ++i)
        {
            quad (b, x, y, advPx, textHeight,
                  i < lit ? GlyphAtlas::GDitherH : GlyphAtlas::GDitherL,
                  i < lit ? 1.0f : 0.45f);
            x += advPx;
        }
        return x;
    }

    // mirror of meterBar: the lit cells fill from the RIGHT edge inward, so the
    // bar grows toward its label on the map side (right-aligned telemetry panel).
    void meterBarR (SceneRenderer::GlyphBatch& b, float x, float y, int cells, float frac,
                    float textHeight)
    {
        const float advPx = charW (textHeight);
        const int lit = (int) std::round (juce::jlimit (0.0f, 1.0f, frac) * (float) cells);
        for (int i = 0; i < cells; ++i)
        {
            const bool on = i >= cells - lit;   // rightmost `lit` cells lit
            quad (b, x + (float) i * advPx, y, advPx, textHeight,
                  on ? GlyphAtlas::GDitherH : GlyphAtlas::GDitherL,
                  on ? 1.0f : 0.45f);
        }
    }

    // energy-history block bars from the ring, `cols` columns of block glyphs;
    // each column is the max over its slice of the ring (most recent at left).
    void drawHistBars (SceneRenderer::GlyphBatch& b, float x, float y, float textHeight,
                       int cols)
    {
        cols = juce::jlimit (8, kRingLen, cols);
        const float advPx = charW (textHeight);
        const int   stride = juce::jmax (1, kRingLen / cols);
        for (int i = 0; i < cols; ++i)
        {
            float v = 0.0f;
            for (int k = 0; k < stride; ++k)
                v = juce::jmax (v, ring[(size_t) ((ringIdx + i * stride + k) % kRingLen)]);
            const int id = GlyphAtlas::idForBlockFrac (v);
            if (id >= 0)
                quad (b, x + (float) i * advPx, y, advPx, textHeight, id, 1.0f);
        }
    }

    // label + a meter bar that spans to `panelRight`, so each row fills the
    // band's full width. `label` should carry a trailing space for the gap.
    void barRow (SceneRenderer::GlyphBatch& b, float x0, float panelRight, float y,
                 float th, const juce::String& label, float frac,
                 juce::Colour cLabel, juce::Colour cBar)
    {
        begin (b);
        const float x = text (b, x0, y, th, label);
        end (b, cLabel);
        const int cells = (int) ((panelRight - x) / charW (th));
        if (cells > 0)
        {
            begin (b);
            meterBar (b, x, y, cells, frac, th);
            end (b, cBar);
        }
    }

    // mirror of barRow: label right-aligned flush to `panelRight` (the map side),
    // with the meter bar to its left, growing rightward toward the label. Used by
    // the right-aligned telemetry panel.
    void barRowR (SceneRenderer::GlyphBatch& b, float x0, float panelRight, float y,
                  float th, const juce::String& label, float frac,
                  juce::Colour cLabel, juce::Colour cBar)
    {
        const float cw = charW (th);
        const float labelX = panelRight - (float) label.length() * cw;
        begin (b);
        text (b, labelX, y, th, label);
        end (b, cLabel);

        const int cells = (int) ((labelX - x0) / cw) - 1;   // 1-cell gap before label
        if (cells > 0)
        {
            begin (b);
            meterBarR (b, labelX - cw - (float) cells * cw, y, cells, frac, th);
            end (b, cBar);
        }
    }

    // Left side band: telemetry, level meters and a full-height spectrum ladder,
    // packed top-to-bottom so the band reads dense like a war-room console.
    void buildTelemetryPanel (SceneRenderer::GlyphBatch& b, const ScreenLayout& L,
                              const Inputs& in, juce::Colour cBody, juce::Colour cBright,
                              juce::Colour cBlocks)
    {
        const float x0 = L.leftPanel.getX();
        const float xr = L.leftPanel.getRight();
        const float th = L.textH, rh = L.rowH;
        const float cw = charW (th);
        const float yBot = L.leftPanel.getBottom();
        float y = L.leftPanel.getY();

        // Right-aligned: every run ends flush at the panel's right edge (the map
        // side), mirroring the control panel that hugs the map on the opposite band.
        auto header = [&] (const juce::String& s) { begin (b); text (b, xr - (float) s.length() * cw, y, th, s); end (b, cBright); y += rh; };
        auto line   = [&] (const juce::String& s) { begin (b); text (b, xr - (float) s.length() * cw, y, th, s); end (b, cBody);   y += rh; };

        header ("// TELEMETRY");
        line (juce::String::formatted ("SCENE  %6.2f MS", in.sceneMs));
        line (juce::String::formatted ("UPDATE %6.2f MS", in.updMs));
        line (juce::String::formatted ("DECAY  %6.3f",    in.decay));
        line (juce::String::formatted ("BLOOM  %6.2f",    in.bloom));
        line ("SEED   " + juce::String::toHexString ((int) in.seed).toUpperCase().paddedLeft ('0', 6));
        y += rh * 0.4f;

        header ("// LEVELS");
        const float vals[3]  = { in.feats.bassEnergy, in.feats.overallEnergy, in.feats.highEnergy };
        const char* names[3] = { " BASS", " OVER", " HIGH" };
        for (int i = 0; i < 3; ++i)
            { barRowR (b, x0, xr, y, th, names[i], vals[i], cBody, cBlocks); y += rh; }
        y += rh * 0.4f;

        header ("// SPECTRUM 64CH");
        // fill the rest of the band with a labelled band ladder
        const float pitch = th * 1.22f;
        const int   rows  = juce::jmax (1, (int) ((yBot - y) / pitch));
        const int   nb    = AudioFeatures::numBands;
        const int   grp   = juce::jmax (1, (nb + rows - 1) / rows);
        for (int r = 0; r < rows; ++r)
        {
            float v = 0.0f;
            for (int k = 0; k < grp; ++k)
                v = juce::jmax (v, in.feats.level[juce::jmin (nb - 1, r * grp + k)]);
            const int hz = (int) (20.0 * std::pow (1000.0, (r + 0.5) / (double) rows));
            barRowR (b, x0, xr, y, th, juce::String::formatted ("%5d", hz), v, cBody, cBlocks);
            y += pitch;
        }
    }

    // Right side band: the interactive control panel. Each parameter is a
    // labelled row with either a [button]/[toggle] or a '< value >' stepper and
    // a normalized value bar; a status log fills the remaining band height. The
    // clickable rects live in ScreenLayout so hit-testing matches the glyphs.
    void buildControlPanel (SceneRenderer::GlyphBatch& b, const ScreenLayout& L,
                            const Inputs& in, juce::Colour cBody, juce::Colour cBright,
                            juce::Colour cBlocks, bool blinkOn)
    {
        const float x0 = L.rightPanel.getX();
        const float xr = L.rightPanel.getRight();
        const float th = L.textH, rh = L.rowH;
        const float yBot = L.rightPanel.getBottom();
        const int   barCells = juce::jmax (4, (int) ((xr - x0) / charW (th)));

        begin (b); text (b, x0, L.rightPanel.getY(), th, "// CONTROL"); end (b, cBright);

        // '< value >' stepper + a normalized value bar on the row below
        auto slider = [&] (int idx, const juce::String& label,
                           const juce::String& valStr, float frac)
        {
            const float ry = L.ctrlRowY (idx);
            begin (b); text (b, x0, ry, th, label); end (b, cBody);
            begin (b);
            text (b, L.ctrlDnX(), ry, th, "<");
            text (b, L.ctrlUpX(), ry, th, ">");
            end (b, cBright);
            begin (b); text (b, L.ctrlValueX(), ry, th, valStr); end (b, cBright);
            begin (b); meterBar (b, x0, ry + rh, barCells, juce::jlimit (0.0f, 1.0f, frac), th); end (b, cBlocks);
        };

        // RESEED — action button
        {
            const float ry = L.ctrlRowY (ScreenLayout::ctReseed);
            begin (b); text (b, x0, ry, th, "SEED"); end (b, cBody);
            begin (b); text (b, L.ctrlValX, ry, th, "[ RESEED ]"); end (b, cBright);
            begin (b); text (b, x0, ry + rh, th,
                             "ID " + juce::String::toHexString ((int) in.seed).toUpperCase().paddedLeft ('0', 6));
                       end (b, cBody);
        }
        // HUE LOCK — toggle
        {
            const float ry = L.ctrlRowY (ScreenLayout::ctLock);
            begin (b); text (b, x0, ry, th, "HUE LOCK"); end (b, cBody);
            begin (b); text (b, L.ctrlValX, ry, th, in.hueLocked ? "[ ON  ]" : "[ OFF ]"); end (b, cBright);
            begin (b); text (b, x0, ry + rh, th,
                             "HUE " + juce::String ((int) (in.hue * 360.0f) % 360).paddedLeft ('0', 3)
                             + (in.hueLocked ? " HELD" : " TRACK")); end (b, cBody);
        }
        // PALETTE — stepper like the sliders: < prev, nn/nn position, > next
        {
            const float ry = L.ctrlRowY (ScreenLayout::ctTheme);
            begin (b); text (b, x0, ry, th, "PALETTE"); end (b, cBody);
            begin (b);
            text (b, L.ctrlDnX(), ry, th, "<");
            text (b, L.ctrlUpX(), ry, th, ">");
            text (b, L.ctrlValueX(), ry, th,
                  juce::String (in.themeIndex + 1).paddedLeft ('0', 2) + "/"
                  + juce::String (in.themeCount).paddedLeft ('0', 2));
            end (b, cBright);
            begin (b); text (b, x0, ry + rh, th,
                             juce::String (in.themeName).paddedRight (' ', 12)); end (b, cBright);
        }
        // AUDIO SOURCE — < > step through every output (loopback) and input device
        {
            const float ry = L.ctrlRowY (ScreenLayout::ctInput);
            // Fit the device name to the band width on the row below the button (ASCII atlas,
            // so a truncated name ends in '>' rather than an ellipsis glyph).
            const int maxChars = juce::jmax (6, barCells - 1);
            juce::String src = in.inputName.isNotEmpty() ? in.inputName.toUpperCase()
                                                         : juce::String ("...");
            if (src.length() > maxChars)
                src = src.substring (0, maxChars - 1) + ">";
            begin (b); text (b, x0, ry, th, "SOURCE"); end (b, cBody);
            if (in.sourceDenied)   // red warning sign in the label gap: access denied (mic privacy)
            {
                begin (b);
                quad (b, x0 + 7.0f * charW (th), ry, charW (th), th, GlyphAtlas::GWarn, 1.0f);
                end (b, juce::Colour::fromHSV (0.0f, 0.85f, 1.00f, 1.00f));   // fixed red on every theme
            }
            begin (b);
            text (b, L.ctrlDnX(), ry, th, "<");
            text (b, L.ctrlUpX(), ry, th, ">");
            text (b, L.ctrlValueX(), ry, th,
                  (in.inputIndex >= 0 ? juce::String (in.inputIndex + 1).paddedLeft ('0', 2)
                                      : juce::String ("--"))
                  + "/" + juce::String (in.inputCount).paddedLeft ('0', 2));
            end (b, cBright);
            begin (b); text (b, x0, ry + rh, th, src); end (b, cBright);
        }
        // DEFAULTS — two-step factory reset of the look (sliders, palette, hue lock)
        {
            const float ry = L.ctrlRowY (ScreenLayout::ctReset);
            begin (b); text (b, x0, ry, th, "DEFAULTS"); end (b, cBody);
            begin (b);
            if (! in.resetArmed)   text (b, L.ctrlValX, ry, th, "[ RESET ]");
            else if (blinkOn)      text (b, L.ctrlValX, ry, th, "[ CONFIRM ]");
            end (b, cBright);
            begin (b); text (b, x0, ry + rh, th, in.resetArmed ? "PRESS AGAIN" : "FACTORY LOOK");
                       end (b, in.resetArmed ? cBright : cBody);
        }
        slider (ScreenLayout::ctPersist, "PERSIST",  juce::String::formatted ("%+.3f", persistShown), (persistShown + 0.05f) / 0.10f);
        slider (ScreenLayout::ctBloom,   "BLOOM",    juce::String::formatted ("%5.2f", bloomShown),   (bloomShown   - 0.50f) / 1.00f);
        slider (ScreenLayout::ctThresh,  "B.THRESH", juce::String::formatted ("%5.2f", threshShown),  (threshShown  - 0.20f) / 0.70f);
        slider (ScreenLayout::ctScan,    "SCANLINE", juce::String::formatted ("%5.2f", scanShown),    (scanShown    - 1.00f) / 5.00f);
        slider (ScreenLayout::ctCrt,     "CRT FX",   juce::String::formatted ("%5.2f", crtShown),     crtShown);
        slider (ScreenLayout::ctShade,   "HILLSHD",  juce::String::formatted ("%5.2f", shadeShown),   shadeShown   / 0.50f);
        slider (ScreenLayout::ctHatch,   "HACHURE",  juce::String::formatted ("%5.2f", hatchShown),   hatchShown);
        slider (ScreenLayout::ctStipple, "STIPPLE",  juce::String::formatted ("%5.3f", stippleShown), stippleShown / 0.12f);
        slider (ScreenLayout::ctSlope,   "RELIEF",   juce::String::formatted ("%5.1f", slopeShown),   (slopeShown - 4.0f) / 20.0f);

        // --- live status readout: real signal + performance telemetry, filling the
        //     rest of the band. Every line reflects this frame's actual state.
        const float cw = charW (th);
        float y = L.ctrlRowY (ScreenLayout::numCtrl) + rh * 0.5f;
        begin (b); text (b, x0, y, th, "// STATUS"); end (b, cBright); y += rh;

        // Derived signal metrics from this frame's features.
        const auto& f = in.feats;
        int   peakIdx = 0;
        float peakVal = 0.0f, fluxMean = 0.0f;
        for (int i = 0; i < AudioFeatures::numBands; ++i)
        {
            if (f.level[i] > peakVal) { peakVal = f.level[i]; peakIdx = i; }
            fluxMean += f.flux[i];
        }
        fluxMean /= (float) AudioFeatures::numBands;
        // 64 log-spaced bands over 20 Hz–20 kHz (matches the analyzer's mapping).
        const float peakHz   = 20.0f * std::pow (1000.0f, (peakIdx + 0.5f) / (float) AudioFeatures::numBands);
        const float brightHz = 20.0f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, f.centroidNorm));

        auto hz = [] (float v) -> juce::String
        {
            return v >= 1000.0f ? juce::String (v / 1000.0f, 1) + "K"
                                : juce::String (juce::roundToInt (v));
        };

        // A labelled meter line + right-aligned value; draws only while it fits the band.
        auto meter = [&] (const juce::String& label, float frac, const juce::String& val)
        {
            if (y + rh > yBot) return;
            begin (b); text (b, x0, y, th, label); end (b, cBody);
            const float barX  = x0 + 6.0f * cw;
            const int   cells = juce::jmax (3, (int) ((xr - barX) / cw) - (val.length() + 1));
            begin (b); meterBar (b, barX, y, cells, juce::jlimit (0.0f, 1.0f, frac), th); end (b, cBlocks);
            if (val.isNotEmpty())
                { begin (b); text (b, xr - (float) val.length() * cw, y, th, val); end (b, cBright); }
            y += rh;
        };
        auto info = [&] (const juce::String& s)
        {
            if (y + rh > yBot) return;
            begin (b); text (b, x0, y, th, s); end (b, cBody); y += rh;
        };

        info (juce::String ("STATE ") + (in.audioStatus != nullptr ? in.audioStatus : "LIVE")
              + "  SR " + juce::String (in.sampleRate / 1000.0f, 1) + "K");
        meter ("SIGNAL", f.overallEnergy, juce::String (juce::roundToInt (f.overallEnergy * 100.0f)) + "%");
        meter ("LOW",    f.bassEnergy, {});
        meter ("HIGH",   f.highEnergy, {});
        meter ("BRIGHT", juce::jlimit (0.0f, 1.0f, f.centroidNorm), hz (brightHz) + "HZ");
        meter ("ONSET",  juce::jlimit (0.0f, 1.0f, fluxMean * 6.0f), {});
        info ("PEAK  " + hz (peakHz) + "HZ LVL "
              + juce::String (juce::roundToInt (peakVal * 100.0f)) + "%");
        info (juce::String::formatted ("FPS %5.1f  SCN %4.2fMS", fpsEst, in.sceneMs));
        info (juce::String::formatted ("UPD %4.2fMS DEC %.3f", in.updMs, in.decay));
        info ("FFT 4096 HANN  CI 60 LVL");
        info ("T+" + juce::String ((int) in.animTime).paddedLeft ('0', 6)
              + "  FR " + juce::String::toHexString (frameCount & 0xFFFFFF).toUpperCase());

        // blinking prompt on the next free line
        if (blinkOn && y + th <= yBot)
        {
            begin (b);
            quad (b, x0, y, charW (th), th, GlyphAtlas::GBlock8, 1.0f);
            end (b, cBright);
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScreenTextBuilder)
};
