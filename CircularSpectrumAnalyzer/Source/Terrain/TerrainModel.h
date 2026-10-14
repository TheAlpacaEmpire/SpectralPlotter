#pragma once

#include <JuceHeader.h>

#include "../Audio/AudioAnalyzer.h"   // AudioFeatures

// Procedurally-placed audio-reactive terrain: a set of elliptical Gaussian peaks over a
// height field, each peak driven by one frequency band. Owns the height field, the peak
// weight textures, and all per-frame breathing/drift/flash animation for the peaks.
class TerrainModel
{
public:
    static constexpr int gridSize = 256;   // height field resolution
    static constexpr int numPeaks = 10;    // audio-driven terrain peaks

    struct TerrainPeak
    {
        float nx, ny, sigma, angle, stretch;
        int   bandStart, bandEnd;
        float anchorNx, anchorNy;
    };

    TerrainModel();

    // Picks a new random seed, re-places peaks, resets terrain animation state, and
    // rebuilds the static weight/base-height textures from scratch.
    void reseed (uint32_t newSeed);

    // Per-frame update: audio-activity envelope, ridge-flow drift, per-peak spatial
    // drift + staggered spatial rebuild, periodic weight rebuild, transient flash,
    // and this frame's per-peak amplitudes. The full-field composition
    // (base + amp*weight*mask) happens on the GPU (SceneRenderer::composeHeightField);
    // point queries go through sampleComposedHeight().
    // Consumes the rich AudioFeatures snapshot: harmonic drives sustained elevation,
    // percussive flux drives the transient flash, and per-peak brightness (spectral tilt
    // within the peak's band slice) drives ridge roughness.
    // animTime is the CURRENT frame's clock value, taken BEFORE the caller advances it —
    // matches the original single-file ordering where peak drift used the pre-increment
    // clock and the periodic weight rebuild used the post-increment clock.
    void update (const AudioFeatures& feats,
                 const std::vector<float>& riverPeakMask, float animTime);

    const std::vector<float>& getBaseHeightField() const noexcept { return baseHeightField; }
    std::vector<float>&       getBaseHeightFieldMutable() noexcept { return baseHeightField; }

    // Bilinear sample of the composed field (base + mask * pNorm3(amp*weight)) at a
    // grid-space point — the CPU-side stand-in for the GPU-composed height texture.
    // Peaks combine as a cube-root-of-sum-of-cubes soft max (not a plain sum) so two
    // overlapping active peaks keep a saddle between them instead of fusing.
    // Matches the old full-field composition exactly; it deliberately excludes the
    // river's dynamic depression, which the old sampling never saw either (the
    // depression was applied after RiverModel read the field).
    float sampleComposedHeight (float x, float y,
                                const std::vector<float>& riverPeakMask) const noexcept;

    // This frame's per-peak amplitudes, premultiplied by heightScale — the uAmp[]
    // uniforms of the GPU compose pass. Valid after update().
    const std::array<float, numPeaks>& getPeakAmps() const noexcept { return peakAmps; }

    // Weight plane for one peak (gridSize^2 floats), for atlas uploads.
    const float* getPeakPlane (int pk) const noexcept
    {
        return peakWeights.data() + (size_t) pk * gridSize * gridSize;
    }

    // Bit pk set => that peak's weight plane changed since the last call.
    uint32_t fetchAndClearDirtyPlaneBits() noexcept
    {
        const auto bits = dirtyPlaneBits;
        dirtyPlaneBits = 0;
        return bits;
    }

    const std::array<TerrainPeak, numPeaks>& getPeaks() const noexcept { return terrainPeaks; }
    float    getHeightFieldMax()  const noexcept { return heightFieldMax; }
    uint32_t getSeed()            const noexcept { return terrainSeed; }

    static constexpr float heightScale = 2.1f;   // harmonic energy -> relief ("Responsive" feel)

private:
    static constexpr float dt = 1.0f / 60.0f;

    // Harmonic-level -> Gaussian sigma growth. Shared by BOTH weight-rebuild paths
    // (rebuildPeakWeights, every 4 frames for all peaks; rebuildOnePeak, one peak
    // per frame) so a peak's ring radius can't jitter between two different widths
    // depending on which path last touched it.
    // Kept modest so loudness reads as HEIGHT, not as a footprint that swells into
    // the neighbouring peaks (wide loud peaks were the main cause of central blobs).
    static constexpr float sigmaLevelGain   = 0.25f;
    static constexpr float stretchLevelGain = 0.20f;   // same rule for elongation

    void placePeaks();              // best-candidate peak placement, shared by ctor path and reseed()
    void buildTerrainWeights();      // one-time (per-seed) weight + base-height-field precompute
    void erodeBaseHeight();          // one-time (per-seed) thermal + hydraulic erosion of baseHeightField
    void rebuildPeakWeights (const AudioFeatures& feats, float animTime);       // all peaks — every 4th update()
    void rebuildOnePeak (int pk, const AudioFeatures& feats, float animTime);   // one peak — one per update()
    float shapePeakLevel (float x) const;    // soft-knee expander + soft top for normalised levels

    // Raw sustained (harmonic) level for a peak: max harmonic energy over its band slice.
    float peakRawLevel (const AudioFeatures& feats, const TerrainPeak& p) const;

    // Per-frame peak drives (peakDrive[]): auto-gain window -> soft-knee shaping ->
    // neighbour inhibition. Every consumer of a peak's audio level reads peakDrive.
    void computePeakDrives (const AudioFeatures& feats);
    // 0..1 spectral tilt within a peak's slice (low = dark/broad ridges, high = bright/fine ridges).
    float peakBrightness (const AudioFeatures& feats, const TerrainPeak& p) const;

    std::array<TerrainPeak, numPeaks> terrainPeaks;
    std::vector<float> peakWeights;      // [numPeaks × gridSize × gridSize] rebuilt every 4 frames
    std::vector<float> baseHeightField;  // static background terrain, precomputed (carved by RiverModel)
    std::vector<float> peakWRX;          // signed wrx per (peak,cell) — rebuilt each cycle
    std::vector<float> peakWRY;          // signed wry per (peak,cell) — rebuilt each cycle
    std::vector<float> peakRidge;        // ridge value per (peak,cell) — rebuilt each cycle

    // Animation state
    float heightFieldMax      = 1.0f;   // smoothed ceiling for dynamic contour range
    float heightFieldSustain  = 1.0f;   // slow ceiling proxy driven by sustained (non-flash) peak amplitude only
    std::array<float, numPeaks> peakPhaseOffset {};   // fixed at construction, never reset on reseed
    int   rebuildCounter = 0;
    float audioActivity  = 0.0f;   // smoothed 0..1 overall-energy envelope; gates idle motion

    // Ridge-flow offset (slowly drifts ridge texture across each peak)
    float ridgeFlowX = 0.0f;
    float ridgeFlowY = 0.0f;

    // Per-peak position drift
    struct PeakVelocity { float vx = 0.0f, vy = 0.0f; };
    std::array<PeakVelocity, numPeaks> peakVelocity {};
    std::array<float, numPeaks> peakFlash     {};
    std::array<float, numPeaks> peakFlashTail {};  // slow-decaying glow component of the two-stage flash decay
    std::array<float, numPeaks> peakAmps      {};  // this frame's amp * heightScale (GPU uAmp[])
    std::array<float, numPeaks> peakDrive     {};  // this frame's shaped, inhibited 0..~1 level

    // Auto-gain reference: slow tracker of the loudest peak's raw level. Persists
    // across reseeds (it describes the audio, not the terrain).
    float loudRef = 0.6f;
    uint32_t dirtyPlaneBits = 0;                   // weight planes changed since last fetch
    int peakSpatialIndex = 0;   // which peak gets its spatial data rebuilt this frame

    uint32_t terrainSeed = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TerrainModel)
};
