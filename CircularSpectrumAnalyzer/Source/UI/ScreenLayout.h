#pragma once

#include <JuceHeader.h>

// Layout of the full-screen CRT terminal (MU-TH-UR style): everything the tube
// displays — header bar, map box, spectrum strip, telemetry rows, command
// line — plus the clickable hotspot rects of the command-line segments.
//
// Pure and deterministic: the GL thread (text building) and the message thread
// (mouse hit-testing) both call compute() and agree exactly. Text is placed on
// a character grid; charW derives from the glyph atlas's measured monospace
// advance so hotspots line up with the drawn glyphs.
//
// The type metrics scale with window size relative to a reference (the default
// 624x724 window == scale 1.0), so a maximized window shows the same chrome
// enlarged rather than tiny type hugging the top. When the window is wide
// enough that a centered square map leaves empty side bands, the telemetry and
// history readouts reflow into those bands (sidePanels) so the screen stays
// populated instead of empty.
struct ScreenLayout
{
    // base type metrics at scale 1.0 (logical px, the original fixed values)
    static constexpr float baseMargin = 12.0f;
    static constexpr float baseTitleH = 16.0f;   // inverse-video header bar height
    static constexpr float baseTextH  = 11.0f;   // glyph cell height of body text
    static constexpr float baseRowH   = 14.0f;   // body line pitch
    static constexpr float baseStripH = 12.0f;   // spectrum strip height

    // reference window == scale 1.0 (matches MainComponent's setSize)
    static constexpr float refW     = 624.0f;
    static constexpr float refH     = 724.0f;
    static constexpr float maxScale = 3.5f;

    // scaled instance metrics (== base values at scale 1.0)
    float scale  = 1.0f;
    float margin = baseMargin;
    float titleH = baseTitleH;
    float textH  = baseTextH;
    float rowH   = baseRowH;
    float stripH = baseStripH;
    float charW  = 5.5f;                      // textH * atlas advance fraction

    juce::Rectangle<float> titleBar;         // inverse-video band
    juce::Rectangle<float> mapBox;           // square CRT map plot
    juce::Rectangle<float> spectrum;         // 64-column text-mode EQ
    juce::Rectangle<float> leftPanel;        // telemetry band (sidePanels only)
    juce::Rectangle<float> rightPanel;       // control-panel band (sidePanels only)
    bool  sidePanels = false;                // reflow telemetry/controls into bands
    float telemetryY = 0.0f;                 // top of the first telemetry row (stacked)
    float cmdY       = 0.0f;                 // top of the command line row

    // Control panel occupying the right band (sidePanels only). Each control is
    // one primary row; sliders add a value bar on the row below. The text
    // builder places labels/values from these anchors and the message thread
    // hit-tests the same rects, so drawn glyphs and clickable areas always agree.
    enum { ctReseed = 0, ctLock, ctTheme, ctInput, ctPersist, ctBloom, ctThresh, ctScan, ctCrt,
           ctShade, ctHatch, ctStipple, ctSlope, numCtrl };
    static constexpr int ctFirstSlider = ctPersist;   // ctPersist..numCtrl-1 are value sliders
    static constexpr float ctrlLabelChars = 9.0f;   // label field width, chars
    bool  ctrlValid   = false;
    float ctrlY0      = 0.0f;                // top of the first control row
    float ctrlRowStep = 0.0f;                // vertical pitch per control
    float ctrlValX    = 0.0f;                // x of the '<' / '[' value field
    juce::Rectangle<float> ctrlAction[numCtrl];  // button/toggle hotspots
    juce::Rectangle<float> ctrlDn[numCtrl];      // '<' hotspots (sliders)
    juce::Rectangle<float> ctrlUp[numCtrl];      // '>' hotspots (sliders)
    juce::Rectangle<float> ctrlBar[numCtrl];     // value-bar hotspots (click-to-set)

    float ctrlRowY  (int i) const noexcept { return ctrlY0 + (float) i * ctrlRowStep; }
    float ctrlDnX   ()      const noexcept { return ctrlValX; }
    float ctrlValueX()      const noexcept { return ctrlValX + 2.0f * charW; }
    float ctrlUpX   ()      const noexcept { return ctrlValX + 8.0f * charW; }

    float mapScale = 1.0f;                   // grid units -> logical px
    float mapOffX  = 0.0f, mapOffY = 0.0f;

    // Command-line segments as character columns. The text builder places each
    // segment at colX(col); the hotspots below use the same columns, so the
    // clickable areas always cover the drawn text.
    static constexpr int colSeed     = 0;    // "[ NEW SEED ]"
    static constexpr int lenSeed     = 12;
    static constexpr int colLock     = 15;   // "LOCK:OFF" / "LOCK:ON"
    static constexpr int lenLock     = 8;
    static constexpr int colPersist  = 26;   // "PERSIST <+0.000>"
    static constexpr int colPersistL = 34;   // '<'
    static constexpr int colPersistR = 41;   // '>'
    static constexpr int colBloom    = 45;   // "BLOOM <1.00>"
    static constexpr int colBloomL   = 51;   // '<'
    static constexpr int colBloomR   = 56;   // '>'
    static constexpr int colCursor   = 59;   // blinking block
    static constexpr int colTheme    = 62;   // "THEME:<NAME>" readout (always visible)

    float colX (int col) const noexcept { return margin + (float) col * charW; }

    juce::Rectangle<float> cmdRect (int col, int numChars, float pad = 2.0f) const noexcept
    {
        return juce::Rectangle<float> (colX (col), cmdY,
                                       (float) numChars * charW, rowH)
                   .expanded (pad * scale);
    }

    // Retro window controls: [+]/[-] fullscreen toggle at the far left, [x] quit at the far
    // right. Both are present in every UI mode. Their targets deliberately extend to the
    // window's top corners (x/y run to 0 / w) — corners are the easiest thing to hit with the
    // mouse, which matters most in fullscreen where the pointer slams into the screen edge.
    // They're 6 cells wide (glyph + a cell of slack each side) and run halfway into the
    // gap under the header bar, so a slightly low click on the glyph still lands.
    juce::Rectangle<float> fullscreenHotspot() const noexcept
    {
        return { 0.0f, 0.0f, titleBar.getX() + 6.0f * charW, controlHotspotBottom() };
    }

    juce::Rectangle<float> closeHotspot() const noexcept
    {
        const float rightEdge = titleBar.getRight() + margin;   // == window width
        const float x = titleBar.getRight() - 6.0f * charW;
        return { x, 0.0f, rightEdge - x, controlHotspotBottom() };
    }

    float controlHotspotBottom() const noexcept { return titleBar.getBottom() + 4.0f * scale; }

    juce::Rectangle<float> seedHotspot()        const { return cmdRect (colSeed, lenSeed); }
    juce::Rectangle<float> lockHotspot()        const { return cmdRect (colLock, lenLock); }
    juce::Rectangle<float> persistDownHotspot() const { return cmdRect (colPersistL, 1, 4.0f); }
    juce::Rectangle<float> persistUpHotspot()   const { return cmdRect (colPersistR, 1, 4.0f); }
    juce::Rectangle<float> bloomDownHotspot()   const { return cmdRect (colBloomL, 1, 4.0f); }
    juce::Rectangle<float> bloomUpHotspot()     const { return cmdRect (colBloomR, 1, 4.0f); }

    static constexpr int telemetryRows = 3;  // rows between spectrum and command line (stacked)

    static ScreenLayout compute (float w, float h, int gridSize, float advanceFrac)
    {
        ScreenLayout L;
        L.scale  = juce::jlimit (1.0f, maxScale, juce::jmin (w / refW, h / refH));
        L.margin = baseMargin * L.scale;
        L.titleH = baseTitleH * L.scale;
        L.textH  = baseTextH  * L.scale;
        L.rowH   = baseRowH   * L.scale;
        L.stripH = baseStripH * L.scale;
        L.charW  = L.textH * advanceFrac;

        const float m    = L.margin;
        const float s    = L.scale;
        const float gapA = 4.0f * s, gapB = 6.0f * s, gapT = 8.0f * s;

        L.titleBar = { m, 8.0f * s, w - 2.0f * m, L.titleH };
        const float mapY = L.titleBar.getBottom() + gapT;

        // Bottom-strip height in stacked (narrow) mode: spectrum strip, three
        // telemetry rows and the command line all live below the map.
        const float bottomStacked = gapA + L.stripH + gapB
                                   + (float) telemetryRows * L.rowH + L.rowH + m;
        // Wide (side-panel) mode drops ALL below-map chrome — the telemetry and
        // controls sit in the side bands, so only the window margin sits under the
        // enlarged map, which grows to fill the freed vertical space.
        const float bottomSide = m;

        // Decide side-panel mode from the map size it would actually produce.
        const float sideSide = juce::jmax (64.0f, juce::jmin (w - 2.0f * m,
                                                              h - mapY - bottomSide));
        const float bandW    = (w - sideSide) * 0.5f - m - gapB;
        const float minBandW = 20.0f * L.charW;   // fits the widest telemetry line
        L.sidePanels = bandW >= minBandW;

        const float side = L.sidePanels
                             ? sideSide
                             : juce::jmax (64.0f, juce::jmin (w - 2.0f * m,
                                                              h - mapY - bottomStacked));

        L.mapBox = { (w - side) * 0.5f, mapY, side, side };

        if (L.sidePanels)
        {
            // No spectrum strip or command line under the map in wide mode; an empty
            // spectrum rect tells the text builder to skip it.
            L.spectrum   = {};
            L.leftPanel  = { m, mapY, L.mapBox.getX() - m - gapB, side };
            L.rightPanel = { L.mapBox.getRight() + gapB, mapY,
                             (w - m) - (L.mapBox.getRight() + gapB), side };
            L.telemetryY = mapY;                       // unused, kept sane
            L.cmdY       = L.mapBox.getBottom();        // unused (no command line)

            // control-panel row geometry + hotspots (right band)
            L.ctrlValid   = true;
            L.ctrlY0      = L.rightPanel.getY() + L.rowH * 1.6f;   // below the "// CONTROL" header
            L.ctrlRowStep = L.rowH * 2.0f;                        // primary row + value bar
            L.ctrlValX    = L.rightPanel.getX() + ctrlLabelChars * L.charW;
            for (int i = 0; i < numCtrl; ++i)
            {
                const float ry = L.ctrlRowY (i);
                L.ctrlAction[i] = { L.ctrlValX, ry, 10.0f * L.charW, L.rowH };
                L.ctrlAction[i] = L.ctrlAction[i].expanded (3.0f * s);
                L.ctrlDn[i]     = juce::Rectangle<float> (L.ctrlDnX(), ry, L.charW, L.rowH).expanded (4.0f * s);
                L.ctrlUp[i]     = juce::Rectangle<float> (L.ctrlUpX(), ry, L.charW, L.rowH).expanded (4.0f * s);
                // value bar sits on the row below the stepper; click anywhere on it to set
                L.ctrlBar[i]    = { L.rightPanel.getX(), ry + L.rowH, L.rightPanel.getWidth(), L.rowH };
            }
        }
        else
        {
            L.spectrum   = { L.mapBox.getX(), L.mapBox.getBottom() + gapA, side, L.stripH };
            L.telemetryY = L.spectrum.getBottom() + gapB;
            L.cmdY       = L.telemetryY + (float) telemetryRows * L.rowH;
        }

        L.mapScale = side / (float) (gridSize - 1);
        L.mapOffX  = L.mapBox.getX();
        L.mapOffY  = L.mapBox.getY();
        return L;
    }
};
