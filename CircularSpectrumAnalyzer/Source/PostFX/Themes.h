#pragma once

#include <cstring>

// Named colour palettes ("themes") for the whole tube. The visualizer collapses
// every layer (scene shader, river ribbons, map labels, terminal UI, and the CRT
// phosphor character) onto one live hue; a Theme reshapes that hue's rest point,
// how far the audio swings it, its saturation, the chromatic-aberration fringe
// hues, and the post-FX phosphor/grain tint. Header-only, in the same spirit as
// Audio/BandGroups.h.
//
// kThemes[0] ("GREEN P31") reproduces the pre-theme behaviour exactly, so the
// default frame is a guaranteed no-op.
namespace Themes
{

// Post-FX multiplicative tints can exceed 1.0 (the green phosphor bias is 1.04),
// so they are raw floats rather than juce::Colour (which clamps to 8-bit).
struct Tint { float r, g, b; };

struct Theme
{
    const char* name;      // THEME: readout and the saved-settings key (<= 11 chars, see ScreenLayout::colReset)

    // Audio-driven hue: targetHue = baseHue - bass*warmPush + high*coolPush,
    // clamped to [hueMin, hueMax]. Small pushes + a tight clamp => the tube stays
    // a single colour (true monochrome terminal); wide pushes => it swings.
    float baseHue;         // rest hue when the mix is quiet (JUCE HSV, 0..1)
    float warmPush;        // bass pull (was hardcoded 0.62)
    float coolPush;        // highs push (was hardcoded 0.30)
    float hueMin, hueMax;  // swing clamp (was 0..1)

    float saturation;      // global HSV saturation multiplier (amber desaturates)

    float caLowHue;        // chromatic-aberration fringe hues (were 0.0 / 0.62)
    float caHighHue;

    Tint phosphorTint;     // post-FX bias (was vec3(0.93, 1.04, 0.95))
    Tint grainTint;        // grain + spark tint (was vec3(0.55, 1.0, 0.62))

    float convergence;     // per-theme RGB-convergence multiplier (1 = neutral)
    float halation;        // per-theme halation-glow multiplier (1 = neutral)

    // Elevation colouring: hue offset per 10-level contour band, centred on the
    // middle band, so the band ladder spans about +/-2.5 * elevHueSpread around
    // the live hue (was hardcoded 0.015). Negative runs the ramp the other way.
    // elevFill (0..1) carries the same ramp into the hillshade, stipple and
    // hachure fill between the lines; 0 = fill stays the flat live hue.
    float elevHueSpread;
    float elevFill;

    // Map-box background wash at the centre, fading to black at the corners
    // (was a fixed vec3(3,8,3)/255 for every theme).
    Tint  bgTint;

    // Per-channel phosphor persistence, added to the audio-driven per-frame decay
    // (was a fixed +0.03 on green only). A negative channel fades faster, so the
    // afterglow drifts away from the fresh trace's colour (P7: blue flash, yellow-
    // green tail).
    Tint  decayBias;

    // 0..1 blend from the hue ramp to this theme's false-colour ramp (ramp[] below)
    // by elevation, for contours and fill. 0 = plain hue ramp; ramp[] is ignored.
    float rampMix;

    // 0..1 darkening of the lowest elevation bands, easing to none at the summits
    // (contours and fill). Tames themes whose low end lands on a bright hue.
    float lowDim;

    // False-colour ramp used when rampMix > 0: five stops from valley floor to summit,
    // spaced evenly after x^rampGamma (< 1 spends more of the ramp on low ground).
    // Themes that do not use it leave these zero.
    float rampGamma;
    Tint  ramp[5];
};

// Index 0 is the untouched default (and the factory reset). The rest run round
// the hue wheel from it, so T steps to a neighbouring colour and Shift+T back.
// Settings save the theme by NAME, so this order can change freely; renaming a
// theme needs a kLegacyNames-style fallback or saved choices fall back to 0.
static constexpr Theme kThemes[] =
{
    // GREEN P31 — identical to the original hardcoded look.
    { "GREEN P31", 0.376f, 0.62f, 0.30f, 0.00f, 1.00f, 1.00f,
      0.00f, 0.62f, { 0.93f, 1.04f, 0.95f }, { 0.55f, 1.00f, 0.62f }, 1.00f, 1.00f,
      0.015f, 0.00f, { 0.012f, 0.031f, 0.012f }, { 0.00f, 0.03f, 0.00f }, 0.0f, 0.00f },

    // CGA MODE4 — punchy cyan that snaps toward magenta on bass; magenta/yellow
    // fringes complete the CGA cyan-magenta-yellow trio. Lows lean blue and dim
    // (not green) so the valleys and river banks stay flat CGA cyan.
    { "CGA MODE4", 0.500f, -0.33f, 0.00f, 0.500f, 0.850f, 1.00f,
      0.830f, 0.150f, { 1.00f, 1.00f, 1.00f }, { 0.80f, 0.90f, 1.00f }, 1.35f, 0.85f,
      -0.012f, 1.00f, { 0.006f, 0.022f, 0.030f }, { 0.00f, 0.03f, 0.00f }, 0.0f, 0.45f },

    // ICE P11 — harsh, washed-out blue-white plotter: low saturation, lifted
    // blacks, little glow and a wide red/blue convergence split.
    { "ICE P11", 0.550f, 0.15f, 0.10f, 0.480f, 0.620f, 0.50f,
      0.000f, 0.620f, { 0.94f, 1.00f, 1.08f }, { 0.80f, 0.92f, 1.00f }, 1.50f, 0.60f,
      0.012f, 0.50f, { 0.022f, 0.030f, 0.038f }, { 0.00f, 0.03f, 0.00f }, 0.0f, 0.00f },

    // SCOPE P7 — dual-layer oscilloscope phosphor: the beam writes blue-white, the
    // afterglow fades yellow-green (blue decays fast, red/green linger).
    { "SCOPE P7", 0.580f, 0.03f, 0.03f, 0.550f, 0.620f, 0.35f,
      0.150f, 0.600f, { 0.95f, 1.02f, 1.04f }, { 0.75f, 1.00f, 0.40f }, 1.00f, 1.00f,
      0.010f, 0.50f, { 0.012f, 0.022f, 0.030f }, { 0.10f, 0.13f, -0.15f }, 0.0f, 0.00f },

    // COBALT 27 — saturated electric blue; drifts toward indigo on highs. Ramp
    // runs dim indigo lows up to azure summits.
    { "COBALT 27", 0.625f, 0.05f, 0.04f, 0.580f, 0.680f, 0.95f,
      0.000f, 0.660f, { 0.82f, 0.90f, 1.12f }, { 0.45f, 0.65f, 1.00f }, 1.20f, 0.90f,
      -0.030f, 1.00f, { 0.006f, 0.012f, 0.035f }, { 0.00f, 0.03f, 0.00f }, 0.0f, 0.40f },

    // VIOLET 405 — synthwave purple that swings magenta (bass) to blue (highs).
    { "VIOLET 405", 0.780f, -0.07f, -0.09f, 0.700f, 0.870f, 0.95f,
      0.830f, 0.660f, { 1.00f, 0.82f, 1.10f }, { 0.80f, 0.50f, 1.00f }, 1.50f, 1.05f,
      0.045f, 1.00f, { 0.024f, 0.008f, 0.035f }, { 0.00f, 0.03f, 0.00f }, 0.0f, 0.00f },

    // VAPOR 95 — rose pink at rest; highs swing it through violet toward cyan, bass
    // nudges it toward red.
    { "VAPOR 95", 0.920f, -0.03f, -0.42f, 0.500f, 0.950f, 0.85f,
      0.500f, 0.830f, { 1.06f, 0.88f, 1.02f }, { 1.00f, 0.55f, 0.85f }, 1.30f, 1.05f,
      0.020f, 1.00f, { 0.030f, 0.008f, 0.024f }, { 0.00f, 0.00f, 0.03f }, 0.0f, 0.00f },

    // INFRA P22 — pure-red P22 monochrome; "night ops" thermal glow, stays red.
    { "INFRA P22", 0.004f, 0.008f, 0.012f, 0.000f, 0.022f, 0.95f,
      0.010f, 0.060f, { 1.14f, 0.74f, 0.72f }, { 1.00f, 0.36f, 0.34f }, 0.80f, 1.25f,
      0.020f, 0.70f, { 0.035f, 0.006f, 0.006f }, { 0.00f, 0.03f, 0.00f }, 0.0f, 0.00f },

    // MAGMA 1200 — near-black crust with cracks glowing deep red, rising through
    // orange to white-hot summits; red persistence lingers like cooling lava.
    { "MAGMA 1200", 0.030f, 0.02f, 0.02f, 0.000f, 0.060f, 1.00f,
      0.000f, 0.080f, { 1.12f, 0.86f, 0.70f }, { 1.00f, 0.45f, 0.15f }, 0.90f, 1.30f,
      0.000f, 1.00f, { 0.020f, 0.004f, 0.002f }, { 0.10f, 0.00f, -0.05f }, 1.0f, 0.30f,
      0.55f, { { 0.30f, 0.03f, 0.02f }, { 0.85f, 0.14f, 0.02f }, { 1.00f, 0.38f, 0.03f },
               { 1.00f, 0.65f, 0.10f }, { 1.00f, 0.95f, 0.70f } } },

    // SODIUM 589 — street-lamp red-orange between INFRA P22 and AMBER P3; its ramp
    // runs the other way (orange valleys, red summits).
    { "SODIUM 589", 0.055f, 0.010f, 0.020f, 0.040f, 0.075f, 1.00f,
      0.020f, 0.120f, { 1.10f, 0.80f, 0.45f }, { 1.00f, 0.58f, 0.15f }, 0.95f, 1.20f,
      -0.018f, 1.00f, { 0.033f, 0.012f, 0.003f }, { 0.00f, 0.03f, 0.00f }, 0.0f, 0.00f },

    // THERMAL LW — false-colour heat map by elevation; louder mixes run hotter.
    { "THERMAL LW", 0.060f, 0.02f, 0.02f, 0.030f, 0.090f, 1.00f,
      0.830f, 0.150f, { 1.05f, 0.95f, 0.92f }, { 1.00f, 0.60f, 0.30f }, 1.00f, 1.10f,
      0.000f, 1.00f, { 0.020f, 0.005f, 0.030f }, { 0.03f, 0.00f, -0.02f }, 1.0f, 0.00f,
      0.65f, { { 0.45f, 0.25f, 1.00f }, { 0.85f, 0.20f, 0.85f }, { 1.00f, 0.12f, 0.18f },
               { 1.00f, 0.62f, 0.05f }, { 1.00f, 1.00f, 0.85f } } },

    // AMBER P3 — warm monochrome terminal; red-orange valleys to gold summits.
    { "AMBER P3", 0.090f, 0.05f, 0.03f, 0.060f, 0.120f, 0.90f,
      0.050f, 0.120f, { 1.05f, 0.92f, 0.72f }, { 1.00f, 0.75f, 0.35f }, 0.90f, 1.15f,
      0.030f, 1.00f, { 0.031f, 0.018f, 0.004f }, { 0.00f, 0.03f, 0.00f }, 0.0f, 0.00f },

    // WHITE P4 — warm paper-white P4 TV monochrome; a faint cream cast keeps it
    // apart from the cool whites (ICE P11, SCOPE P7).
    { "WHITE P4", 0.100f, 0.00f, 0.00f, 0.090f, 0.110f, 0.12f,
      0.000f, 0.620f, { 1.04f, 1.00f, 0.92f }, { 1.00f, 0.92f, 0.78f }, 1.00f, 0.50f,
      0.000f, 0.00f, { 0.024f, 0.021f, 0.016f }, { 0.00f, 0.03f, 0.00f }, 0.0f, 0.00f },

    // LIME P1 — yellow-green P1 radar phosphor; yellow valleys to green summits.
    { "LIME P1", 0.250f, 0.10f, 0.08f, 0.200f, 0.310f, 0.95f,
      0.000f, 0.550f, { 0.98f, 1.06f, 0.80f }, { 0.75f, 1.00f, 0.35f }, 1.00f, 1.10f,
      0.025f, 1.00f, { 0.022f, 0.030f, 0.006f }, { 0.00f, 0.03f, 0.00f }, 0.0f, 0.00f },

    // NVG P43 — image-intensifier night vision: dim, heavily grained green valleys,
    // high ground blooming out to white like an overdriven tube.
    { "NVG P43", 0.330f, 0.02f, 0.02f, 0.310f, 0.350f, 0.85f,
      0.300f, 0.400f, { 0.92f, 1.06f, 0.92f }, { 0.90f, 1.60f, 0.90f }, 0.80f, 1.50f,
      0.000f, 1.00f, { 0.015f, 0.040f, 0.015f }, { 0.00f, 0.04f, 0.00f }, 1.0f, 0.00f,
      1.00f, { { 0.10f, 0.35f, 0.12f }, { 0.20f, 0.65f, 0.22f }, { 0.40f, 0.95f, 0.40f },
               { 0.75f, 1.00f, 0.70f }, { 1.00f, 1.00f, 0.95f } } },
};

static constexpr int numThemes = (int) (sizeof (kThemes) / sizeof (kThemes[0]));

inline const Theme& get (int index) noexcept
{
    const int i = ((index % numThemes) + numThemes) % numThemes;
    return kThemes[i];
}

// Index of the theme with this name, or -1.
inline int findByName (const char* name) noexcept
{
    for (int i = 0; i < numThemes; ++i)
        if (std::strcmp (kThemes[i].name, name) == 0)
            return i;
    return -1;
}

// Settings from 1.0.0 stored the theme as an index into the original 9-theme
// table; these are those themes' current names, in that old order.
static constexpr const char* kLegacyNames[] =
{
    "GREEN P31", "AMBER P3", "ICE P11", "CGA MODE4", "INFRA P22",
    "SODIUM 589", "COBALT 27", "VIOLET 405", "WHITE P4",
};

} // namespace Themes
