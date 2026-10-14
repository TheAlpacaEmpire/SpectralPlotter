#pragma once

// GLSL sources for the CRT/phosphor post-processing chain. Written in GLSL 1.1
// style (attribute/varying/texture2D, no #version) and run through
// juce::OpenGLHelpers::translate*ShaderToV3() at compile time, so they work on
// whatever profile the driver hands JUCE.
namespace PostFXShaders
{

// Shared fullscreen-quad vertex shader: aPosition in clip space, UV derived.
constexpr const char* quadVertex = R"(
attribute vec2 aPosition;
varying vec2 vUV;

void main()
{
    vUV = aPosition * 0.5 + 0.5;
    gl_Position = vec4 (aPosition, 0.0, 1.0);
}
)";

// Phosphor persistence: accum = max (scene, prevAccum * decay - epsilon).
// Green decays slower than red/blue for a P31-phosphor feel. The 1/255 epsilon
// matters: the accum FBO is RGBA8, and x * decay rounds back to x for small x,
// which would leave permanent ghosts without it.
constexpr const char* persistenceFrag = R"(
varying vec2 vUV;
uniform sampler2D uScene;
uniform sampler2D uPrev;
uniform vec3 uDecay;

void main()
{
    vec3 scene = texture2D (uScene, vUV).rgb;
    vec3 prev  = texture2D (uPrev,  vUV).rgb * uDecay - vec3 (1.0 / 255.0);
    gl_FragColor = vec4 (max (scene, max (prev, vec3 (0.0))), 1.0);
}
)";

// Bloom bright pass: keep only what exceeds the threshold, renormalised.
constexpr const char* brightPassFrag = R"(
varying vec2 vUV;
uniform sampler2D uTex;
uniform float uThreshold;

void main()
{
    vec3 c = texture2D (uTex, vUV).rgb;
    gl_FragColor = vec4 (max (c - vec3 (uThreshold), vec3 (0.0))
                             / max (1.0 - uThreshold, 1e-4), 1.0);
}
)";

// Separable Gaussian: 9-tap kernel folded into 5 linear-filtered fetches.
// uDelta = (1/w, 0) for the horizontal pass, (0, 1/h) for the vertical.
constexpr const char* gaussianFrag = R"(
varying vec2 vUV;
uniform sampler2D uTex;
uniform vec2 uDelta;

void main()
{
    vec2 o1 = uDelta * 1.3846154;
    vec2 o2 = uDelta * 3.2307692;
    vec3 acc = texture2D (uTex, vUV).rgb * 0.227027;
    acc += (texture2D (uTex, vUV + o1).rgb + texture2D (uTex, vUV - o1).rgb) * 0.3162162;
    acc += (texture2D (uTex, vUV + o2).rgb + texture2D (uTex, vUV - o2).rgb) * 0.0702703;
    gl_FragColor = vec4 (acc, 1.0);
}
)";

// Plain blit — used to downsample the half-res blur into the quarter-res chain
// (the destination FBO's linear filtering does the downsampling).
constexpr const char* copyFrag = R"(
varying vec2 vUV;
uniform sampler2D uTex;

void main()
{
    gl_FragColor = vec4 (texture2D (uTex, vUV).rgb, 1.0);
}
)";

// Final composite to screen: barrel distortion, analog drift, per-scanline
// jitter, two-tier bloom add, scanlines, phosphor slot mask, vignette, grain,
// green phosphor bias and global flicker.
constexpr const char* compositeFrag = R"(
varying vec2 vUV;
uniform sampler2D uAccum;
uniform sampler2D uBloomTight;
uniform sampler2D uBloomWide;
uniform vec2  uResolution;
uniform float uTime;
uniform float uBloomIntensity;
uniform float uBarrel;
uniform float uDriftAmpPx;
uniform float uJitterAmpPx;
uniform float uJitterGate;
uniform float uFlicker;
uniform float uScanSpacingPx;
uniform float uScanAlpha;
uniform float uMaskStrength;
uniform float uGrain;
uniform float uEnergy;          // overall song energy: excites the phosphor grain
uniform float uSparkAmount;
uniform float uSparkGain;
uniform float uVigStart;
uniform float uVigEdge;
uniform float uBoxHalfDiagPx;
uniform vec2  uTubeCentreUV;    // CRT tube (map box) centre, UV space (y-up)
uniform vec3  uPhosphorTint;    // per-theme phosphor bias (green tube = 0.93,1.04,0.95)
uniform vec3  uGrainTint;       // per-theme grain + spark tint
uniform sampler2D uHalation;    // wide low-threshold glow-diffusion (glass scatter)
uniform float uHalationIntensity;
uniform vec3  uHalationTint;    // halo colour, follows the active theme
uniform float uConvergencePx;   // max R/B raster split at the tube edge, physical px
uniform float uRollOffset;      // vertical-hold slip on a beat (fraction of screen height)
uniform float uDegauss;         // degauss purity-ripple amplitude on a beat (0..1, decaying)

float hash21 (vec2 p)
{
    return fract (sin (dot (p, vec2 (127.1, 311.7))) * 43758.5453);
}

// Sparse glowing dots, like tiny electrical discharges. The screen is diced into
// cells; only a fraction ever spark, and each active cell flashes on its own slow,
// desynced cycle -- so dots appear at random times, brighten and fade smoothly,
// and drift a couple of pixels while lit, rather than all strobing together.
float sparks (vec2 px, float t)
{
    const float cellPx = 40.0;
    vec2  cell = floor (px / cellPx);
    vec2  f    = fract (px / cellPx) * cellPx;         // position within cell, px

    // only a fraction of cells ever spark
    if (hash21 (cell + 17.3) < 1.0 - uSparkAmount)
        return 0.0;

    float period = mix (1.5, 5.0, hash21 (cell + 1.7));  // seconds between flashes
    float tp     = t / period + hash21 (cell + 5.1);
    float phase  = fract (tp);                            // 0..1 within this cycle
    float cyc    = floor (tp);                            // which flash we're on

    // smooth rise/fall over a short lit window; dark for the rest of the cycle
    const float dur = 0.16;                               // lit fraction of cycle
    if (phase > dur)
        return 0.0;
    float env = sin (phase / dur * 3.14159265);          // 0 -> 1 -> 0
    env *= env;                                          // sharpen the peak

    // dot centre: fresh per flash, kept off the cell edges so it never clips,
    // plus a few px of drift over the flash's lifetime
    vec2 c0    = (0.25 + 0.5 * vec2 (hash21 (cell + cyc * 2.3),
                                     hash21 (cell + cyc * 4.9))) * cellPx;
    vec2 drift = (vec2 (hash21 (cell + cyc * 7.1), hash21 (cell + cyc * 8.7)) - 0.5)
                 * 4.0 * phase;
    vec2 d = f - (c0 + drift);

    return env * exp (-dot (d, d) * 0.6);                 // tight glowing core
}

void main()
{
    // subtle barrel distortion (curved tube face, centred on the map box)
    vec2 cc = vUV - uTubeCentreUV;
    vec2 uv = uTubeCentreUV + cc * (1.0 + uBarrel * dot (cc, cc));

    // slow analog drift: the whole image sways sub-pixel amounts
    uv.x += sin (uTime * 0.31 + uv.y * 2.7) * uDriftAmpPx / uResolution.x;
    uv.y += sin (uTime * 0.23) * uDriftAmpPx * 0.5 / uResolution.y;

    // vertical-hold roll: a beat slips the picture vertically; it wraps and settles
    // back to lock as the envelope decays. A dark band (the blanking interval) rides
    // the wrap seam like a real tube losing sync.
    float yroll = uv.y + uRollOffset;
    uv.y = fract (yroll);
    float eSeam = fract (yroll);
    float blank = 1.0 - 0.9 * step (0.0015, uRollOffset)
                            * (smoothstep (0.02, 0.0, eSeam) + smoothstep (0.98, 1.0, eSeam));

    // degauss: a brief radial purity ripple radiating from the tube centre on a beat
    vec2  dgv   = uv - uTubeCentreUV;
    float dgD   = length (dgv);
    uv += (dgv / max (dgD, 1e-4)) * sin (dgD * 42.0 - uTime * 26.0) * uDegauss * 0.004;

    // occasional per-scanline horizontal tear
    float line = floor (uv.y * uResolution.y);
    float gate = step (uJitterGate, hash21 (vec2 (line * 0.37, floor (uTime * 7.0))));
    uv.x += (hash21 (vec2 (line, floor (uTime * 24.0))) - 0.5)
            * gate * uJitterAmpPx / uResolution.x;

    // RGB convergence: the beams misalign radially, worsening toward the tube edge.
    // Sample red pulled outward and blue pulled inward; green stays put.
    vec2 conv = (uv - uTubeCentreUV) * (2.0 * uConvergencePx / uResolution);
    vec3 col;
    col.r = texture2D (uAccum, uv + conv).r;
    col.g = texture2D (uAccum, uv).g;
    col.b = texture2D (uAccum, uv - conv).b;

    col += texture2D (uBloomTight, uv).rgb * uBloomIntensity;
    col += texture2D (uBloomWide,  uv).rgb * uBloomIntensity * 0.45;

    // halation: wide soft glass-scatter halo, tinted to the theme
    col += texture2D (uHalation, uv).rgb * uHalationIntensity * uHalationTint;

    // black outside the (barrel-curved) tube face
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        col = vec3 (0.0);

    vec2 px = uv * uResolution;

    // scanlines (was CPU: 1.5px black bands at 30% alpha every 3px)
    float scan = 1.0 - uScanAlpha * (0.5 + 0.5 * sin (px.y * 6.2831853 / uScanSpacingPx));

    // phosphor slot mask, ~3px period, deliberately faint
    float mask = 1.0 - uMaskStrength * (0.5 + 0.5 * sin (px.x * 2.0943951));

    // vignette (was CPU: 12-stop eased radial gradient over the map box)
    float r = length (px - uTubeCentreUV * uResolution) / uBoxHalfDiagPx;
    float t = clamp ((r - uVigStart) / (1.0 - uVigStart), 0.0, 1.0);
    float vig = 1.0 - t * t * (3.0 - 2.0 * t) * uVigEdge;

    col *= scan * mask * vig * blank;

    float lum = clamp (dot (col, vec3 (0.299, 0.587, 0.114)), 0.0, 1.0);

    // Phosphor speckle, EXCITED BY THE MUSIC. The green grain is the tube's coating
    // fizzing under the beam: it's anchored to the tube face (floor(px)) and shimmers
    // rather than boils, but louder audio makes it re-roll faster and glow brighter
    // (uEnergy), and it clusters in the afterglow -- excited-but-unsaturated phosphor
    // grains the most, while dead black and bright cores grain less. Two scales (fine
    // grain + coarse clumps) give it body; tinted phosphor-green.
    float shimmerHz = 2.0 + 7.0 * uEnergy;                // slow shimmer quiet, faster when loud
    float sTb   = floor (uTime * shimmerHz);
    float sp1   = hash21 (floor (px) + sTb * 0.017);      // fine grain
    float sp2   = hash21 (floor (px * 0.5) + 7.3);        // coarse clumping
    float sp3   = hash21 (floor (px) * 0.37 + sTb * 0.023 + 19.0);
    float speck = pow (sp1 * (0.55 + 0.45 * sp2), 2.0)    // dense fine grains
                + 0.5 * pow (sp3, 3.0);                   // scattered brighter flecks
    // excitation profile: half-strength over dead black, full in the dim afterglow,
    // fading out on the bright trace cores.
    float exc   = (0.5 + 0.5 * smoothstep (0.0, 0.05, lum))
                * (1.0 - smoothstep (0.35, 0.75, lum));
    float amp   = uGrain * (0.7 + 1.1 * uEnergy);         // grain breathes with the music
    col += speck * amp * exc * uGrainTint;

    // sparse electrical-discharge sparks, tinted like the phosphor
    col += sparks (px, uTime) * uSparkGain * uGrainTint;

    // phosphor colour bias + slight global brightness flicker
    col *= uPhosphorTint;
    col *= 1.0 - uFlicker * hash21 (vec2 (floor (uTime * 48.0), 3.7));

    gl_FragColor = vec4 (col, 1.0);
}
)";

} // namespace PostFXShaders
