#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <cmath>
#include <random>

#include "../Terrain/TerrainModel.h"
#include "../River/RiverModel.h"

// Message-thread map-label placement. Produces three record streams that the GL
// thread turns into glyph quads + contour gaps:
//   - contour elevation labels riding the index contours (tracked with hysteresis
//     so they slide along the moving terrain instead of teleporting),
//   - spot elevations at the audio-driven peaks,
//   - a static seeded clutter scatter of tiny map symbols.
//
// All coordinates are grid space [0 .. gridSize-1], matching
// TerrainModel::sampleComposedHeight and the letterbox math on the GL side.
namespace Labels
{

struct ContourLabel
{
    float gx = 0.0f, gy = 0.0f;   // anchor (label centre), grid space
    float angle = 0.0f;           // baseline direction, radians (screen y-down)
    int   level = 0;              // contour level k; elevation text = level*20
    float alpha = 0.0f;           // current fade
    float targetAlpha = 1.0f;     // 1 = alive, 0 = dying
};

struct SpotLabel
{
    float gx = 0.0f, gy = 0.0f;   // summit, grid space
    int   meters = 0;             // displayed elevation (deadbanded)
    float alpha = 0.0f;
    float targetAlpha = 0.0f;
};

struct ClutterMark
{
    float gx = 0.0f, gy = 0.0f;
    int   glyph = 0;              // GlyphAtlas id
    float phase = 0.0f;           // twinkle phase
    float baseAlpha = 0.05f;
};

class LabelPlanner
{
public:
    static constexpr int gridSize = TerrainModel::gridSize;

    // Index contour levels we label (every 7th level == index contour, skipping
    // the lowest which sits in the valley floor). Elevation = level*20 metres.
    static constexpr int labelLevels[8] = { 8, 15, 22, 29, 36, 43, 50, 57 };
    static constexpr int maxContourLabels = 14;

    const std::vector<ContourLabel>& getContourLabels() const noexcept { return contourLabels; }
    const std::vector<SpotLabel>&    getSpotLabels()    const noexcept { return spotLabels; }
    const std::vector<ClutterMark>&  getClutter()       const noexcept { return clutter; }

    // Wipe per-terrain labels on reseed (positions are meaningless on new terrain).
    void clearDynamic() noexcept { contourLabels.clear(); spotLabels.clear(); }

    //==============================================================================
    // Called EVERY frame: cheap Newton-projection of the existing labels back onto
    // their (moving) contour so they stay glued to the terrain instead of lagging
    // the ~3 Hz seeding pass. Snappy slew — the per-frame terrain delta is tiny, so
    // this reads as smooth tracking, not teleporting.
    void trackContourLabels (const TerrainModel& terrain, const RiverModel& river, float hMax)
    {
        const auto& mask = river.getPeakMask();
        auto sampleT = [&] (float x, float y)
        {
            const float h = terrain.sampleComposedHeight (x, y, mask);
            return 61.0f * std::pow (juce::jlimit (0.0f, 1.0f, h / juce::jmax (1e-4f, hMax)), 0.8f);
        };

        for (auto& lab : contourLabels)
        {
            float x = lab.gx, y = lab.gy;
            bool ok = reproject (sampleT, x, y, (float) lab.level);
            if (ok && passesSite (mask, x, y, sampleT))
            {
                lab.gx += (x - lab.gx) * 0.6f;    // snappy: kills the visible lag
                lab.gy += (y - lab.gy) * 0.6f;
                const float a = contourAngle (sampleT, lab.gx, lab.gy);
                lab.angle = slewAngle (lab.angle, a, 0.5f);
                lab.targetAlpha = 1.0f;
            }
            else
            {
                lab.targetAlpha = 0.0f;   // drift off its contour -> fade out
            }
        }
    }

    //==============================================================================
    // Called on the throttled planner tick (~every 20 frames): the expensive part,
    // seeding new labels on the index contours from a coarse lattice crossing scan.
    void seedContourLabels (const TerrainModel& terrain, const RiverModel& river, float hMax)
    {
        const auto& mask = river.getPeakMask();
        auto sampleT = [&] (float x, float y)
        {
            const float h = terrain.sampleComposedHeight (x, y, mask);
            return 61.0f * std::pow (juce::jlimit (0.0f, 1.0f, h / juce::jmax (1e-4f, hMax)), 0.8f);
        };

        constexpr int step = 3;                       // lattice sample spacing, cells
        constexpr float minSpacing = 42.0f;           // grid cells between labels
        for (int level : labelLevels)
        {
            int perLevel = 0;
            for (const auto& l : contourLabels)
                if (l.level == level && l.alpha > 0.05f) ++perLevel;

            for (int gy = 8; gy < gridSize - 8 && perLevel < 2; gy += step)
            {
                for (int gx = 8; gx < gridSize - 8 && perLevel < 2; gx += step)
                {
                    if ((int) contourLabels.size() >= maxContourLabels) break;

                    const float t00 = sampleT ((float) gx,        (float) gy);
                    const float t10 = sampleT ((float) (gx + step), (float) gy);
                    const float k   = (float) level;
                    // horizontal edge crossing of t == k
                    if ((t00 - k) * (t10 - k) >= 0.0f)
                        continue;
                    const float frac = (k - t00) / (t10 - t00);
                    float cx = (float) gx + frac * (float) step;
                    float cy = (float) gy;
                    if (! reproject (sampleT, cx, cy, k) || ! passesSite (mask, cx, cy, sampleT))
                        continue;
                    if (tooClose (cx, cy, minSpacing))
                        continue;

                    ContourLabel lab;
                    lab.gx = cx; lab.gy = cy; lab.level = level;
                    lab.angle = contourAngle (sampleT, cx, cy);
                    lab.alpha = 0.0f; lab.targetAlpha = 1.0f;
                    contourLabels.push_back (lab);
                    ++perLevel;
                }
            }
        }
    }

    //==============================================================================
    void updateSpotLabels (const TerrainModel& terrain, const RiverModel& river, float hMax)
    {
        const auto& mask  = river.getPeakMask();
        const auto& peaks = terrain.getPeaks();
        const auto& amps  = terrain.getPeakAmps();

        if ((int) spotLabels.size() != TerrainModel::numPeaks)
        {
            spotLabels.assign ((size_t) TerrainModel::numPeaks, SpotLabel());
            for (int pk = 0; pk < TerrainModel::numPeaks; ++pk)
                spotLabels[(size_t) pk].meters = -1000000;   // force first commit
        }

        for (int pk = 0; pk < TerrainModel::numPeaks; ++pk)
        {
            auto& s = spotLabels[(size_t) pk];
            float x = juce::jlimit (2.0f, (float) (gridSize - 3), peaks[(size_t) pk].nx * gridSize);
            float y = juce::jlimit (2.0f, (float) (gridSize - 3), peaks[(size_t) pk].ny * gridSize);

            // hill-climb to the local summit
            float bestH = terrain.sampleComposedHeight (x, y, mask);
            for (int it = 0; it < 14; ++it)
            {
                float bx = x, by = y, bh = bestH;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const float nx = juce::jlimit (0.0f, (float) (gridSize - 1), x + (float) dx);
                        const float ny = juce::jlimit (0.0f, (float) (gridSize - 1), y + (float) dy);
                        const float h  = terrain.sampleComposedHeight (nx, ny, mask);
                        if (h > bh) { bh = h; bx = nx; by = ny; }
                    }
                if (bx == x && by == y) break;
                x = bx; y = by; bestH = bh;
            }

            s.gx = x; s.gy = y;

            const int m = (int) std::lround (61.0f
                * std::pow (juce::jlimit (0.0f, 1.0f, bestH / juce::jmax (1e-4f, hMax)), 0.8f)
                * 20.0f / 5.0f) * 5;
            if (std::abs (m - s.meters) >= 10)   // 2-quantum deadband kills flicker
                s.meters = m;

            const float norm = amps[(size_t) pk] / TerrainModel::heightScale;
            s.targetAlpha = juce::jlimit (0.0f, 1.0f,
                                          juce::jmap (norm, 0.02f, 0.10f, 0.0f, 1.0f));
        }
    }

    //==============================================================================
    // Rebuilt only on reseed. dotGlyph/tickGlyph/crossGlyph are GlyphAtlas ids.
    void rebuildClutter (uint32_t seed, const RiverModel& river,
                         int dotGlyph, int tickGlyph, int crossGlyph)
    {
        clutter.clear();
        const auto& mask = river.getPeakMask();
        std::mt19937 rng (seed ^ 0x9E3779B9u);
        std::uniform_real_distribution<float> uni (0.0f, 1.0f);
        const int glyphs[3] = { dotGlyph, tickGlyph, crossGlyph };

        constexpr int target = 300;
        int attempts = 0;
        while ((int) clutter.size() < target && attempts < target * 6)
        {
            ++attempts;
            const float gx = 10.0f + uni (rng) * (float) (gridSize - 20);
            const float gy = 10.0f + uni (rng) * (float) (gridSize - 20);
            const int cell = (int) gy * gridSize + (int) gx;
            if ((int) mask.size() >= gridSize * gridSize && mask[(size_t) cell] < 0.9f)
                continue;
            ClutterMark m;
            m.gx = gx; m.gy = gy;
            m.glyph = glyphs[(int) (uni (rng) * 3.0f) % 3];
            m.phase = uni (rng) * juce::MathConstants<float>::twoPi;
            m.baseAlpha = 0.04f + uni (rng) * 0.05f;
            clutter.push_back (m);
        }
    }

    //==============================================================================
    // Per-frame alpha slew + reaping of fully-faded labels. dt in seconds.
    void advanceFades (float dt)
    {
        const float rate = juce::jmin (1.0f, dt * 3.5f);
        for (auto& l : contourLabels)
            l.alpha += (l.targetAlpha - l.alpha) * rate;
        for (auto& s : spotLabels)
            s.alpha += (s.targetAlpha - s.alpha) * rate;

        contourLabels.erase (std::remove_if (contourLabels.begin(), contourLabels.end(),
            [] (const ContourLabel& l) { return l.targetAlpha < 0.5f && l.alpha < 0.02f; }),
            contourLabels.end());
    }

private:
    // Newton-project (x,y) onto the level-k contour: step along grad(t) toward k.
    template <typename SampleT>
    static bool reproject (SampleT&& sampleT, float& x, float& y, float k)
    {
        for (int it = 0; it < 5; ++it)
        {
            const float t = sampleT (x, y);
            float gx, gy;
            gradT (sampleT, x, y, gx, gy);
            const float g2 = gx * gx + gy * gy;
            if (g2 < 1e-8f)
                return false;
            const float d = (t - k) / g2;
            x = juce::jlimit (1.0f, (float) (gridSize - 2), x - d * gx);
            y = juce::jlimit (1.0f, (float) (gridSize - 2), y - d * gy);
        }
        return std::abs (sampleT (x, y) - k) < 0.3f;
    }

    template <typename SampleT>
    static void gradT (SampleT&& sampleT, float x, float y, float& gx, float& gy)
    {
        gx = (sampleT (x + 1.0f, y) - sampleT (x - 1.0f, y)) * 0.5f;
        gy = (sampleT (x, y + 1.0f) - sampleT (x, y - 1.0f)) * 0.5f;
    }

    // Label baseline = contour tangent = perpendicular to grad(t), flipped to keep
    // text upright (advance pointing generally rightward).
    template <typename SampleT>
    static float contourAngle (SampleT&& sampleT, float x, float y)
    {
        float gx, gy;
        gradT (sampleT, x, y, gx, gy);
        float a = std::atan2 (gx, -gy);   // perp(grad) in screen y-down space
        if (a > juce::MathConstants<float>::halfPi)  a -= juce::MathConstants<float>::pi;
        if (a < -juce::MathConstants<float>::halfPi) a += juce::MathConstants<float>::pi;
        return a;
    }

    static float slewAngle (float cur, float target, float rate)
    {
        float d = target - cur;
        while (d >  juce::MathConstants<float>::halfPi) d -= juce::MathConstants<float>::pi;
        while (d < -juce::MathConstants<float>::halfPi) d += juce::MathConstants<float>::pi;
        return cur + d * rate;
    }

    // River / plateau / straightness rejection for a candidate site.
    template <typename SampleT>
    static bool passesSite (const std::vector<float>& mask, float x, float y, SampleT&& sampleT)
    {
        const int cell = juce::jlimit (0, gridSize * gridSize - 1,
                                       (int) y * gridSize + (int) x);
        if ((int) mask.size() >= gridSize * gridSize && mask[(size_t) cell] < 0.85f)
            return false;   // too near the river

        float gx, gy;
        gradT (sampleT, x, y, gx, gy);
        const float g = std::sqrt (gx * gx + gy * gy);
        if (g < 0.03f)
            return false;   // plateau: contour would smear across the label

        // straightness: t along the tangent must stay near the level over the label
        const float k = std::floor (sampleT (x, y) + 0.5f);
        const float tx = -gy / g, ty = gx / g;
        for (float s : { -4.0f, 4.0f })
            if (std::abs (sampleT (x + tx * s, y + ty * s) - k) > 0.6f)
                return false;
        return true;
    }

    bool tooClose (float x, float y, float minDist) const
    {
        const float m2 = minDist * minDist;
        for (const auto& l : contourLabels)
            if (l.alpha > 0.05f || l.targetAlpha > 0.5f)
            {
                const float dx = l.gx - x, dy = l.gy - y;
                if (dx * dx + dy * dy < m2)
                    return true;
            }
        return false;
    }

    std::vector<ContourLabel> contourLabels;
    std::vector<SpotLabel>    spotLabels;
    std::vector<ClutterMark>  clutter;
};

} // namespace Labels
