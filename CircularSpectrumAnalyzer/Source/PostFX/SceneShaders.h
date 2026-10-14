#pragma once

// GLSL sources for the GPU scene renderer (background, radar grid, valley mist,
// topo isolines, chromatic aberration). Written in the same GLSL 1.1 idiom as
// Shaders.h and run through juce::OpenGLHelpers::translate*ShaderToV3().
//
// The isoline pass replaces the CPU marching-squares tracer: contour levels are
// gamma-spaced as level = ((lev+1)/61)^1.25 * hMax, so in the transformed space
// t = (h/hMax)^(1/1.25) * 61 every contour sits exactly on an integer t = 1..60.
// That makes the whole 60-level ladder O(1) per fragment: distance to the nearest
// contour in pixels is |t - round(t)| / fwidth(t).
namespace SceneShaders
{

constexpr const char* sceneFrag = R"(
varying vec2 vUV;
uniform sampler2D uHeight;      // gridSize^2 height field; texel (x,y) = grid vertex, y-down
uniform sampler2D uRiverMask;   // full-res; r=1 inside the river polygon
uniform vec2  uLogicalSize;     // component size in logical px
uniform float uRenderScale;     // physical px per logical px
uniform vec2  uBoxOffset;       // map-box top-left, logical px
uniform float uBoxScale;        // logical px per grid cell
uniform float uGridSize;        // grid vertices per side
uniform float uHMax;            // contour ceiling (heightFieldMax)
uniform float uHue;             // live neon hue
uniform float uSaturation;      // per-theme global HSV saturation multiplier
uniform float uCaLowHue;        // per-theme chromatic-aberration fringe hues
uniform float uCaHighHue;
uniform float uBass;            // bass energy (mist thickness)
uniform float uEnergy;          // overall song energy (analog beam intensity)
uniform float uTime;            // scene clock (analog beam wander / hot spots)
uniform float uAberPx;          // chromatic aberration offset, logical px
uniform float uHasRiver;        // 1 = mask holds a river polygon
uniform float uMinGrad;         // plateau guard: fade contours where fwidth(t) < this
uniform vec3  uLightDir;        // hillshade light, normalized, y-down, z toward viewer
uniform float uShadeStrength;   // hillshade master gain
uniform float uSlopeK;          // height-to-grid-unit slope scale
uniform float uHatchStrength;   // hachure mix gain (0 disables)
uniform float uStippleStrength; // stipple mix gain (0 disables)

// Label gaps: contour lines are punched out under each map label so the digits
// read cleanly. Each gap is an oriented box in map-local logical px (centre line
// p0->p1, half-height uGapHalfW): the labelled contour gets a clean, square-ended
// break; neighbouring contours only fade inside the text box itself, so no
// rounded "pill" is carved out of the surrounding linework. uGapCount <= 32.
uniform vec4  uGapSeg[32];       // (p0.xy, p1.xy)
uniform float uGapHalfW[32];
uniform float uGapLevel[32];     // contour index k the label sits on
uniform int   uGapCount;

vec3 hsv (float h, float s, float v)
{
    s = clamp (s * uSaturation, 0.0, 1.0);   // per-theme global saturation
    vec3 p = abs (fract (vec3 (h) + vec3 (0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return v * mix (vec3 (1.0), clamp (p - 1.0, 0.0, 1.0), s);
}

float hash21 (vec2 p)
{
    return fract (sin (dot (p, vec2 (127.1, 311.7))) * 43758.5453);
}

// Smooth 2D value noise in [0,1] (used for the analog beam intensity along traces).
float vnoise (vec2 p)
{
    vec2 i = floor (p), f = fract (p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21 (i);
    float b = hash21 (i + vec2 (1.0, 0.0));
    float c = hash21 (i + vec2 (0.0, 1.0));
    float d = hash21 (i + vec2 (1.0, 1.0));
    return mix (mix (a, b, f.x), mix (c, d, f.x), f.y);
}

// Contour-space transform: integers of the result are the 60 CPU contour levels.
// Only the lower bound is clamped: a peak whose flash overshoot pushes it above
// the (deliberately slow) uHMax ceiling keeps generating contours past level 61
// instead of saturating into one flat, bald summit cap. The pow() gamma stays
// real and monotone for h/uHMax > 1, so rings above the ceiling just space out.
float levelSpace (float h)
{
    return pow (max (h / uHMax, 0.0), 0.8) * 61.0;   // 0.8 = 1/1.25
}

void main()
{
    // Logical-pixel position, y-down like the JUCE Graphics code this replaces.
    vec2 pLog  = vec2 (vUV.x, 1.0 - vUV.y) * uLogicalSize;
    vec2 local = pLog - uBoxOffset;
    vec2 gridP = local / uBoxScale;
    vec2 uvH   = (gridP + 0.5) / uGridSize;

    float boxSize     = uBoxScale * (uGridSize - 1.0);
    float boxHalfDiag = boxSize * 0.70710678;

    // --- everything derivative-dependent runs before any branching ---
    float h    = texture2D (uHeight, uvH).r;
    float t    = levelSpace (h);
    float grad = max (fwidth (t), 1e-4);
    float notRiver = 1.0 - ((uHasRiver > 0.5) ? texture2D (uRiverMask, vUV).r : 0.0);

    // --- terrain slope (shared by hillshade, hachures, stipple) ---
    float e   = 0.5 / uGridSize;                 // half-texel taps smooth bilinear facets
    float dhx = texture2D (uHeight, uvH + vec2 (e, 0.0)).r
              - texture2D (uHeight, uvH - vec2 (e, 0.0)).r;
    float dhy = texture2D (uHeight, uvH + vec2 (0.0, e)).r
              - texture2D (uHeight, uvH - vec2 (0.0, e)).r;
    vec2  gvec   = vec2 (dhx, dhy) * uSlopeK;
    float sMag   = length (gvec);
    vec2  tPerp  = vec2 (-gvec.y, gvec.x) / max (sMag, 1e-5);  // contour tangent, NaN-safe
    float hNorm  = clamp (h / uHMax, 0.0, 1.0);
    // Partition divider: steepness as on-screen contour crowding (contours closer
    // than ~12 logical px count as steep, fully steep at ~5 px). grad is per
    // physical px, so scale back to logical.
    float slopeGate = smoothstep (0.08, 0.20, grad * uRenderScale);

    // CA neighbours: a stroke translated by +d puts a line at p when the contour
    // is at p - d, so sample the field offset in the opposite direction.
    float aberGrid = uAberPx / uBoxScale;
    float tR = levelSpace (texture2D (uHeight, uvH - vec2 (aberGrid / uGridSize, 0.0)).r);
    float tB = levelSpace (texture2D (uHeight, uvH + vec2 (aberGrid / uGridSize, 0.0)).r);

    // --- isoline classification ---
    float k     = floor (t + 0.5);
    float valid = (k >= 1.0) ? 1.0 : 0.0;   // no upper cap: rings continue above the ceiling
    float flatFade = smoothstep (0.0, uMinGrad, grad);   // no plateau-wide lines on flat field

    int  lev     = int (k) - 1;
    bool isIndex = (lev - (lev / 7) * 7) == 0;           // every 7th level, brighter

    // 80s vector-display wobble: every 1/20 s each contour snaps to a fresh random
    // offset along its normal (an unstable DAC / deflection amp), in constant physical
    // px whatever the slope (t-space offset = px * grad). Thin lines wobble most, index
    // lines stay steadier; louder music shakes harder. Stepped, never smooth.
    float jit   = (hash21 (vec2 (k * 1.37, floor (uTime * 20.0))) - 0.5) * 2.0;
    float jitPx = (isIndex ? 0.15 : 0.45) * (0.6 + 0.8 * uEnergy) * uRenderScale;
    float dPx   = abs (t + jit * jitPx * grad - k) / grad;   // distance to contour, physical px
    int  bucket  = lev / 10;                             // 6 brightness buckets

    float hueB  = fract (uHue + (float (bucket) - 2.5) * 0.015 + 1.0);
    vec3  cLine = hsv (hueB, 1.0, 0.70 + 0.06 * float (bucket));

    // --- analog beam de-uniforming (scales with song intensity) ----------------
    // Real plotter/CRT traces are never uniform: beam energy wanders along the
    // line, and every so often a bright "hot spot" flares up a little thicker and
    // hotter, crawling slowly along the trace. Quiet passages keep the traces
    // nearly uniform; louder passages widen the wander and throw brighter, thicker,
    // more frequent flares. sAlong is the along-contour arc coordinate (same
    // projection the hachures use); beamK decorrelates adjacent contours.
    float amt    = clamp (0.12 + uEnergy * 1.1, 0.0, 1.0);              // 0.12 idle .. 1.0 loud
    float sAlong = dot (local, tPerp);                                  // along-trace, logical px
    float beamK  = k * 2.3;
    float wLow   = vnoise (vec2 (sAlong * 0.028 + beamK, uTime * 0.22 + beamK));   // slow dwell
    float wCrawl = vnoise (vec2 (sAlong * 0.075 - uTime * 0.9, beamK * 0.5 + 5.0)); // crawls along
    float beam   = 1.0 + (0.10 + 0.34 * amt) * (wLow - 0.5) * 2.0       // ~uniform when quiet
                       + (0.05 + 0.16 * amt) * (wCrawl - 0.5) * 2.0;
    float hsN    = vnoise (vec2 (sAlong * 0.045 - uTime * 0.6, beamK + 11.0));
    float hotThr = mix (0.90, 0.66, amt);                              // rare when quiet, frequent when loud
    float hot    = smoothstep (hotThr, hotThr + 0.16, hsN) * (0.25 + 0.85 * amt);
    float beamBright = max (0.25, beam) + hot;                          // hot spots punch brighter

    // CPU strokes were logical-px widths under a renderScale transform.
    float widen = (hot * 0.7 + (wLow - 0.5) * 0.15 * amt) * uRenderScale;   // flares fatten the core
    float coreW = (isIndex ? 0.7 : 0.35) * uRenderScale + widen;       // half-widths, physical px
    float glowW = (isIndex ? 2.5 : 1.5)  * uRenderScale + hot * 1.6 * uRenderScale;
    vec3  cCore = clamp (cLine * beamBright, 0.0, 1.4);                 // brighten, don't over-mix
    float aCore = (isIndex ? 0.90 : 0.60) * (1.0 - smoothstep (coreW - 0.5, coreW + 0.5, dPx));
    float aGlow = (isIndex ? 0.07 : 0.05) * (0.6 + 0.9 * beamBright)
                * (1.0 - smoothstep (glowW - 0.5, glowW + 0.5, dPx));
    float lineGate = valid * flatFade;

    // Chromatic aberration: thin red/blue fringes on the outer 8 levels (lev >= 52).
    float aberW = 0.6 * uRenderScale;
    float kR = floor (tR + 0.5);
    float kB = floor (tB + 0.5);
    float dR = abs (tR - kR) / grad;
    float dB = abs (tB - kB) / grad;
    float aR = ((kR >= 53.0 && kR <= 60.0) ? 0.07 : 0.0)
             * (1.0 - smoothstep (aberW - 0.5, aberW + 0.5, dR)) * flatFade;
    float aB = ((kB >= 53.0 && kB <= 60.0) ? 0.07 : 0.0)
             * (1.0 - smoothstep (aberW - 0.5, aberW + 0.5, dB)) * flatFade;

    // --- background: radial gradient, faint green centre to black ---
    vec2  centre = uBoxOffset + vec2 (boxSize * 0.5);   // map-box centre, not window centre
    float rad    = length (pLog - centre);
    float bgR    = boxHalfDiag * 1.1;
    vec3  col    = mix (vec3 (3.0, 8.0, 3.0) / 255.0, vec3 (0.0),
                        clamp (rad / bgR, 0.0, 1.0));

    vec3 kNeon = hsv (uHue, 1.0, 1.0);

    // --- hillshade: faint lambert relief between contours ---
    {
        vec3  N     = normalize (vec3 (-gvec, 1.0));
        float shade = dot (N, uLightDir) - uLightDir.z;   // == 0 on flat ground
        float m = smoothstep (0.06, 0.35, hNorm) * notRiver * uShadeStrength;
        col += hsv (uHue, 0.85, 1.0) * max (shade, 0.0) * m;   // sunlit: add hue
        col *= 1.0 + min (shade, 0.0) * m * 0.8;               // shaded: darken bg
    }

    // --- hypsometric stipple: dot-grid halftone, denser per elevation band ---
    {
        float cell   = 5.0;                                  // logical px
        vec2  cellId = floor (local / cell);
        vec2  g      = local - (cellId + 0.5) * cell;        // centered in-cell coords
        float d      = length (g);
        float aaW    = 1.0 / uRenderScale;                   // ~1 physical px, logical units
        float dotCov = 1.0 - smoothstep (0.65 - aaW, 0.65 + aaW, d);

        int   band    = int (clamp (hNorm, 0.0, 0.999) * 6.0);
        float density = 0.12 + 0.09 * float (band);          // higher band = denser
        float lit     = step (hash21 (cellId + 31.7), density);

        float gateS = (1.0 - slopeGate)                      // hachures own steep ground
                    * smoothstep (0.10, 0.18, hNorm)         // out of mist/river lowlands
                    * notRiver * flatFade;
        float aStip = uStippleStrength * (1.0 + 0.6 * uBass) * dotCov * lit * gateS;
        col = mix (col, hsv (uHue, 0.7, 0.85), aStip);
    }

    // --- slope hachures: down-slope ticks in the band below each contour ---
    {
        float spacing = 7.0;                                 // logical px between marks
        float s   = dot (local, tPerp);                      // along-contour coordinate
        float sw  = max (fwidth (s), 1e-4);
        float md  = abs (fract (s / spacing) - 0.5) * spacing;     // px to nearest stripe
        float stripe = 1.0 - smoothstep (0.5 - sw, 0.5 + sw, md);  // ~1 px wide marks

        float frac     = t - floor (t);
        float bandGate = smoothstep (0.40, 0.58, frac)       // upper ~55% of interval
                       - smoothstep (0.92, 1.0, frac);       // release just under the line
        float coreFade = smoothstep (0.8, 1.6, dPx / uRenderScale);   // clear line cores only

        float gateH = slopeGate * bandGate * coreFade
                    * smoothstep (0.12, 0.20, hNorm)         // no hatching in lowlands
                    * notRiver * flatFade;
        col = mix (col, hsv (uHue, 0.85, 0.90), uHatchStrength * stripe * gateH);
    }

    // --- military radar grid: dotted lines every 72 px + dotted concentric circles ---
    float aGrid = 0.0;
    vec2  lm    = mod (local, 72.0);
    if (lm.y < 1.0 && mod (local.x, 7.0) < 2.0) aGrid = 1.0;   // dashed horizontal rows
    if (lm.x < 1.0 && mod (local.y, 7.0) < 2.0) aGrid = 1.0;   // dashed vertical columns
    {
        float rk = 72.0 * floor (rad / 72.0 + 0.5);            // nearest ring
        if (rk >= 72.0 && abs (rad - rk) <= 1.0)
        {
            float n   = max (8.0, floor (6.2831853 * rk / 7.0));
            float ang = atan (pLog.y - centre.y, pLog.x - centre.x);
            if (ang < 0.0) ang += 6.2831853;
            float idx  = ang / 6.2831853 * n;
            float near = floor (idx + 0.5);
            float arcPx = abs (idx - near) * 6.2831853 / n * rk;
            if (mod (near, 2.0) < 0.5 && arcPx <= 1.0) aGrid = 1.0;   // every other dot lit
        }
    }
    col = mix (col, kNeon, 0.10 * aGrid);

    // --- valley mist: fill 2x2-vertex cells whose 4 corners are all below threshold ---
    {
        vec2 cell = floor (gridP / 2.0) * 2.0;
        if (cell.x >= 0.0 && cell.y >= 0.0 && cell.x <= 252.0 && cell.y <= 252.0)
        {
            float m00 = texture2D (uHeight, (cell + vec2 (0.5, 0.5)) / uGridSize).r;
            float m10 = texture2D (uHeight, (cell + vec2 (2.5, 0.5)) / uGridSize).r;
            float m01 = texture2D (uHeight, (cell + vec2 (0.5, 2.5)) / uGridSize).r;
            float m11 = texture2D (uHeight, (cell + vec2 (2.5, 2.5)) / uGridSize).r;
            if (max (max (m00, m10), max (m01, m11)) < 0.12)
            {
                float mistAlpha = 0.015 + 0.04 * uBass;
                vec3  mistC = hsv (fract (uHue + 0.5), 0.55, 0.8);
                col = mix (col, mistC, mistAlpha);
            }
        }
    }

    // --- label gaps: punch contours (and their CA fringes) out under labels ---
    float gapMask = 1.0;
    for (int i = 0; i < 32; ++i)
    {
        if (i >= uGapCount) break;
        vec2  a = uGapSeg[i].xy, b = uGapSeg[i].zw;
        vec2  ab = b - a;
        float L  = max (length (ab), 1e-5);
        vec2  u  = ab / L;
        vec2  q  = local - 0.5 * (a + b);
        vec2  qb = abs (vec2 (dot (q, u), dot (q, vec2 (-u.y, u.x))))
                 - vec2 (0.5 * L, uGapHalfW[i]);
        float sd = length (max (qb, 0.0)) + min (max (qb.x, qb.y), 0.0);   // box SDF, <0 inside

        if (abs (k - uGapLevel[i]) < 0.5)
            gapMask *= smoothstep (1.0, 2.5, sd);    // own contour: square break, ~2 px clearance
        else
            gapMask *= smoothstep (-2.0, 0.0, sd);   // neighbours: fade only within the text box
    }

    // --- contours + CA, clipped out of the river interior and label gaps ---
    col = mix (col, cCore, aGlow * lineGate * notRiver * gapMask);
    col = mix (col, cCore, aCore * lineGate * notRiver * gapMask);
    col = mix (col, hsv (uCaLowHue,  0.7, 1.0), aR * notRiver * gapMask);
    col = mix (col, hsv (uCaHighHue, 0.7, 1.0), aB * notRiver * gapMask);

    // --- map chrome: neatline, edge ticks, corner brackets ---
    {
        vec2  dEdge = min (local, vec2 (boxSize) - local);   // px to nearest edge, per axis
        float dMin  = min (dEdge.x, dEdge.y);
        float aChrome = 0.0;
        // neatline: 1 px frame inset 3 px
        if (abs (dMin - 3.0) < 0.5) aChrome = max (aChrome, 0.30);
        // ticks every 72 px (aligned with the radar grid), long tick (9 px) every 5th
        {
            float along   = (dEdge.x < dEdge.y) ? local.y : local.x;
            float tickIdx = floor (along / 72.0 + 0.5);
            float dTick   = abs (along - tickIdx * 72.0);
            float len     = (mod (tickIdx, 5.0) < 0.5) ? 9.0 : 5.0;
            if (dTick < 0.5 && dMin > 3.0 && dMin < 3.0 + len)
                aChrome = max (aChrome, 0.35);
        }
        // corner registration brackets: 16 px legs, 1.5 px wide, inset 6 px
        if (max (dEdge.x, dEdge.y) < 22.0)
        {
            bool leg = (abs (dEdge.x - 6.0) < 0.75 && dEdge.y >= 6.0)
                    || (abs (dEdge.y - 6.0) < 0.75 && dEdge.x >= 6.0);
            if (leg) aChrome = max (aChrome, 0.50);
        }
        col = mix (col, kNeon, aChrome);
    }

    // --- map-box clip: letterbox margins stay pure black ---
    if (local.x < 0.0 || local.y < 0.0 || local.x >= boxSize || local.y >= boxSize)
        col = vec3 (0.0);

    gl_FragColor = vec4 (col, 1.0);
}
)";

// Height-field composition, replacing TerrainModel's per-frame CPU loop:
//   height = base + pNorm3(amp_pk * peakWeight_pk) * riverPeakMask - riverDepression
// pNorm3 = cube root of the sum of cubes: a soft max, so two overlapping active
// peaks keep a visible saddle instead of summing into one fused mass (a lone peak
// is unchanged). Must match TerrainModel::sampleComposedHeight.
// Renders into the 256^2 R32F height FBO; vUV texel centers map 1:1 onto grid
// vertices. The peak planes live in a horizontal 10-plane atlas (u' = (u+pk)/10).
constexpr const char* heightComposeFrag = R"(
varying vec2 vUV;
uniform sampler2D uBase;        // static terrain incl. permanent river carve
uniform sampler2D uPeakAtlas;   // 10 x gridSize wide, one weight plane per slot
uniform sampler2D uPeakMask;    // 0 at river bed .. 1 at valley shoulder
uniform sampler2D uMinDist2;    // per-cell squared distance to the swayed backbone
uniform float uAmp[10];         // per-peak amplitude, premultiplied by heightScale
uniform float uDynDepth;        // dynamic valley depression depth
uniform float uInv2Sigma2;      // 1 / (2 * sigma^2)
uniform float uDepCut2;         // 9 * sigma^2 (3-sigma cutoff, matches CPU)

void main()
{
    float h    = texture2D (uBase, vUV).r;
    float mask = texture2D (uPeakMask, vUV).r;

    float acc = 0.0;
    for (int pk = 0; pk < 10; ++pk)
    {
        float c = max (uAmp[pk] * texture2D (uPeakAtlas, vec2 ((vUV.x + float (pk)) * 0.1, vUV.y)).r, 0.0);
        acc += c * c * c;
    }
    h += (acc > 0.0 ? pow (acc, 1.0 / 3.0) : 0.0) * mask;

    float r2 = texture2D (uMinDist2, vUV).r;
    if (r2 < uDepCut2)
        h -= uDynDepth * exp (-r2 * uInv2Sigma2);

    gl_FragColor = vec4 (h, 0.0, 0.0, 1.0);
}
)";

// River ribbon / glitch-band geometry: positions are logical px (y-down, same
// space as the old JUCE Graphics code); aEdge.x is the across-strip coordinate
// (-1 at the left edge, +1 at the right), aEdge.y the half-width in physical px.
constexpr const char* ribbonVertex = R"(
attribute vec2 aPos;
attribute vec2 aEdge;
uniform vec2 uLogicalSize;
varying vec2 vEdge;

void main()
{
    vEdge = aEdge;
    gl_Position = vec4 (aPos.x / uLogicalSize.x * 2.0 - 1.0,
                        1.0 - aPos.y / uLogicalSize.y * 2.0,
                        0.0, 1.0);
}
)";

// Coverage model matches an antialiased stroke: full alpha in the interior,
// ~1 px linear feather at the edges, and thin strokes (< 1 px total width)
// scale down by their width like a rasterized hairline would.
constexpr const char* ribbonFrag = R"(
varying vec2 vEdge;
uniform vec4 uColor;

void main()
{
    float halfW  = vEdge.y;
    float edgePx = (1.0 - abs (vEdge.x)) * halfW;   // distance to strip edge, px
    float cov    = clamp (edgePx + 0.5, 0.0, 1.0) * clamp (halfW * 2.0, 0.0, 1.0);
    gl_FragColor = vec4 (uColor.rgb, uColor.a * cov);
}
)";

// Map labels (contour elevations, spot heights, symbol clutter): textured quads
// from the glyph atlas, positions in logical px (y-down, same space as the
// ribbons). Drawn into the scene FBO before post-FX so they inherit the phosphor
// glow/persistence/barrel-distortion of everything else.
constexpr const char* glyphVertex = R"(
attribute vec2 aPos;
attribute vec2 aUV;
attribute float aAlpha;
uniform vec2 uLogicalSize;
varying vec2 vGUV;
varying float vGAlpha;

void main()
{
    vGUV    = aUV;
    vGAlpha = aAlpha;
    gl_Position = vec4 (aPos.x / uLogicalSize.x * 2.0 - 1.0,
                        1.0 - aPos.y / uLogicalSize.y * 2.0,
                        0.0, 1.0);
}
)";

constexpr const char* glyphFrag = R"(
varying vec2 vGUV;
varying float vGAlpha;
uniform sampler2D uAtlas;   // single-channel coverage
uniform vec4 uColor;

void main()
{
    float a = texture2D (uAtlas, vGUV).r;
    gl_FragColor = vec4 (uColor.rgb, uColor.a * a * vGAlpha);
}
)";

} // namespace SceneShaders
