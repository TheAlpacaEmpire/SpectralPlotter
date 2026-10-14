#pragma once

#include <cstdint>

// Value noise + fBm + ridged fBm — pure functions, seeded explicitly so callers
// (terrain generation, paint-time jitter effects) get reproducible per-seed variation.
namespace Noise
{
    uint32_t valueNoiseHash    (uint32_t seed, int x, int y) noexcept;
    float    valueNoiseLattice (uint32_t seed, int x, int y) noexcept;
    float    valueNoiseSample  (uint32_t seed, float x, float y) noexcept;
    float    fbmNoise          (uint32_t seed, float x, float y, int octaves,
                                 float lacunarity, float gain) noexcept;
    float    ridgedFbm         (uint32_t seed, float x, float y, int octaves,
                                 float lacunarity, float gain, float offset) noexcept;

    // Domain-warps a peak-local envelope coordinate (rx,ry) with two fBm lookups to break the
    // elliptical Gaussian symmetry. Shared verbatim by TerrainModel::buildTerrainWeights and
    // rebuildOnePeak — the per-peak phase offsets are baked in so both produce identical warps.
    void warpPeakCoord (uint32_t seed, int peakIndex, float rx, float ry,
                        float wFreq, float wAmp, float& wrx, float& wry) noexcept;
}
