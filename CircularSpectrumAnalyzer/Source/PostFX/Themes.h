#pragma once

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
    const char* name;      // shown in the THEME: readout

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
};

// Order fixed: index 0 is the untouched default. Cycled with the T key / the
// [ THEME ] button.
static constexpr Theme kThemes[] =
{
    // GREEN P31 — identical to the original hardcoded look.
    { "GREEN P31", 0.376f, 0.62f, 0.30f, 0.00f, 1.00f, 1.00f,
      0.00f, 0.62f, { 0.93f, 1.04f, 0.95f }, { 0.55f, 1.00f, 0.62f }, 1.00f, 1.00f },

    // AMBER P3 — warm monochrome terminal; barely swings, slightly desaturated.
    { "AMBER P3", 0.090f, 0.05f, 0.03f, 0.060f, 0.120f, 0.90f,
      0.050f, 0.120f, { 1.05f, 0.92f, 0.72f }, { 1.00f, 0.75f, 0.35f }, 0.90f, 1.15f },

    // ICE — cool blue-white plotter; whiter (low sat), classic red/blue fringe.
    { "ICE", 0.550f, 0.15f, 0.10f, 0.480f, 0.620f, 0.72f,
      0.000f, 0.620f, { 0.90f, 0.98f, 1.06f }, { 0.60f, 0.85f, 1.00f }, 1.10f, 0.80f },

    // CGA — punchy cyan that snaps toward magenta on bass; full saturation.
    { "CGA", 0.500f, -0.33f, 0.00f, 0.500f, 0.850f, 1.00f,
      0.830f, 0.500f, { 1.00f, 1.00f, 1.00f }, { 0.80f, 0.90f, 1.00f }, 1.35f, 0.85f },

    // INFRARED — pure-red P22 monochrome; "night ops" thermal glow, stays red.
    { "INFRARED", 0.004f, 0.008f, 0.012f, 0.000f, 0.022f, 0.95f,
      0.010f, 0.060f, { 1.14f, 0.74f, 0.72f }, { 1.00f, 0.36f, 0.34f }, 0.80f, 1.25f },

    // SODIUM — vivid street-lamp orange; clearly hotter/oranger than AMBER's gold.
    { "SODIUM", 0.075f, 0.010f, 0.020f, 0.060f, 0.100f, 1.00f,
      0.040f, 0.120f, { 1.10f, 0.80f, 0.45f }, { 1.00f, 0.58f, 0.15f }, 0.95f, 1.20f },

    // COBALT — saturated electric blue; drifts toward indigo on highs.
    { "COBALT", 0.625f, 0.05f, 0.04f, 0.580f, 0.680f, 0.95f,
      0.000f, 0.660f, { 0.82f, 0.90f, 1.12f }, { 0.45f, 0.65f, 1.00f }, 1.20f, 0.90f },

    // VIOLET — synthwave purple that swings magenta (bass) to blue (highs).
    { "VIOLET", 0.780f, -0.07f, -0.09f, 0.700f, 0.870f, 0.95f,
      0.830f, 0.660f, { 1.00f, 0.82f, 1.10f }, { 0.80f, 0.50f, 1.00f }, 1.50f, 1.05f },

    // PAPER WHITE — near-white P4 TV monochrome; hue washed out by low saturation.
    { "PAPER WHITE", 0.580f, 0.00f, 0.00f, 0.550f, 0.600f, 0.10f,
      0.000f, 0.620f, { 0.98f, 1.00f, 1.04f }, { 0.85f, 0.92f, 1.00f }, 1.00f, 0.50f },
};

static constexpr int numThemes = (int) (sizeof (kThemes) / sizeof (kThemes[0]));

inline const Theme& get (int index) noexcept
{
    const int i = ((index % numThemes) + numThemes) % numThemes;
    return kThemes[i];
}

} // namespace Themes
