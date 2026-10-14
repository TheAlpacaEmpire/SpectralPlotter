#pragma once

#include <JuceHeader.h>

#include <cstring>
#include <vector>

// Single-channel glyph atlas shared by the in-scene map labels AND the
// full-screen terminal UI (header, telemetry, command line). Rasterized once
// on the message thread with JUCE Graphics, then handed to SceneRenderer as
// raw bytes for a GL_R8 texture. Glyphs are drawn oversized (36 px font in a
// 32x48 cell) and minified at draw time, so the padding inside each cell
// covers GL_LINEAR bleed.
//
// Header-only so it needs no Projucer regeneration.
class GlyphAtlas
{
public:
    enum : int
    {
        D0 = 0,            // '0'..'9' at ids 0..9
        GA       = 10,     // 'A'..'Z' at ids 10..35
        GTri     = 36,     // triangulation-point triangle
        GDot     = 37,     // clutter: dot
        GTick    = 38,     // clutter: slanted tick
        GCross   = 39,     // clutter: small cross

        // terminal punctuation
        GMinus   = 40,     // '-'
        GDotP    = 41,     // '.'
        GColon   = 42,     // ':'
        GSlash   = 43,     // '/'
        GLess    = 44,     // '<'
        GGreater = 45,     // '>'
        GLBrack  = 46,     // '['
        GRBrack  = 47,     // ']'
        GPlus    = 48,     // '+'
        GPercent = 49,     // '%'
        GEquals  = 50,     // '='

        // text-mode graphics: eighth-blocks (1/8 .. 8/8 filled from the
        // baseline up), light/heavy dither, and a solid cell for rules/bars.
        GBlock1  = 51,     // '▁' .. GBlock8 = '█'
        GBlock8  = 58,
        GDitherL = 59,     // '░'
        GDitherH = 60,     // '▓'
        GSolid   = 61,     // full-white cell (rules, inverse-video bars)
        GWarn    = 62,     // warning sign: outlined triangle with a "!" (error flags)

        numGlyphs = 63
    };

    static constexpr int cellW = 32, cellH = 48;
    static constexpr float cellAspect = (float) cellW / (float) cellH;

    void build()
    {
        juce::Image img (juce::Image::SingleChannel, cellW * numGlyphs, cellH, true);
        {
            juce::Graphics g (img);
            g.setColour (juce::Colours::white);

            const juce::Font font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                      36.0f, juce::Font::plain));
            g.setFont (font);

            // Monospace advance measured from two positioned glyphs (robust
            // against deprecated width APIs across JUCE versions).
            {
                juce::GlyphArrangement ga;
                ga.addLineOfText (font, "00", 0.0f, 0.0f);
                advanceFrac = ga.getNumGlyphs() >= 2
                                  ? (ga.getGlyph (1).getLeft() - ga.getGlyph (0).getLeft()) / (float) cellH
                                  : 0.45f;
            }

            auto cellRect = [] (int id)
            {
                return juce::Rectangle<int> (id * cellW, 0, cellW, cellH);
            };
            auto drawChar = [&] (int id, juce::juce_wchar c)
            {
                g.drawText (juce::String::charToString (c), cellRect (id),
                            juce::Justification::centred, false);
            };

            for (int d = 0; d <= 9; ++d)
                drawChar (D0 + d, (juce::juce_wchar) ('0' + d));
            for (int a = 0; a < 26; ++a)
                drawChar (GA + a, (juce::juce_wchar) ('A' + a));

            drawChar (GMinus,   '-');
            drawChar (GDotP,    '.');
            drawChar (GColon,   ':');
            drawChar (GSlash,   '/');
            drawChar (GLess,    '<');
            drawChar (GGreater, '>');
            drawChar (GLBrack,  '[');
            drawChar (GRBrack,  ']');
            drawChar (GPlus,    '+');
            drawChar (GPercent, '%');
            drawChar (GEquals,  '=');

            // Triangulation triangle: outline, sized to match the digits' cap height.
            {
                const float cx = GTri * cellW + cellW * 0.5f, cy = cellH * 0.5f;
                juce::Path p;
                p.addTriangle (cx, cy - 12.0f, cx - 12.0f, cy + 10.0f, cx + 12.0f, cy + 10.0f);
                g.strokePath (p, juce::PathStrokeType (3.0f));
            }
            // Warning sign: the triangulation triangle's stroke, a touch larger, with a "!"
            {
                const float cx = GWarn * cellW + cellW * 0.5f, cy = cellH * 0.5f;
                juce::Path p;
                p.addTriangle (cx, cy - 14.0f, cx - 14.0f, cy + 11.0f, cx + 14.0f, cy + 11.0f);
                g.strokePath (p, juce::PathStrokeType (3.0f, juce::PathStrokeType::mitered));
                g.fillRect (cx - 1.5f, cy - 6.0f, 3.0f, 9.0f);   // bar
                g.fillRect (cx - 1.5f, cy + 5.0f, 3.0f, 3.0f);   // dot
            }
            // Clutter dot
            {
                const float cx = GDot * cellW + cellW * 0.5f, cy = cellH * 0.5f;
                g.fillEllipse (cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
            }
            // Clutter tick: short slanted stroke
            {
                const float cx = GTick * cellW + cellW * 0.5f, cy = cellH * 0.5f;
                g.drawLine (cx - 7.0f, cy + 7.0f, cx + 7.0f, cy - 7.0f, 3.0f);
            }
            // Clutter cross
            {
                const float cx = GCross * cellW + cellW * 0.5f, cy = cellH * 0.5f;
                g.drawLine (cx - 8.0f, cy, cx + 8.0f, cy, 3.0f);
                g.drawLine (cx, cy - 8.0f, cx, cy + 8.0f, 3.0f);
            }

            // Eighth-blocks: fill i/8 of the cell from the bottom up. Inset 3 px
            // horizontally so GL_LINEAR minification never bleeds across cells;
            // adjacent block glyphs still read as a continuous strip.
            for (int i = 1; i <= 8; ++i)
            {
                const int id = GBlock1 + i - 1;
                const float bh = cellH * (float) i / 8.0f;
                g.fillRect ((float) (id * cellW) + 3.0f, (float) cellH - bh,
                            (float) cellW - 6.0f, bh);
            }

            // Dither cells: sparse / dense checker stipple.
            {
                for (int y = 0; y < cellH; y += 4)
                    for (int x = 0; x < cellW; x += 4)
                    {
                        if (((x + y) / 4) % 2 == 0)
                            g.fillRect (GDitherH * cellW + x, y, 2, 2);
                        if (((x + y) / 4) % 4 == 0)
                            g.fillRect (GDitherL * cellW + x, y, 2, 2);
                    }
            }

            // Solid cell: inset 2 px so linear sampling at the cell borders
            // stays clean; drawn quads stretch it, so the inset is invisible.
            g.fillRect (GSolid * cellW + 2, 2, cellW - 4, cellH - 4);
        }

        // Flatten to a tight byte plane for the GL_R8 upload (row 0 = image top).
        w = img.getWidth();
        h = img.getHeight();
        pix.resize ((size_t) w * (size_t) h);
        const juce::Image::BitmapData bd (img, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < h; ++y)
            std::memcpy (pix.data() + (size_t) y * (size_t) w, bd.getLinePointer (y), (size_t) w);
    }

    int getWidth()  const noexcept { return w; }
    int getHeight() const noexcept { return h; }
    const std::vector<juce::uint8>& getPixels() const noexcept { return pix; }

    // Horizontal advance between text glyphs, as a fraction of the drawn glyph height.
    float getAdvanceFrac() const noexcept { return advanceFrac; }

    static void getUV (int id, float& u0, float& v0, float& u1, float& v1) noexcept
    {
        u0 = (float) id / (float) numGlyphs;
        u1 = (float) (id + 1) / (float) numGlyphs;
        v0 = 0.0f;
        v1 = 1.0f;
    }

    static int idForChar (juce::juce_wchar c) noexcept
    {
        if (c >= '0' && c <= '9') return D0 + (int) (c - '0');
        if (c >= 'A' && c <= 'Z') return GA + (int) (c - 'A');
        if (c >= 'a' && c <= 'z') return GA + (int) (c - 'a');
        switch (c)
        {
            case '-': return GMinus;
            case '.': return GDotP;
            case ':': return GColon;
            case '/': return GSlash;
            case '<': return GLess;
            case '>': return GGreater;
            case '[': return GLBrack;
            case ']': return GRBrack;
            case '+': return GPlus;
            case '%': return GPercent;
            case '=': return GEquals;
            case '^': return GTri;
            default:  return -1;   // includes ' ': caller advances the pen
        }
    }

    // Eighth-block id for a 0..1 level (returns -1 below the first step so
    // silent columns stay dark).
    static int idForBlockFrac (float frac) noexcept
    {
        const int i = (int) std::ceil (juce::jlimit (0.0f, 1.0f, frac) * 8.0f);
        return i <= 0 ? -1 : GBlock1 + (i - 1);
    }

private:
    std::vector<juce::uint8> pix;
    int w = 0, h = 0;
    float advanceFrac = 0.45f;
};
