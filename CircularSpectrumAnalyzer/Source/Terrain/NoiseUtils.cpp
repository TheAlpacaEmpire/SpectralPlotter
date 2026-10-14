#include "NoiseUtils.h"

#include <cmath>
#include <JuceHeader.h>

namespace Noise
{
    uint32_t valueNoiseHash (uint32_t seed, int x, int y) noexcept
    {
        uint32_t h = (uint32_t)(x * 1619 + y * 31337) ^ seed;
        h ^= (h << 13); h ^= (h >> 17); h ^= (h << 5);
        return h;
    }

    float valueNoiseLattice (uint32_t seed, int x, int y) noexcept
    {
        return (float)(valueNoiseHash (seed, x, y) & 0xFFFFFF) / (float)0xFFFFFF;
    }

    float valueNoiseSample (uint32_t seed, float x, float y) noexcept
    {
        const int   ix = (int)std::floor (x), iy = (int)std::floor (y);
        const float fx = x - (float)ix,      fy = y - (float)iy;
        const float ux = fx * fx * (3.f - 2.f * fx);
        const float uy = fy * fy * (3.f - 2.f * fy);
        return valueNoiseLattice (seed, ix,   iy  ) * (1 - ux) * (1 - uy)
             + valueNoiseLattice (seed, ix+1, iy  ) *      ux  * (1 - uy)
             + valueNoiseLattice (seed, ix,   iy+1) * (1 - ux) *      uy
             + valueNoiseLattice (seed, ix+1, iy+1) *      ux  *      uy;
    }

    float fbmNoise (uint32_t seed, float x, float y, int octaves, float lac, float gain) noexcept
    {
        float v = 0, a = 0.5f, f = 1.f, m = 0.f;
        for (int o = 0; o < octaves; ++o)
        {
            v += a * valueNoiseSample (seed, x * f, y * f);
            m += a; a *= gain; f *= lac;
        }
        return v / m;
    }

    float ridgedFbm (uint32_t seed, float x, float y, int octaves,
                      float lac, float gain, float offset) noexcept
    {
        float value = 0.f, amp = 0.5f, freq = 1.f, weight = 1.f;
        for (int o = 0; o < octaves; ++o)
        {
            float sig = valueNoiseSample (seed, x * freq, y * freq) * 2.f - 1.f;
            sig = offset - std::abs (sig);
            sig *= sig * weight;
            value  += sig * amp;
            weight  = juce::jlimit (0.f, 1.f, sig * 2.f);
            amp *= gain; freq *= lac;
        }
        return juce::jlimit (0.f, 1.f, value);
    }

    void warpPeakCoord (uint32_t seed, int peakIndex, float rx, float ry,
                        float wFreq, float wAmp, float& wrx, float& wry) noexcept
    {
        const float pk = (float) peakIndex;
        wrx = rx + wAmp * (fbmNoise (seed, rx * wFreq + pk * 4.9f,
                                           ry * wFreq + pk * 2.3f,
                                           3, 2.0f, 0.5f) - 0.5f);
        wry = ry + wAmp * (fbmNoise (seed, rx * wFreq + pk * 7.1f + 2.0f,
                                           ry * wFreq + pk * 1.7f + 4.5f,
                                           3, 2.0f, 0.5f) - 0.5f);
    }
}
