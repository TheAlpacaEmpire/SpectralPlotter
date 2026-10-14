#include "TerrainModel.h"
#include "NoiseUtils.h"
#include "../Audio/BandGroups.h"

#include <random>

//==============================================================================
TerrainModel::TerrainModel()
{
    for (int pk = 0; pk < numPeaks; ++pk)
        peakPhaseOffset[pk] = (float) pk * juce::MathConstants<float>::twoPi / (float) numPeaks;
}

//==============================================================================
// Procedurally place 10 elliptical Gaussian peaks with best-candidate (Mitchell)
// sampling: each peak takes the best of N random candidates, scored by clearance
// from the peaks already placed. Unlike rejection sampling this never falls back to
// stacking peaks, and it fills the whole map instead of the central diagonal.
// Band assignments are fixed to preserve full-spectrum audio reactivity.
void TerrainModel::placePeaks()
{
    std::mt19937 peakRng (terrainSeed ^ 0xC0FFEE42u);
    std::uniform_real_distribution<float> posDist    (0.12f, 0.88f);
    std::uniform_real_distribution<float> sigmaDist  (10.0f, 26.0f);
    std::uniform_real_distribution<float> angleDist  (0.0f, juce::MathConstants<float>::pi);
    std::uniform_real_distribution<float> stretchDist(1.0f, 2.6f);   // was 3.5: long ridges bridged neighbours

    // Musically-grouped frequency ranges: each peak covers a band of related instruments
    // (see BandGroups.h — the table is shared with the ripple system).
    static_assert (BandGroups::numGroups == numPeaks, "one peak per band group");
    const int* bandStarts = BandGroups::starts;
    const int* bandEnds   = BandGroups::ends;
    constexpr int   candidates   = 32;
    constexpr float freqNbrScale = 0.7f;   // adjacent-band peaks need ~1.4x the clearance
    constexpr float biasWeight   = 0.30f;  // pull toward the bass -> treble gradient

    for (int pk = 0; pk < numPeaks; ++pk)
    {
        // Gentle bias toward a bass (bottom-left) -> treble (top-right) spatial gradient,
        // as a soft score term so placement still looks organic/random.
        const float freqFrac = (float) pk / (float) (numPeaks - 1);
        const float biasCx   = juce::jmap (freqFrac, 0.25f, 0.75f);
        const float biasCy   = juce::jmap (freqFrac, 0.75f, 0.25f);

        float nx = 0.5f, ny = 0.5f, bestScore = -1.0e9f;
        for (int c = 0; c < candidates; ++c)
        {
            const float cx = posDist (peakRng), cy = posDist (peakRng);

            // Clearance to placed peaks. Neighbouring frequency slices light up together
            // for broadband sounds (one instrument bleeding across slices), so their
            // distances are discounted — they get pushed further apart than the rest.
            float clear = 1.0f;
            for (int j = 0; j < pk; ++j)
            {
                float d = std::hypot (cx - terrainPeaks[j].nx, cy - terrainPeaks[j].ny);
                if (std::abs (pk - j) == 1) d *= freqNbrScale;
                clear = juce::jmin (clear, d);
            }
            const float score = clear - biasWeight * std::hypot (cx - biasCx, cy - biasCy);
            if (score > bestScore) { bestScore = score; nx = cx; ny = cy; }
        }
        terrainPeaks[pk] = { nx, ny, sigmaDist (peakRng),
                             angleDist (peakRng), stretchDist (peakRng),
                             bandStarts[pk], bandEnds[pk], nx, ny };
    }
}

//==============================================================================
void TerrainModel::reseed (uint32_t newSeed)
{
    terrainSeed = newSeed;
    placePeaks();

    // Reset drifting animation state so the new terrain starts clean
    ridgeFlowX = 0.0f;  ridgeFlowY = 0.0f;
    peakVelocity   = {};
    peakFlash      = {};
    peakFlashTail  = {};
    heightFieldMax     = 1.0f;
    heightFieldSustain = 1.0f;
    audioActivity  = 0.0f;
    peakSpatialIndex = 0;
    rebuildCounter   = 0;
    peakAmps         = {};
    dirtyPlaneBits   = (1u << numPeaks) - 1u;   // all planes rebuilt from scratch

    buildTerrainWeights();
}

//==============================================================================
float TerrainModel::sampleComposedHeight (float x, float y,
                                          const std::vector<float>& riverPeakMask) const noexcept
{
    const int ix = juce::jlimit (0, gridSize - 2, (int) x);
    const int iy = juce::jlimit (0, gridSize - 2, (int) y);
    const float fx = x - (float) ix, fy = y - (float) iy;

    const int i00 =  iy      * gridSize + ix;
    const int i10 = i00 + 1;
    const int i01 = (iy + 1) * gridSize + ix;
    const int i11 = i01 + 1;

    // Peaks combine per vertex as a p=3 soft max (cube root of the sum of cubes), exactly
    // as the GPU compose pass does; the river mask is a common factor so it applies after.
    float c00 = 0.0f, c10 = 0.0f, c01 = 0.0f, c11 = 0.0f;
    for (int pk = 0; pk < numPeaks; ++pk)
    {
        const float amp = peakAmps[pk];
        if (amp <= 0.0f)
            continue;
        const float* w = peakWeights.data() + (size_t) pk * gridSize * gridSize;
        const float a = amp * w[i00], b = amp * w[i10], c = amp * w[i01], d = amp * w[i11];
        c00 += a * a * a;  c10 += b * b * b;  c01 += c * c * c;  c11 += d * d * d;
    }

    const bool masked = (int) riverPeakMask.size() >= gridSize * gridSize;
    auto peakSum = [&] (float cubes, int i)
    {
        return (cubes > 0.0f ? std::cbrt (cubes) : 0.0f) * (masked ? riverPeakMask[i] : 1.0f);
    };
    const float h00 = baseHeightField[i00] + peakSum (c00, i00);
    const float h10 = baseHeightField[i10] + peakSum (c10, i10);
    const float h01 = baseHeightField[i01] + peakSum (c01, i01);
    const float h11 = baseHeightField[i11] + peakSum (c11, i11);

    return h00 * (1.0f - fx) * (1.0f - fy) + h10 * fx * (1.0f - fy)
         + h01 * (1.0f - fx) * fy          + h11 * fx * fy;
}

//==============================================================================
// Shaping curve for an auto-gained 0..~1.5 level. Replaces the old hard noise-floor
// cut + gamma 1.8 + clamp, which flattened both ends:
//  - bottom: x^2/(x+k) is a soft-knee expander — quiet material fades in smoothly
//    (~x^2/k near zero) instead of being chopped at a threshold, then turns linear.
//  - top: above 0.7 a tanh soft clip takes over (C1-continuous at the knee), so loud
//    peaks keep growing a little instead of hitting a flat-topped ceiling at 1.
float TerrainModel::shapePeakLevel (float x) const
{
    constexpr float knee    = 0.35f;
    const     float kneeTop = 1.0f / (1.0f + knee);          // f(1), normalises f to 1 at x = 1
    x = juce::jlimit (0.0f, 1.5f, x);
    const float g = (x * x / (x + knee + 1.0e-6f)) / kneeTop;

    constexpr float softStart = 0.7f, headroom = 0.3f;
    if (g <= softStart)
        return g;
    return softStart + headroom * std::tanh ((g - softStart) / headroom);
}

//==============================================================================
// Raw sustained level for a peak: loudest HARMONIC (tonal) band in its slice.
// Using the harmonic stream (not raw level) means sustained tones build the landmass while
// drum hits — which live in the percussive stream — do not inflate the whole Gaussian.
float TerrainModel::peakRawLevel (const AudioFeatures& feats, const TerrainPeak& p) const
{
    float m = 0.0f;
    for (int b = p.bandStart; b <= p.bandEnd; ++b)
        m = juce::jmax (m, feats.harmonic[b]);
    return m;
}

//==============================================================================
// Per-frame peak drives. Three stages:
//  1. Auto-gain: the analyzer maps -60..0 dBFS to 0..1 with no adaptation, so quiet
//     sources barely moved the map and loud masters pinned every peak at 1. A slow
//     loudness reference (fast-ish up, slow down) re-windows the levels: [gateLevel, ref]
//     maps to [0, 0.9]. The window is anchored at ~-54 dBFS so true silence / the noise
//     floor is never boosted, and ref is floored so a quiet room can't max the gain.
//  2. Soft-knee shaping (shapePeakLevel).
//  3. Neighbour inhibition (lateral inhibition, as in the cochlea): each peak is damped
//     by its frequency neighbours' activity. A broadband sound that lights several
//     adjacent slices gets flattened toward its strongest slice, while a sound that
//     stands out in its own range keeps full height — so separate instruments read as
//     separate summits instead of one fused mass.
void TerrainModel::computePeakDrives (const AudioFeatures& feats)
{
    std::array<float, numPeaks> raw {};
    float loudest = 0.0f;
    for (int pk = 0; pk < numPeaks; ++pk)
    {
        raw[pk] = peakRawLevel (feats, terrainPeaks[pk]);
        loudest = juce::jmax (loudest, raw[pk]);
    }

    constexpr float gateLevel = 0.10f;   // ~-54 dBFS: below this stays silent
    constexpr float refFloor  = 0.40f;   // max boost: [0.10, 0.40] stretched to full range
    {
        const float tau = (loudest > loudRef) ? 1.2f : 10.0f;   // seconds
        loudRef += (loudest - loudRef) * (1.0f - std::exp (-dt / tau));
        loudRef  = juce::jmax (refFloor, loudRef);
    }

    std::array<float, numPeaks> shaped {};
    const float invWindow = 0.9f / (loudRef - gateLevel);
    for (int pk = 0; pk < numPeaks; ++pk)
        shaped[pk] = shapePeakLevel ((raw[pk] - gateLevel) * invWindow);

    constexpr float inhibition = 0.45f;
    for (int pk = 0; pk < numPeaks; ++pk)
    {
        float nbr = 0.0f; int n = 0;
        if (pk > 0)            { nbr += shaped[pk - 1]; ++n; }
        if (pk < numPeaks - 1) { nbr += shaped[pk + 1]; ++n; }
        nbr /= (float) juce::jmax (1, n);
        peakDrive[pk] = shaped[pk] * (1.0f - inhibition * juce::jmin (1.0f, nbr));
    }
}

//==============================================================================
// 0..1 spectral tilt within a peak's band slice: 0 = energy sits at the low end of the slice
// (dark timbre → broad, smooth ridges), 1 = energy at the high end (bright timbre → fine,
// sharp ridges). Drives ridge frequency in rebuildOnePeak so different instruments in the
// same peak read as different landform textures.
float TerrainModel::peakBrightness (const AudioFeatures& feats, const TerrainPeak& p) const
{
    if (p.bandEnd <= p.bandStart) return 0.5f;
    float num = 0.0f, den = 0.0f;
    for (int b = p.bandStart; b <= p.bandEnd; ++b)
    {
        const float w = feats.harmonic[b] + feats.percussive[b];
        num += w * (float) (b - p.bandStart);
        den += w;
    }
    if (den < 1.0e-6f) return 0.5f;
    return juce::jlimit (0.0f, 1.0f, (num / den) / (float) (p.bandEnd - p.bandStart));
}

//==============================================================================
void TerrainModel::buildTerrainWeights()
{
    const int   N   = gridSize * gridSize;
    const float inv = 1.0f / (float) gridSize;
    peakWeights.resize (numPeaks * N);
    peakWRX.resize     (numPeaks * N);
    peakWRY.resize     (numPeaks * N);
    peakRidge.resize   (numPeaks * N);
    baseHeightField.assign (N, 0.0f);

    for (int pk = 0; pk < numPeaks; ++pk)
    {
        const auto& p    = terrainPeaks[pk];
        const float cosA = std::cos (p.angle);
        const float sinA = std::sin (p.angle);
        const float sig  = p.sigma * inv;

        for (int gy = 0; gy < gridSize; ++gy)
        for (int gx = 0; gx < gridSize; ++gx)
        {
            const float dx =  gx * inv - p.nx;
            const float dy =  gy * inv - p.ny;
            const float rx =  cosA * dx + sinA * dy;
            const float ry = -sinA * dx + cosA * dy;
            // Domain-warp the envelope coordinate to break elliptical symmetry
            const float wFreq = 2.5f / (p.sigma * inv);
            const float wAmp  = (p.sigma * inv) * 0.5f;
            float wrx, wry;
            Noise::warpPeakCoord (terrainSeed, pk, rx, ry, wFreq, wAmp, wrx, wry);
            const float r2  = (wrx * wrx) / (sig * sig)
                            + (wry * wry) / (sig * sig * p.stretch * p.stretch);
            const float env = std::exp (-r2 * 0.5f);
            const float ridgeFreq = 1.0f / (p.sigma * inv * 2.5f);
            const float ridgeVal  = Noise::ridgedFbm (terrainSeed,
                rx * ridgeFreq + (float)pk * 7.3f,
                ry * ridgeFreq * p.stretch + (float)pk * 3.1f,
                5, 2.0f, 0.5f, 1.0f);
            const int idx = pk * N + gy * gridSize + gx;
            peakWeights[idx] = env * ridgeVal;
            peakWRX[idx]     = wrx;
            peakWRY[idx]     = wry;
            peakRidge[idx]   = ridgeVal;
        }
    }

    // Base terrain from two-layer ridged fBm — no circular Gaussian symmetry.
    // Macro scales and offsets vary per launch so the coarse ridge structure differs.
    std::mt19937 macroRng (terrainSeed ^ 0xDEADBEEFu);
    std::uniform_real_distribution<float> coarseScaleDist (1.6f, 2.6f);
    std::uniform_real_distribution<float> midScaleDist    (0.9f, 1.7f);
    std::uniform_real_distribution<float> offDist         (0.0f, 12.0f);
    const float coarseScale = coarseScaleDist (macroRng);
    const float midScale    = midScaleDist    (macroRng);
    const float midOffX     = offDist         (macroRng);
    const float midOffY     = offDist         (macroRng);

    for (int gy = 0; gy < gridSize; ++gy)
    for (int gx = 0; gx < gridSize; ++gx)
    {
        const float nx = gx * inv;
        const float ny = gy * inv;
        baseHeightField[gy * gridSize + gx] =
            0.20f * Noise::ridgedFbm (terrainSeed, nx * coarseScale,        ny * coarseScale,        4, 2.0f, 0.5f, 1.0f)
          + 0.08f * Noise::ridgedFbm (terrainSeed, nx * midScale + midOffX, ny * midScale + midOffY, 3, 2.0f, 0.5f, 0.9f);
    }

    // fBm roughness layer — 7 octaves at gridSize=256 resolves down to ~1-cell features
    const float fbmFreqBase = 3.5f, fbmAmplitude = 0.18f;
    for (int gy = 0; gy < gridSize; ++gy)
    for (int gx = 0; gx < gridSize; ++gx)
        baseHeightField[gy * gridSize + gx] +=
            fbmAmplitude * (Noise::fbmNoise (terrainSeed, gx * inv * fbmFreqBase,
                                              gy * inv * fbmFreqBase,
                                              7, 2.0f, 0.5f) - 0.5f);

    // North-to-south drainage slope: terrain is highest at north edge, lowest at south.
    // Valleys naturally drain southward, so gradient-descent river needs no directional bias.
    for (int gy = 0; gy < gridSize; ++gy)
    for (int gx = 0; gx < gridSize; ++gx)
        baseHeightField[gy * gridSize + gx] += 0.28f * (1.0f - (float)gy * inv);

    // Geologic realism: erode the raw ridged-fBm base so ridgelines, slopes, and drainage
    // read as a weathered landscape rather than pure noise. One-time, per-seed only.
    erodeBaseHeight();
}

//==============================================================================
// One-time thermal (talus) erosion + light hydraulic smoothing of the static base terrain.
// Thermal erosion moves material from cells whose slope to their lowest 4-neighbour exceeds
// a talus angle, sharpening ridgelines and forming plateaus/scree slopes; the interleaved
// smoothing simulates sediment deposition so valleys stay connected and drainable. Runs on
// baseHeightField only (before the river is carved), so it never touches the per-frame path.
void TerrainModel::erodeBaseHeight()
{
    const int   N       = gridSize;
    constexpr int   iterations = 25;
    constexpr float talus      = 0.004f;   // slope threshold below which nothing moves
    constexpr float carry      = 0.5f;     // fraction of the excess moved downhill per iteration
    constexpr float smoothMix  = 0.12f;    // deposition/hydraulic blur strength

    std::vector<float> delta (N * N, 0.0f);
    auto& h = baseHeightField;

    for (int it = 0; it < iterations; ++it)
    {
        std::fill (delta.begin(), delta.end(), 0.0f);

        // Thermal transport toward the lowest 4-neighbour.
        for (int gy = 1; gy < N - 1; ++gy)
        for (int gx = 1; gx < N - 1; ++gx)
        {
            const int   c  = gy * N + gx;
            const float hc = h[c];

            int   lowIdx  = c;
            float lowVal  = hc;
            const int nb[4] = { c - 1, c + 1, c - N, c + N };
            for (int k = 0; k < 4; ++k)
                if (h[nb[k]] < lowVal) { lowVal = h[nb[k]]; lowIdx = nb[k]; }

            const float diff = hc - lowVal;
            if (diff > talus)
            {
                const float move = carry * (diff - talus) * 0.5f;
                delta[c]      -= move;
                delta[lowIdx] += move;
            }
        }
        for (int i = 0; i < N * N; ++i)
            h[i] += delta[i];

        // Light hydraulic smoothing (box blur toward the 4-neighbour mean).
        for (int gy = 1; gy < N - 1; ++gy)
        for (int gx = 1; gx < N - 1; ++gx)
        {
            const int c = gy * N + gx;
            const float mean = 0.25f * (h[c - 1] + h[c + 1] + h[c - N] + h[c + N]);
            delta[c] = (mean - h[c]) * smoothMix;
        }
        for (int gy = 1; gy < N - 1; ++gy)
        for (int gx = 1; gx < N - 1; ++gx)
            h[gy * N + gx] += delta[gy * N + gx];
    }
}

//==============================================================================
// Recomputes peak envelope weights using animated sigma/stretch — called every 4 frames.
// Ridge texture and warped coordinates are kept from the precomputed arrays to avoid
// expensive per-frame fBm evaluation.
void TerrainModel::rebuildPeakWeights (const AudioFeatures& feats, float animTime)
{
    const int   N   = gridSize * gridSize;
    const float inv = 1.0f / (float) gridSize;

    juce::ignoreUnused (feats);               // levels now come from peakDrive (computePeakDrives)
    dirtyPlaneBits = (1u << numPeaks) - 1u;   // every plane refreshed below

    for (int pk = 0; pk < numPeaks; ++pk)
    {
        const auto& p     = terrainPeaks[pk];
        const float level = peakDrive[pk];
        const float phase = peakPhaseOffset[pk];

        // Sigma: idle breath (gated by audioActivity) + modest scaling from the drive
        // (loudness mostly reads as height; see sigmaLevelGain).
        const float sigBreath   = 1.0f + (0.03f + 0.12f * audioActivity) * std::sin (animTime * 0.5f + phase)
                                       + sigmaLevelGain * level;
        const float liveSigma   = p.sigma * juce::jmax (0.5f, sigBreath);
        // Stretch inflates slightly with louder audio
        const float liveStretch = p.stretch * (1.0f + stretchLevelGain * level);

        const float sig        = liveSigma * inv;
        const float inv_sig2   = 1.0f / (sig * sig);
        const float inv_sig2s2 = inv_sig2 / (liveStretch * liveStretch);

        const float* wx  = peakWRX.data()   + pk * N;
        const float* wy  = peakWRY.data()   + pk * N;
        const float* rg  = peakRidge.data() + pk * N;
        float*       wgt = peakWeights.data() + pk * N;

        for (int i = 0; i < N; ++i)
        {
            const float r2 = wx[i] * wx[i] * inv_sig2 + wy[i] * wy[i] * inv_sig2s2;
            wgt[i] = std::exp (-r2 * 0.5f) * rg[i];
        }
    }
}

//==============================================================================
// Full rebuild for one peak: recomputes warp coords (using current position from the
// drift step), ridge texture (using ridgeFlowX/Y), and Gaussian envelope.
// Called once per frame cycling through all peaks — keeps per-frame cost ~6ms.
void TerrainModel::rebuildOnePeak (int pk, const AudioFeatures& feats, float animTime)
{
    const int   N   = gridSize * gridSize;
    const float inv = 1.0f / (float) gridSize;

    dirtyPlaneBits |= (1u << pk);
    const auto& p    = terrainPeaks[pk];
    const float cosA = std::cos (p.angle);
    const float sinA = std::sin (p.angle);

    // Envelope params for this frame
    const float level       = peakDrive[pk];
    const float brightness  = peakBrightness (feats, p);   // 0..1 spectral tilt within the slice
    const float phase       = peakPhaseOffset[pk];
    const float sigBreath   = 1.0f + (0.03f + 0.12f * audioActivity) * std::sin (animTime * 0.5f + phase)
                                   + sigmaLevelGain * level;
    const float liveSigma   = p.sigma * juce::jmax (0.5f, sigBreath);
    const float liveStretch = p.stretch * (1.0f + stretchLevelGain * level);
    const float sig         = liveSigma * inv;
    const float inv_sig2    = 1.0f / (sig * sig);
    const float inv_sig2s2  = inv_sig2 / (liveStretch * liveStretch);

    float* wx  = peakWRX.data()    + pk * N;
    float* wy  = peakWRY.data()    + pk * N;
    float* rg  = peakRidge.data()  + pk * N;
    float* wgt = peakWeights.data() + pk * N;

    const float wFreq     = 2.5f / (p.sigma * inv);
    const float wAmp      = (p.sigma * inv) * 0.5f;
    // Brightness → ridge frequency: bright timbres carve finer, sharper ridgelines (0.7x..1.6x).
    const float ridgeFreq = (1.0f / (p.sigma * inv * 2.5f)) * (0.7f + 0.9f * brightness);

    for (int gy = 0; gy < gridSize; ++gy)
    for (int gx = 0; gx < gridSize; ++gx)
    {
        const float dx =  gx * inv - p.nx;
        const float dy =  gy * inv - p.ny;
        const float rx =  cosA * dx + sinA * dy;
        const float ry = -sinA * dx + cosA * dy;

        // Domain-warp envelope coords (shared helper — see buildTerrainWeights)
        float wrx, wry;
        Noise::warpPeakCoord (terrainSeed, pk, rx, ry, wFreq, wAmp, wrx, wry);

        // Ridge sampled with scrolled offset (flowing texture)
        const float ridgeVal = Noise::ridgedFbm (terrainSeed,
            rx * ridgeFreq + (float)pk * 7.3f  + ridgeFlowX,
            ry * ridgeFreq * p.stretch + (float)pk * 3.1f + ridgeFlowY,
            5, 2.0f, 0.5f, 1.0f);

        const int idx = gy * gridSize + gx;
        wx[idx]  = wrx;
        wy[idx]  = wry;
        rg[idx]  = ridgeVal;

        const float r2 = wrx * wrx * inv_sig2 + wry * wry * inv_sig2s2;
        wgt[idx] = std::exp (-r2 * 0.5f) * ridgeVal;
    }
}

//==============================================================================
void TerrainModel::update (const AudioFeatures& feats,
                           const std::vector<float>& riverPeakMask, float animTime)
{
    // Smoothed 0..1 overall-energy envelope that gates idle motion (breathing, drift):
    // fast attack so it catches up quickly when music starts, slow release (~1.3s half-life)
    // so it doesn't snap back to "quiet" between beats.
    {
        const float curEnergy = feats.overallEnergy;
        const float aAlpha = (curEnergy > audioActivity) ? 0.12f : 0.015f;
        audioActivity += (juce::jlimit (0.0f, 1.0f, curEnergy * 2.2f) - audioActivity) * aAlpha;
    }

    // This frame's auto-gained, shaped, neighbour-inhibited per-peak drives.
    computePeakDrives (feats);

    // Ridge flow and per-peak position drift
    {
        // Advance ridge scroll — speed proportional to overall spectral energy
        const float avgEnergy = feats.overallEnergy;
        ridgeFlowX += avgEnergy * 0.08f * dt;   // ~0.024 units/sec at 30% energy
        ridgeFlowY += avgEnergy * 0.055f * dt;  // different rate for diagonal drift

        // Update all peak velocities and positions each frame.
        // Drift is bounded to a small radius around each peak's fixed spawn anchor (spring-back
        // force + hard wander clamp) so a peak stays recognizably "in its spot" for the whole
        // session instead of roaming across most of the canvas — preserves the frequency <-> screen
        // location association a viewer builds up over time.
        //
        // Interaction: simultaneously active peaks that sit close together push each other
        // apart (force ~ product of their drives), so two instruments playing at once visibly
        // shoulder away from each other instead of fusing. Bounded by the same wander radius.
        std::array<float, numPeaks> repelX {}, repelY {};
        {
            constexpr float repelRange = 0.30f;    // normalised map units
            constexpr float repelK     = 0.004f;
            for (int i = 0; i < numPeaks; ++i)
            for (int j = i + 1; j < numPeaks; ++j)
            {
                const float both = peakDrive[i] * peakDrive[j];
                if (both < 1.0e-4f) continue;
                const float ddx = terrainPeaks[i].nx - terrainPeaks[j].nx;
                const float ddy = terrainPeaks[i].ny - terrainPeaks[j].ny;
                const float d   = std::sqrt (ddx * ddx + ddy * ddy);
                if (d >= repelRange || d < 1.0e-4f) continue;
                const float s = repelK * both * (1.0f - d / repelRange) / d;
                repelX[i] += ddx * s;  repelY[i] += ddy * s;
                repelX[j] -= ddx * s;  repelY[j] -= ddy * s;
            }
        }

        for (int pk = 0; pk < numPeaks; ++pk)
        {
            auto& p   = terrainPeaks[pk];
            auto& vel = peakVelocity[pk];

            vel.vx += repelX[pk] * dt;
            vel.vy += repelY[pk] * dt;

            const float level      = peakDrive[pk];
            const float kickAngle  = animTime * 0.3f + (float) pk * 2.399f;
            const float driftScale = 0.4f + 0.6f * audioActivity;   // calmer when quiet
            vel.vx += std::cos (kickAngle) * level * 0.0001f * dt * driftScale;
            vel.vy += std::sin (kickAngle) * level * 0.0001f * dt * driftScale;
            vel.vx *= 0.98f;  // damping keeps terminal velocity gentle
            vel.vy *= 0.98f;

            // Spring pulling back toward the anchor — keeps the peak "in its spot."
            constexpr float springK = 0.045f;
            vel.vx += (p.anchorNx - p.nx) * springK * dt;
            vel.vy += (p.anchorNy - p.ny) * springK * dt;

            float newNx = p.nx + vel.vx;
            float newNy = p.ny + vel.vy;

            // Bounded wander radius around the anchor, not a full-canvas clamp.
            constexpr float maxWanderR = 0.06f;
            float ddx = newNx - p.anchorNx, ddy = newNy - p.anchorNy;
            float distR = std::sqrt (ddx * ddx + ddy * ddy);
            if (distR > maxWanderR)
            {
                const float s = maxWanderR / distR;
                newNx = p.anchorNx + ddx * s;
                newNy = p.anchorNy + ddy * s;
                const float nxr = ddx / distR, nyr = ddy / distR;
                const float vDotN = vel.vx * nxr + vel.vy * nyr;
                if (vDotN > 0.0f) { vel.vx -= vDotN * nxr * 1.4f; vel.vy -= vDotN * nyr * 1.4f; }
            }

            constexpr float clamp = 0.14f;   // outer safety bound, should rarely trigger
            p.nx = juce::jlimit (clamp, 1.0f - clamp, newNx);
            p.ny = juce::jlimit (clamp, 1.0f - clamp, newNy);
        }

        // Full spatial + envelope rebuild for one peak per frame (staggered cycle)
        // Each peak's warp coords, ridge, and envelope are refreshed every 10 frames (~6 Hz)
        peakSpatialIndex = (peakSpatialIndex + 1) % numPeaks;
        rebuildOnePeak (peakSpatialIndex, feats, animTime);
    }

    // Rebuild animated peak weights every 4 frames (~15 Hz). The caller's clock has
    // advanced by dt between the drift step above and this one in the original ordering,
    // so we add dt here rather than receiving a second animTime parameter.
    if (++rebuildCounter >= 4)
    {
        rebuildCounter = 0;
        rebuildPeakWeights (feats, animTime + dt);
    }

    // Per-peak transient flash: triggers on percussive onsets (e.g. drum hits).
    // Driven directly by the HPSS-derived spectral flux (feats.flux) over the peak's slice —
    // this is already a positive-going onset signal isolated to the percussive stream, so a
    // sustained tone in the same band does not trigger it. Two-stage decay (fast snap + slower
    // glow tail, combined via max) gives a percussive "hit then afterglow" shape.
    for (int pk = 0; pk < numPeaks; ++pk)
    {
        const auto& p = terrainPeaks[pk];
        float fluxLvl = 0.0f;
        for (int b = p.bandStart; b <= p.bandEnd; ++b)
            fluxLvl = juce::jmax (fluxLvl, feats.flux[b]);
        const float trigger = juce::jlimit (0.0f, 1.5f, fluxLvl * 3.5f);

        if (trigger > peakFlash[pk])
        {
            peakFlash[pk]     = trigger;
            peakFlashTail[pk] = trigger * 0.5f;
        }
        else
        {
            peakFlash[pk]     *= 0.55f;
            peakFlashTail[pk] *= 0.90f;
        }
        peakFlash[pk] = juce::jmax (peakFlash[pk], peakFlashTail[pk]);
    }

    // This frame's per-peak amplitudes: base + audio-amplitude-scaled peak shapes.
    // Flash overshoot (peakFlash) is allowed to push a peak's rendered height above the
    // contour ceiling (see below) so a hard hit visibly "breaks through" the outer rings
    // instead of being absorbed into a global rescale. The full-field summation the old
    // CPU loop did here now runs in the GPU compose pass; amps below 1e-4 are zeroed so
    // both the shader's contribution and sampleComposedHeight() match the old skip.
    // riverPeakMask is still consumed by sampleComposedHeight()/the GPU compose pass, not by
    // this per-frame amplitude update — kept in the signature for callers, unused here.
    juce::ignoreUnused (riverPeakMask);
    for (int pk = 0; pk < numPeaks; ++pk)
    {
        const float amp = peakDrive[pk] * (1.0f + peakFlash[pk] * 0.9f);
        peakAmps[pk] = (amp < 1e-4f) ? 0.0f : amp * heightScale;
    }

    // Track the contour ceiling from a *sustained* per-peak amplitude proxy that deliberately
    // excludes peakFlash, with a much slower attack/decay than the old instantaneous-max
    // tracking. This stops a single drum hit from rescaling the entire 60-level contour ladder
    // (which used to shrink every other peak's apparent height and leave a multi-second
    // "deflated" look behind) — the ceiling now reflects genuinely sustained loudness only.
    {
        float sustainedHMax = 0.0f;
        for (int pk = 0; pk < numPeaks; ++pk)
            sustainedHMax = juce::jmax (sustainedHMax, peakDrive[pk] * heightScale);
        // The excess above the 1.0 floor relaxes with a half-life that follows the music:
        // ~4 s while playing (a single hit still can't move it), ~0.6 s once the audio has
        // gone quiet — so pausing returns the contour ladder to baseline in ~2 s instead
        // of leaving the bare terrain drawn against a stale, inflated ceiling for ~10 s.
        const float halfLife = 0.6f + 3.4f * audioActivity;
        heightFieldSustain = 1.0f + (heightFieldSustain - 1.0f) * std::exp2 (-dt / halfLife);
        if (sustainedHMax > heightFieldSustain)
            heightFieldSustain += (sustainedHMax - heightFieldSustain) * 0.03f;
        heightFieldSustain = juce::jmax (1.0f, heightFieldSustain);
        heightFieldMax = heightFieldSustain;
    }
}
