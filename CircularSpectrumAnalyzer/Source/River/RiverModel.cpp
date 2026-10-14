#include "RiverModel.h"

#include <random>

//==============================================================================
RiverModel::RiverModel (int gridSizeIn)
    : gridSize (gridSizeIn)
{
}

//==============================================================================
// Traces a river path through the static base terrain using gradient descent with
// high momentum so the path is smooth. Stored in grid space; animated in update().
void RiverModel::buildPath (const std::vector<float>& baseHeightField, uint32_t seed)
{
    riverGridPts.clear();

    auto sampleBase = [&] (float x, float y) -> float
    {
        const int ix = juce::jlimit (0, gridSize - 2, (int) x);
        const int iy = juce::jlimit (0, gridSize - 2, (int) y);
        const float fx = x - (float) ix, fy = y - (float) iy;
        return baseHeightField[ iy       * gridSize + ix    ] * (1 - fx) * (1 - fy)
             + baseHeightField[ iy       * gridSize + ix + 1] *      fx  * (1 - fy)
             + baseHeightField[(iy + 1)  * gridSize + ix    ] * (1 - fx) *      fy
             + baseHeightField[(iy + 1)  * gridSize + ix + 1] *      fx  *      fy;
    };

    // Find the highest point in the top third — with the N→S slope baked in, this
    // reliably lands near the north edge where the river should originate.
    float bestH = -1.0f;
    float bestX = (float) gridSize * 0.5f, bestY = (float) gridSize * 0.1f;
    for (int gy = 0; gy < gridSize / 3; gy += 10)
    for (int gx = gridSize / 8; gx < gridSize * 7 / 8; gx += 10)
    {
        const float h = baseHeightField[gy * gridSize + gx];
        if (h > bestH) { bestH = h; bestX = (float) gx; bestY = (float) gy; }
    }

    // Exit point varies slightly per launch so different rivers converge to different
    // south-edge positions rather than always centering on x=0.5.
    std::mt19937 riverRng (seed ^ 0x7A1E0000u);
    std::uniform_real_distribution<float> exitDist (0.33f, 0.67f);
    const float fixedExitX = (float) gridSize * exitDist (riverRng);
    const float fixedExitY = (float) (gridSize + 20);

    // Start 20 grid units above the visible area so the river enters from offscreen
    float px = bestX, py = -20.0f;
    float flowX = 0.0f, flowY = 0.0f;
    bool  initSet = false;

    riverGridPts.push_back ({ px, py });

    for (int step = 0; step < riverMaxSteps; ++step)
    {
        const float gdx  = sampleBase (px + 0.8f, py) - sampleBase (px - 0.8f, py);
        const float gdy  = sampleBase (px, py + 0.8f) - sampleBase (px, py - 0.8f);
        const float glen = std::hypot (gdx, gdy);

        float dhX = 0.0f, dhY = 1.0f;
        if (glen >= 4e-4f) { dhX = -gdx / glen; dhY = -gdy / glen; }
        // Clamp BEFORE renormalizing: ensures dhY is never northward.
        // A post-blend normalize would cancel any additive bias, so we must
        // floor the raw gradient component here, then renormalize.
        dhY = juce::jmax (0.3f, dhY);   // minimum southward component
        {
            const float dhLen = std::hypot (dhX, dhY);
            if (dhLen > 1e-5f) { dhX /= dhLen; dhY /= dhLen; }
        }
        if (! initSet) { flowX = dhX; flowY = dhY; initSet = true; }

        // In the bottom quarter, blend terrain gradient toward the fixed exit so the
        // river converges naturally rather than snapping at the last step
        if (py > (float) gridSize * 0.75f)
        {
            const float t     = juce::jlimit (0.0f, 1.0f,
                                    (py - (float) gridSize * 0.75f) / ((float) gridSize * 0.25f));
            const float toX   = fixedExitX - px;
            const float toY   = fixedExitY - py;
            const float toLen = std::hypot (toX, toY);
            if (toLen > 1e-5f)
            {
                dhX = dhX * (1.0f - t) + (toX / toLen) * t;
                dhY = dhY * (1.0f - t) + (toY / toLen) * t;
                const float dlen = std::hypot (dhX, dhY);
                if (dlen > 1e-5f) { dhX /= dlen; dhY /= dlen; }
            }
        }

        // High momentum = smooth non-chaotic path
        flowX = flowX * 0.85f + dhX * 0.15f;
        flowY = flowY * 0.85f + dhY * 0.15f;
        const float flen = std::hypot (flowX, flowY);
        if (flen > 1e-5f) { flowX /= flen; flowY /= flen; }

        px += flowX * 2.0f;
        py += flowY * 2.0f;

        // Soft lateral repulsion — nudges flow direction away from edges rather than hard-clamping,
        // which would create straight kinks along the boundary.
        // Renormalize after adding repulsion so the unit-length invariant is preserved;
        // without this the inflated flowX magnitude carries forward and causes lateral lurches.
        constexpr float edgeMargin = 15.0f;
        if (px < edgeMargin)                      flowX += (edgeMargin - px) * 0.04f;
        if (px > (float) gridSize - edgeMargin)   flowX -= (px - ((float) gridSize - edgeMargin)) * 0.04f;
        {
            const float rlen = std::hypot (flowX, flowY);
            if (rlen > 1e-5f) { flowX /= rlen; flowY /= rlen; }
        }
        px = juce::jlimit (1.0f, (float) (gridSize - 2), px);
        if (py >= fixedExitY)
        {
            riverGridPts.push_back ({ fixedExitX, fixedExitY });
            break;
        }

        riverGridPts.push_back ({ px, py });
    }

    riverTangX.resize (riverGridPts.size());
    riverTangY.resize (riverGridPts.size());
}

//==============================================================================
void RiverModel::carveChannel (std::vector<float>& baseHeightField)
{
    // Two-component valley: narrow deep floor (river bed) + wide gentle shoulder (valley walls).
    // The shoulder sigma=18 places the Gaussian's steepest slope at r=18, well outside the
    // clip boundary (~6 grid units), so valley-wall contours are tightly packed and visible.
    constexpr float floorSigma    = 2.5f;
    constexpr float shoulderSigma = 12.0f;
    const float     searchR       = shoulderSigma * 3.0f;   // 54 grid units — one-time cost
    const float     floorSigma2   = floorSigma    * floorSigma;
    const float     shoulderSigma2 = shoulderSigma * shoulderSigma;
    const float     cutoff2        = 9.0f * shoulderSigma2;

    // Build per-cell min-distance to the static backbone (local — not stored as member
    // since update() overwrites riverMinDist2 from swayed positions every frame).
    std::vector<float> minDist2 (gridSize * gridSize, 1e18f);

    for (const auto& pt : riverGridPts)
    {
        if (pt.x < 0 || pt.x >= (float) gridSize || pt.y < 0 || pt.y >= (float) gridSize)
            continue;

        const int x0 = (int) juce::jmax (0.0f, pt.x - searchR);
        const int x1 = (int) juce::jmin ((float) (gridSize - 1), pt.x + searchR);
        const int y0 = (int) juce::jmax (0.0f, pt.y - searchR);
        const int y1 = (int) juce::jmin ((float) (gridSize - 1), pt.y + searchR);

        for (int gy = y0; gy <= y1; ++gy)
        for (int gx = x0; gx <= x1; ++gx)
        {
            const float dx = (float) gx - pt.x;
            const float dy = (float) gy - pt.y;
            const float d2 = dx * dx + dy * dy;
            auto& md = minDist2[gy * gridSize + gx];
            if (d2 < md) md = d2;
        }
    }

    // Carve a permanent baseline valley into baseHeightField so a low-point exists
    // along the backbone even at silence, before any per-frame valley is applied.
    for (int i = 0, N = gridSize * gridSize; i < N; ++i)
    {
        const float r2 = minDist2[i];
        if (r2 >= cutoff2) continue;
        baseHeightField[i] -= 0.10f * std::exp (-r2 / (2.0f * floorSigma2))
                            + 0.05f * std::exp (-r2 / (2.0f * shoulderSigma2));
    }

    // Build peak-suppression mask from the same distance field.
    // Peaks taper to zero at the river bed and recover to full weight at the carved shoulder,
    // so loud audio peaks can never raise a ridge through the valley floor.
    riverPeakMask.assign (gridSize * gridSize, 1.0f);
    for (int i = 0, N = gridSize * gridSize; i < N; ++i)
    {
        const float r = std::sqrt (minDist2[i]);
        riverPeakMask[i] = juce::jlimit (0.0f, 1.0f, (r - 4.0f) / (12.0f - 4.0f));
    }
}

//==============================================================================
void RiverModel::update (const AudioFeatures& feats,
                         const std::function<float (float, float)>& sampleHeight,
                         int viewWidth, int viewHeight)
{
    constexpr int numBands = AudioFeatures::numBands;
    const float riverAvgE  = feats.overallEnergy;
    const float riverBassE = feats.bassEnergy;

    // Strongest percussive onset across the spectrum — gives the river flash a crisp,
    // beat-locked pulse instead of reacting to the slow average-energy derivative alone.
    float percOnset = 0.0f;
    for (int b = 0; b < numBands; ++b)
        percOnset = juce::jmax (percOnset, feats.flux[b]);

    // Compute scale the same way paint() does so grid units match screen units.
    const float scale   = juce::jmin ((float) viewWidth  / (float) (gridSize - 1),
                                      (float) viewHeight / (float) (gridSize - 1));
    // clipBase mirrors the base audio-reactive width used by paint()'s clip mask.
    const float clipBase  = juce::jmax (2.0f, (4.0f + riverBassE * 14.0f) / scale);
    // Sigma = 1× clipBase keeps the Gaussian tight so the falloff at the original backbone
    // is steep enough (≈60% at sway=3 units) that contour lines visibly shift with sway.
    // Wider sigmas (1.8× was prior value) cause both backbone and swayed positions to look
    // nearly identical to the marching-squares pass, especially in low-sway gorge sections.
    const float sigma     = clipBase * 1.0f;
    // Dynamic search radius always covers the full 3σ Gaussian tail.
    const float searchR = std::ceil (3.0f * sigma);

    riverPhase += 0.015f + riverAvgE * 0.04f;
    riverPhase = std::fmod (riverPhase, juce::MathConstants<float>::twoPi);
    const float delta = riverAvgE - prevAvgEnergy;
    riverFlash = juce::jmax (riverFlash, juce::jmax (delta * 4.0f, percOnset * 2.0f));
    riverFlash *= 0.92f;
    prevAvgEnergy = riverAvgE;

    // Compute swayed positions in grid space using per-band amplitude.
    const int nPts = (int) riverGridPts.size();
    riverSwayedPts.resize (nPts);

    // Grid-space tangents — reuse preallocated member vectors (no per-frame heap alloc)
    auto& gtx = riverTangX;
    auto& gty = riverTangY;
    for (int i = 0; i < nPts; ++i)
    {
        const int i0 = juce::jmax (0, i - 1);
        const int i1 = juce::jmin (nPts - 1, i + 1);
        float tx = riverGridPts[i1].x - riverGridPts[i0].x;
        float ty = riverGridPts[i1].y - riverGridPts[i0].y;
        const float tlen = std::hypot (tx, ty);
        if (tlen > 1e-5f) { tx /= tlen; ty /= tlen; }
        gtx[i] = tx;  gty[i] = ty;
    }

    // Per-point valley openness: sample heightField perpendicularly to flow direction.
    // High cross-slope (terrain rises steeply on both sides) → gorge → low openness.
    // Audio-driven terrain peaks near the river dynamically narrow the channel.
    riverValleyOpenness.resize (nPts);
    riverBankWidths.resize (nPts);

    const auto& sampleHF = sampleHeight;   // composed terrain height, grid space

    constexpr float perpSampleDist = 20.0f;
    for (int i = 0; i < nPts; ++i)
    {
        // Sample at swayed position (previous frame, imperceptible lag at 60 fps)
        // so gorge/openness responds to where the river actually is this frame
        const float px  = riverSwayedPts[i].x;
        const float py  = riverSwayedPts[i].y;
        const float nx  = -gty[i];   // perpendicular to flow
        const float ny  =  gtx[i];
        const float hL  = sampleHF (px + nx * perpSampleDist, py + ny * perpSampleDist);
        const float hR  = sampleHF (px - nx * perpSampleDist, py - ny * perpSampleDist);
        const float hC  = sampleHF (px, py);
        const float crossRise      = juce::jmax (0.0f, (hL + hR) * 0.5f - hC);
        riverValleyOpenness[i]     = 1.0f / (1.0f + crossRise * 10.0f);
        riverBankWidths[i]         = (4.0f + riverBassE * 14.0f);
    }

    for (int i = 0; i < nPts; ++i)
    {
        const float s    = static_cast<float> (i) / static_cast<float> (nPts);
        const int   band = juce::jlimit (0, numBands - 1, (int) (s * (float) numBands));
        // Clamp per-point so sway cannot push the backbone outside the grid.
        const float distToEdge = juce::jmin (
            juce::jmin (riverGridPts[i].x, (float)(gridSize - 1) - riverGridPts[i].x),
            juce::jmin (riverGridPts[i].y, (float)(gridSize - 1) - riverGridPts[i].y));
        // Gorge constraint: in narrow valleys, meander amplitude is bounded by valley walls.
        const float valleySwayMax = 12.0f * riverValleyOpenness[i];
        const float swayAmt = juce::jmin (juce::jmin (feats.level[band] * 10.0f,
                                                       juce::jmax (0.0f, distToEdge * 0.85f)),
                                          valleySwayMax);
        const float sway    = std::sin (riverPhase + s * juce::MathConstants<float>::twoPi * 1.5f);
        const float ox = -gty[i] * sway * swayAmt;
        const float oy =  gtx[i] * sway * swayAmt;
        riverSwayedPts[i] = { riverGridPts[i].x + ox, riverGridPts[i].y + oy };
    }

    // Recompute riverMinDist2 from swayed positions — searchR covers the full Gaussian.
    // Clamp each point onto the grid before distance tests so that off-screen backbone
    // points (y < 0 at the river source) still populate the top-row cells — fixes the
    // contour clip gap that let contours poke through the river polygon at its entry.
    riverMinDist2.assign (gridSize * gridSize, 1e18f);
    for (auto pt : riverSwayedPts)
    {
        pt.x = juce::jlimit (0.0f, (float) (gridSize - 1), pt.x);
        pt.y = juce::jlimit (0.0f, (float) (gridSize - 1), pt.y);
        const int x0 = (int) juce::jmax (0.0f, pt.x - searchR);
        const int x1 = (int) juce::jmin ((float) (gridSize - 1), pt.x + searchR);
        const int y0 = (int) juce::jmax (0.0f, pt.y - searchR);
        const int y1 = (int) juce::jmin ((float) (gridSize - 1), pt.y + searchR);
        for (int gy = y0; gy <= y1; ++gy)
        for (int gx = x0; gx <= x1; ++gx)
        {
            const float dx = (float) gx - pt.x;
            const float dy = (float) gy - pt.y;
            const float d2 = dx * dx + dy * dy;
            auto& md = riverMinDist2[gy * gridSize + gx];
            if (d2 < md) md = d2;
        }
    }

    // Dynamic valley depression parameters — the subtraction itself
    // (h -= dynDepth * exp(-r2 / (2*sigma^2)), gated at 9*sigma^2) runs in the
    // GPU height-compose pass, fed by riverMinDist2 + these two scalars.
    // Raised base depth (0.30 vs old 0.14) compensates for the narrower sigma footprint,
    // keeping the valley clearly visible in contours even at silence.
    lastDynDepth = 0.30f + riverBassE * 0.30f + riverAvgE * 0.16f + riverFlash * 0.14f;
    lastSigma    = sigma;
}
