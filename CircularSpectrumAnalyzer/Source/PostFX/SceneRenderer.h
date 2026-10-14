#pragma once

#include <JuceHeader.h>

// GPU scene renderer: draws the full topo scene (background gradient, radar
// grid, valley mist, 60-level isolines, chromatic aberration) into the scene
// FBO with a single fullscreen fragment pass over the height-field texture,
// replacing the CPU marching-squares tracer + path stroking.
//
// Every method must be called on the GL render thread, between
// newOpenGLContextCreated() and openGLContextClosing().
class SceneRenderer
{
public:
    SceneRenderer() = default;

    // Height-composition inputs (Landscape mode). Static planes upload only when
    // their dirty flags say so; minDist2 uploads every frame.
    struct ComposeInputs
    {
        int gridSize = 0;
        bool baseDirty = false;                        // base + peakMask re-upload (reseed)
        const float* base       = nullptr;             // gridSize^2
        const float* peakMask   = nullptr;             // gridSize^2
        juce::uint32 planeDirtyBits = 0;               // bit pk => planes[pk] holds fresh data
        const std::vector<float>* planes = nullptr;    // array of numPeaks vectors, gridSize^2 each
        int numPeaks = 0;
        const float* peakAmps = nullptr;               // numPeaks, premultiplied by heightScale
        const float* minDist2 = nullptr;               // gridSize^2, every frame
        float dynDepth = 0.0f;
        float sigma    = 1.0f;
    };

    // Per-frame inputs, all snapshot-derived (no model access from the GL thread).
    struct Frame
    {
        int   gridSize       = 0;
        float heightFieldMax = 1.0f;
        float hue            = 0.376f;
        float saturation     = 1.0f;         // per-theme global HSV saturation multiplier
        float caLowHue       = 0.0f;         // per-theme chromatic-aberration fringe hues
        float caHighHue      = 0.62f;
        float shadeStrength  = 0.18f;        // hillshade master gain (UI slider)
        float slopeK         = 12.0f;        // height-to-grid slope scale (UI slider)
        float hatchStrength  = 0.65f;        // slope-hachure gain (UI slider)
        float stippleStrength = 0.045f;      // hypsometric stipple gain (UI slider)
        float bassEnergy     = 0.0f;
        float overallEnergy  = 0.0f;         // song intensity (analog beam scaling)
        float centroidNorm   = 0.0f;
        float animTime       = 0.0f;
        float logicalW      = 600.0f;
        float logicalH       = 600.0f;
        float renderScale    = 1.0f;         // physical px per logical px
        bool  hasRiver       = false;        // mask FBO holds this frame's river polygon

        // Map-box placement (from ScreenLayout — the single source of truth; the
        // renderer no longer recomputes the old centred-letterbox transform).
        float mapScale       = 1.0f;         // grid units -> logical px
        float mapOffX        = 0.0f;         // grid origin, logical px
        float mapOffY        = 0.0f;

        // Contour label gaps: capsule endpoints in map-local logical px (segment
        // p0->p1 packed as x0,y0,x1,y1) with a per-capsule radius. Punched out of
        // the contour lines in the scene shader so labels read cleanly.
        static constexpr int maxGaps = 32;
        int gapCount = 0;
        std::array<float, 4 * maxGaps> gapSeg {};
        std::array<float, maxGaps>     gapHalfW {};
        std::array<float, maxGaps>     gapLevel {};   // contour index k each label sits on
    };

    // Textured glyph quads (labels + clutter), built per frame on the GL thread
    // from the snapshot's label records. Positions in logical px (y-down).
    struct GlyphVertex
    {
        float x, y;      // logical px
        float u, v;      // atlas UV
        float alpha;     // per-glyph fade / twinkle
    };

    struct GlyphDraw
    {
        int first = 0, count = 0;   // vertex range (GL_TRIANGLES)
        juce::Colour colour;
    };

    struct GlyphBatch
    {
        std::vector<GlyphVertex> verts;
        std::vector<GlyphDraw>   draws;

        void clear() { verts.clear(); draws.clear(); }
    };

    // River strokes + glitch bands as triangle-strip ribbons, built per frame on
    // the GL thread from the snapshot's river geometry.
    struct RibbonVertex
    {
        float x, y;          // logical px, y-down
        float across;        // -1..+1 across the strip
        float halfWPx;       // half-width in physical px (feathering)
    };

    struct RibbonDraw
    {
        int first = 0, count = 0;   // vertex range (triangle strip)
        juce::Colour colour;
    };

    struct RibbonBatch
    {
        std::vector<RibbonVertex> verts;
        std::vector<RibbonDraw>   draws;         // drawn in order, alpha-blended
        int fillFirst = 0, fillCount = 0;        // river fill strip (mask source)

        void clear() { verts.clear(); draws.clear(); fillFirst = fillCount = 0; }
    };

    // Stages the glyph-atlas pixels (single-channel, row 0 = top) for upload on
    // the GL thread. Call before the context is created; the copy is retained so
    // context recreation re-uploads. w*h bytes expected.
    void setAtlas (int w, int h, const std::vector<juce::uint8>& pixels);

    void initialise (juce::OpenGLContext& ctx);
    void shutdown();

    // Full-res river-interior mask target (white = inside river). Reallocated on
    // resize; caller paints the river polygon into it before renderScene().
    juce::OpenGLFrameBuffer& getMaskBuffer (juce::OpenGLContext& ctx, int wPx, int hPx);

    // GPU height composition: uploads any dirty planes, then renders
    // base + peaks + river depression into the height texture (R32F FBO).
    void composeHeightField (const ComposeInputs& in);

    // Renders the scene pass into sceneFBO from the height texture already produced
    // by composeHeightField(). Writes every pixel (no prior clear needed), blending off.
    void renderScene (const Frame& f,
                      juce::OpenGLFrameBuffer& sceneFBO,
                      juce::OpenGLFrameBuffer& maskFBO);

    // Uploads this frame's ribbon batch into the stream VBO. Call once per frame
    // before renderRiverMask / drawRibbons.
    void uploadRibbons (const RibbonBatch& batch);

    // Clears the mask FBO and renders the river fill strip in solid white.
    void renderRiverMask (const Frame& f, const RibbonBatch& batch,
                          juce::OpenGLFrameBuffer& maskFBO);

    // Alpha-blends every ribbon draw (river strokes, then glitch bands) into
    // sceneFBO, scissored to the map box like the CPU path's clip.
    void drawRibbons (const Frame& f, const RibbonBatch& batch,
                      juce::OpenGLFrameBuffer& sceneFBO);

    // Uploads this frame's glyph batch and alpha-blends every glyph draw into
    // sceneFBO. Map labels pass clipToMap = true (scissored to the map box like
    // the ribbons); the terminal screen UI passes false to draw anywhere on the
    // tube. Draw last, over the contours, before post-FX.
    void drawGlyphs (const Frame& f, const GlyphBatch& batch,
                     juce::OpenGLFrameBuffer& sceneFBO, bool clipToMap = true);

    // Last shader compile/link error from initialise(), empty when all programs built.
    // Non-empty means a program is null and its pass silently no-ops (black screen) —
    // callers can surface this on the terminal UI instead of leaving no diagnostic.
    const juce::String& getLastShaderError() const noexcept { return lastShaderError; }

private:
    void drawQuad (juce::OpenGLShaderProgram& prog);
    void bindRibbonState (const Frame& f, juce::OpenGLFrameBuffer& target);
    void drawRibbonRange (int first, int count, juce::Colour colour);

    void ensureHeightTexture (int gridSize);
    unsigned int makeFieldTexture (int w, int h, unsigned int internalFormat,
                                   bool nearest);

    void bindGlyphState (const Frame& f, juce::OpenGLFrameBuffer& target, bool clipToMap);

    // Enables GL_SCISSOR_TEST clipped to the map box (same rect the CPU path clipped to),
    // shared by the ribbon and glyph passes. target supplies the framebuffer height for the
    // GL bottom-left flip.
    void applyMapBoxScissor (const Frame& f, juce::OpenGLFrameBuffer& target);

    std::unique_ptr<juce::OpenGLShaderProgram> sceneProg, ribbonProg, composeProg, glyphProg;
    juce::String lastShaderError;   // set by initialise() on any compile/link failure
    juce::OpenGLFrameBuffer maskFBO;

    unsigned int heightTex = 0, quadVAO = 0, quadVBO = 0;
    unsigned int ribbonVAO = 0, ribbonVBO = 0;
    unsigned int glyphVAO = 0, glyphVBO = 0, glyphAtlasTex = 0;
    unsigned int heightFBO = 0;                       // raw R32F target (JUCE FBOs are RGBA8)
    unsigned int baseTex = 0, peakMaskTex = 0, minDist2Tex = 0, peakAtlasTex = 0;
    size_t ribbonVBOCapacity = 0;   // bytes currently allocated (orphaned each frame)
    size_t glyphVBOCapacity = 0;    // bytes currently allocated (orphaned each frame)

    // Glyph atlas pixels retained on the message thread, uploaded (and re-uploaded
    // after context loss) by initialise() on the GL thread.
    std::vector<juce::uint8> atlasPixels;
    int atlasW = 0, atlasH = 0;
    bool atlasUploaded = false;
    int heightTexSize = 0;          // allocated side length, 0 = not yet created
    int composeTexSize = 0;         // allocated side length of the compose inputs
    int atlasPlanes = 0;
    int maskW = 0, maskH = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SceneRenderer)
};
