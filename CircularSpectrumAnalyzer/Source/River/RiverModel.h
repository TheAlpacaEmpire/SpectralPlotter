#pragma once

#include <JuceHeader.h>

#include <functional>

#include "../Audio/AudioAnalyzer.h"   // AudioFeatures

// Traces a river backbone through the terrain's base height field, carves a permanent
// valley channel along it, and animates the river's per-frame sway/glow/depression.
// Shares terrain's height fields by reference rather than owning them, since the river
// both reads terrain height (to trace/sample) and writes into it (to carve the valley).
class RiverModel
{
public:
    explicit RiverModel (int gridSize);

    // Traces the backbone via gradient descent over the static base terrain — call once
    // at startup and again after each reseed, before carveChannel(). seed ties the river's
    // exit-point randomization to the same terrain seed so the whole scene is reproducible.
    void buildPath (const std::vector<float>& baseHeightField, uint32_t seed);

    // Depresses baseHeightField along the backbone and builds the peak-suppression mask.
    // Call once, immediately after buildPath().
    void carveChannel (std::vector<float>& baseHeightField);

    // Resets riverPhase/riverFlash/prevAvgEnergy — call on reseed, after buildPath()+carveChannel().
    void resetAnimationState() noexcept { riverPhase = 0.0f; riverFlash = 0.0f; prevAvgEnergy = 0.0f; }

    // Per-frame: sway, distance field, and the dynamic-depression parameters. Samples the
    // composed terrain height through sampleHeight (grid-space x,y -> height, matching this
    // frame's TerrainModel state) to gauge valley openness. The depression itself is applied
    // by the GPU height-compose pass from getMinDist2()/getDynDepth()/getSigma().
    // Consumes the AudioFeatures snapshot (overall + bass energy for flow/width, per-band
    // level for sway, percussive flux for the flash).
    void update (const AudioFeatures& feats,
                 const std::function<float (float, float)>& sampleHeight,
                 int viewWidth, int viewHeight);

    const std::vector<juce::Point<float>>& getSwayedPoints() const noexcept { return riverSwayedPts; }
    const std::vector<float>&              getBankWidths()   const noexcept { return riverBankWidths; }
    const std::vector<float>&              getPeakMask()     const noexcept { return riverPeakMask; }
    float getPhase() const noexcept { return riverPhase; }
    float getFlash() const noexcept { return riverFlash; }

    // Inputs of the GPU valley-depression term: h -= dynDepth * exp(-d2 / (2*sigma^2)),
    // cut off at 9*sigma^2. Valid after update().
    const std::vector<float>& getMinDist2() const noexcept { return riverMinDist2; }
    float getDynDepth() const noexcept { return lastDynDepth; }
    float getSigma()    const noexcept { return lastSigma; }

private:
    const int gridSize;
    static constexpr int riverMaxSteps = 450;   // gradient-descent steps per river

    std::vector<juce::Point<float>> riverGridPts;   // stable river backbone in grid space, built once
    std::vector<juce::Point<float>> riverSwayedPts; // swayed river positions in grid space, updated each frame
    std::vector<float>              riverMinDist2;  // per-cell min squared distance to river backbone (recomputed each frame)
    std::vector<float>              riverTangX;           // per-backbone-point tangent X, preallocated to avoid per-frame heap alloc
    std::vector<float>              riverTangY;           // per-backbone-point tangent Y
    std::vector<float>              riverValleyOpenness;  // per-backbone-point valley openness (0=gorge, 1=open plain)
    std::vector<float>              riverBankWidths;      // per-backbone-point bank half-width in screen pixels
    std::vector<float>              riverPeakMask;        // precomputed [0,1] fade; 0 at river bed, 1 at valley shoulder — prevents peaks overriding the valley

    float riverPhase    = 0.0f;   // advances each frame — drives lateral sway and glint travel
    float riverFlash    = 0.0f;   // decaying brightness boost triggered by energy transients
    float prevAvgEnergy = 0.0f;   // last frame's average energy for transient detection
    float lastDynDepth  = 0.0f;   // this frame's dynamic depression depth (GPU compose input)
    float lastSigma     = 1.0f;   // this frame's depression Gaussian sigma (GPU compose input)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RiverModel)
};
