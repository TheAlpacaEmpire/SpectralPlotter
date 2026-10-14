#include "SceneRenderer.h"
#include "SceneShaders.h"
#include "Shaders.h"
#include "GLProgram.h"

using namespace juce::gl;

//==============================================================================
void SceneRenderer::setAtlas (int w, int h, const std::vector<juce::uint8>& pixels)
{
    atlasW = w;
    atlasH = h;
    atlasPixels = pixels;
    atlasUploaded = false;   // GL thread re-uploads on next initialise()
}

//==============================================================================
void SceneRenderer::initialise (juce::OpenGLContext& ctx)
{
    glGenVertexArrays (1, &quadVAO);
    glGenBuffers (1, &quadVBO);

    glBindVertexArray (quadVAO);
    glBindBuffer (GL_ARRAY_BUFFER, quadVBO);
    static const float quad[] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f };
    glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_STATIC_DRAW);
    glBindVertexArray (0);
    glBindBuffer (GL_ARRAY_BUFFER, 0);

    auto makeProg = [&ctx, this] (const char* label, const char* vert, const char* frag)
    {
        return GLProgram::make (ctx, label, vert, frag, &lastShaderError);
    };

    sceneProg   = makeProg ("Scene",   PostFXShaders::quadVertex,  SceneShaders::sceneFrag);
    ribbonProg  = makeProg ("Ribbon",  SceneShaders::ribbonVertex, SceneShaders::ribbonFrag);
    composeProg = makeProg ("Compose", PostFXShaders::quadVertex,  SceneShaders::heightComposeFrag);
    glyphProg   = makeProg ("Glyph",   SceneShaders::glyphVertex,  SceneShaders::glyphFrag);

    glGenVertexArrays (1, &ribbonVAO);
    glGenBuffers (1, &ribbonVBO);
    ribbonVBOCapacity = 0;

    glGenVertexArrays (1, &glyphVAO);
    glGenBuffers (1, &glyphVBO);
    glyphVBOCapacity = 0;

    // Upload the glyph atlas (once per context). GL_R8, linear-filtered for the
    // minified label draw; row 0 is the atlas top (matches GlyphAtlas byte order).
    if (! atlasPixels.empty() && atlasW > 0 && atlasH > 0)
    {
        if (glyphAtlasTex == 0)
            glGenTextures (1, &glyphAtlasTex);
        glBindTexture (GL_TEXTURE_2D, glyphAtlasTex);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_R8, atlasW, atlasH, 0,
                      GL_RED, GL_UNSIGNED_BYTE, atlasPixels.data());
        // Mipmapped minification: the 48 px cells shrink ~4x for the terminal
        // text, and plain GL_LINEAR drops thin glyph stems at that ratio.
        glGenerateMipmap (GL_TEXTURE_2D);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture (GL_TEXTURE_2D, 0);
        atlasUploaded = true;
    }

    // context recreation lost every texture — force realloc + full re-upload
    heightTexSize  = 0;
    composeTexSize = 0;
    atlasPlanes    = 0;
    maskW = maskH  = 0;
}

void SceneRenderer::shutdown()
{
    sceneProg.reset();
    ribbonProg.reset();
    composeProg.reset();
    glyphProg.reset();
    maskFBO.release();

    if (heightFBO    != 0) { glDeleteFramebuffers (1, &heightFBO);   heightFBO    = 0; }
    if (heightTex    != 0) { glDeleteTextures (1, &heightTex);       heightTex    = 0; }
    if (baseTex      != 0) { glDeleteTextures (1, &baseTex);         baseTex      = 0; }
    if (peakMaskTex  != 0) { glDeleteTextures (1, &peakMaskTex);     peakMaskTex  = 0; }
    if (minDist2Tex  != 0) { glDeleteTextures (1, &minDist2Tex);     minDist2Tex  = 0; }
    if (peakAtlasTex != 0) { glDeleteTextures (1, &peakAtlasTex);    peakAtlasTex = 0; }
    if (quadVBO      != 0) { glDeleteBuffers (1, &quadVBO);          quadVBO      = 0; }
    if (quadVAO      != 0) { glDeleteVertexArrays (1, &quadVAO);     quadVAO      = 0; }
    if (ribbonVBO    != 0) { glDeleteBuffers (1, &ribbonVBO);        ribbonVBO    = 0; }
    if (ribbonVAO    != 0) { glDeleteVertexArrays (1, &ribbonVAO);   ribbonVAO    = 0; }
    if (glyphVBO     != 0) { glDeleteBuffers (1, &glyphVBO);         glyphVBO     = 0; }
    if (glyphVAO     != 0) { glDeleteVertexArrays (1, &glyphVAO);    glyphVAO     = 0; }
    if (glyphAtlasTex != 0){ glDeleteTextures (1, &glyphAtlasTex);   glyphAtlasTex = 0; }

    ribbonVBOCapacity = 0;
    glyphVBOCapacity = 0;
    atlasUploaded = false;
    heightTexSize = 0;
    composeTexSize = 0;
    atlasPlanes = 0;
    maskW = maskH = 0;
}

juce::OpenGLFrameBuffer& SceneRenderer::getMaskBuffer (juce::OpenGLContext& ctx, int wPx, int hPx)
{
    if (wPx != maskW || hPx != maskH)
    {
        maskW = wPx;
        maskH = hPx;
        maskFBO.initialise (ctx, wPx, hPx);
    }
    return maskFBO;
}

//==============================================================================
void SceneRenderer::drawQuad (juce::OpenGLShaderProgram& prog)
{
    glBindVertexArray (quadVAO);
    glBindBuffer (GL_ARRAY_BUFFER, quadVBO);
    const GLint loc = glGetAttribLocation (prog.getProgramID(), "aPosition");
    glEnableVertexAttribArray ((GLuint) loc);
    glVertexAttribPointer ((GLuint) loc, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray ((GLuint) loc);
    glBindVertexArray (0);
    glBindBuffer (GL_ARRAY_BUFFER, 0);
}

unsigned int SceneRenderer::makeFieldTexture (int w, int h, unsigned int internalFormat,
                                              bool nearest)
{
    unsigned int tex = 0;
    glGenTextures (1, &tex);
    glBindTexture (GL_TEXTURE_2D, tex);
    glTexImage2D (GL_TEXTURE_2D, 0, (GLint) internalFormat, w, h, 0,
                  GL_RED, GL_FLOAT, nullptr);
    const GLint filter = nearest ? GL_NEAREST : GL_LINEAR;
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

void SceneRenderer::ensureHeightTexture (int gridSize)
{
    if (heightTexSize == gridSize)
        return;
    if (heightTex != 0)
        glDeleteTextures (1, &heightTex);
    if (heightFBO != 0) { glDeleteFramebuffers (1, &heightFBO); heightFBO = 0; }

    // R32F: the field spans ~[-0.4, +2.5] and flat valley floors would shimmer
    // contours at half precision. Doubles as the compose pass's render target
    // (juce::OpenGLFrameBuffer is RGBA8-only, hence the raw FBO).
    heightTex = makeFieldTexture (gridSize, gridSize, GL_R32F, false);
    glGenFramebuffers (1, &heightFBO);
    glBindFramebuffer (GL_FRAMEBUFFER, heightFBO);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, heightTex, 0);
    jassert (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    heightTexSize = gridSize;
}

void SceneRenderer::composeHeightField (const ComposeInputs& in)
{
    if (composeProg == nullptr || in.gridSize <= 1 || in.numPeaks <= 0
        || in.peakAmps == nullptr || in.minDist2 == nullptr)
        return;

    const int gs = in.gridSize;
    ensureHeightTexture (gs);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);

    // (Re)create the compose input textures on first use / grid change.
    bool uploadStatics = in.baseDirty;
    juce::uint32 planeBits = in.planeDirtyBits;
    if (composeTexSize != gs || atlasPlanes != in.numPeaks)
    {
        uploadStatics = true;   // fresh textures know nothing
        planeBits = ~0u;
        if (baseTex      != 0) glDeleteTextures (1, &baseTex);
        if (peakMaskTex  != 0) glDeleteTextures (1, &peakMaskTex);
        if (minDist2Tex  != 0) glDeleteTextures (1, &minDist2Tex);
        if (peakAtlasTex != 0) glDeleteTextures (1, &peakAtlasTex);
        baseTex      = makeFieldTexture (gs, gs, GL_R32F, false);
        peakMaskTex  = makeFieldTexture (gs, gs, GL_R32F, false);
        minDist2Tex  = makeFieldTexture (gs, gs, GL_R32F, false);
        // NEAREST: the compose pass reads texel centers 1:1, and it kills any
        // cross-plane bleed at the atlas seams.
        peakAtlasTex = makeFieldTexture (gs * in.numPeaks, gs, GL_R16F, true);
        composeTexSize = gs;
        atlasPlanes    = in.numPeaks;
    }

    if (uploadStatics && in.base != nullptr && in.peakMask != nullptr)
    {
        glBindTexture (GL_TEXTURE_2D, baseTex);
        glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, gs, gs, GL_RED, GL_FLOAT, in.base);
        glBindTexture (GL_TEXTURE_2D, peakMaskTex);
        glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, gs, gs, GL_RED, GL_FLOAT, in.peakMask);
    }

    if (in.planes != nullptr)
    {
        glBindTexture (GL_TEXTURE_2D, peakAtlasTex);
        for (int pk = 0; pk < in.numPeaks; ++pk)
            if ((planeBits & (1u << pk)) != 0
                && (int) in.planes[pk].size() >= gs * gs)
                glTexSubImage2D (GL_TEXTURE_2D, 0, pk * gs, 0, gs, gs,
                                 GL_RED, GL_FLOAT, in.planes[pk].data());
    }

    glBindTexture (GL_TEXTURE_2D, minDist2Tex);
    glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, gs, gs, GL_RED, GL_FLOAT, in.minDist2);

    // --- compose pass into the height FBO ---
    glDisable (GL_BLEND);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_SCISSOR_TEST);
    glDisable (GL_STENCIL_TEST);

    glBindFramebuffer (GL_FRAMEBUFFER, heightFBO);
    glViewport (0, 0, gs, gs);

    composeProg->use();
    glActiveTexture (GL_TEXTURE0); glBindTexture (GL_TEXTURE_2D, baseTex);
    glActiveTexture (GL_TEXTURE1); glBindTexture (GL_TEXTURE_2D, peakAtlasTex);
    glActiveTexture (GL_TEXTURE2); glBindTexture (GL_TEXTURE_2D, peakMaskTex);
    glActiveTexture (GL_TEXTURE3); glBindTexture (GL_TEXTURE_2D, minDist2Tex);
    glActiveTexture (GL_TEXTURE0);

    composeProg->setUniform ("uBase",      0);
    composeProg->setUniform ("uPeakAtlas", 1);
    composeProg->setUniform ("uPeakMask",  2);
    composeProg->setUniform ("uMinDist2",  3);
    const float sigma2 = juce::jmax (1e-6f, in.sigma * in.sigma);
    composeProg->setUniform ("uDynDepth",   in.dynDepth);
    composeProg->setUniform ("uInv2Sigma2", 1.0f / (2.0f * sigma2));
    composeProg->setUniform ("uDepCut2",    9.0f * sigma2);
    glUniform1fv (glGetUniformLocation (composeProg->getProgramID(), "uAmp"),
                  in.numPeaks, in.peakAmps);

    drawQuad (*composeProg);

    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glBindTexture (GL_TEXTURE_2D, 0);
}

void SceneRenderer::renderScene (const Frame& f,
                                 juce::OpenGLFrameBuffer& sceneFBO,
                                 juce::OpenGLFrameBuffer& mask)
{
    if (sceneProg == nullptr || f.gridSize <= 1)
        return;

    if (heightTexSize != f.gridSize)
        return;   // nothing composed yet this session

    // --- scene pass: one fullscreen quad writing every pixel ---
    // JUCE's 2D renderer may have left arbitrary state behind.
    glDisable (GL_BLEND);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_SCISSOR_TEST);
    glDisable (GL_STENCIL_TEST);

    glBindFramebuffer (GL_FRAMEBUFFER, sceneFBO.getFrameBufferID());
    glViewport (0, 0, sceneFBO.getWidth(), sceneFBO.getHeight());

    sceneProg->use();

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, heightTex);
    glActiveTexture (GL_TEXTURE1);
    glBindTexture (GL_TEXTURE_2D, mask.getTextureID());
    glActiveTexture (GL_TEXTURE0);

    // Map-box transform comes from ScreenLayout via the Frame — one source of truth.
    const float scale = f.mapScale;
    const float offX  = f.mapOffX;
    const float offY  = f.mapOffY;
    const float aberPx = 1.5f * (1.0f + f.centroidNorm * 1.2f);

    sceneProg->setUniform ("uHeight",      0);
    sceneProg->setUniform ("uRiverMask",   1);
    sceneProg->setUniform ("uLogicalSize", f.logicalW, f.logicalH);
    sceneProg->setUniform ("uRenderScale", f.renderScale);
    sceneProg->setUniform ("uBoxOffset",   offX, offY);
    sceneProg->setUniform ("uBoxScale",    scale);
    sceneProg->setUniform ("uGridSize",    (float) f.gridSize);
    sceneProg->setUniform ("uHMax",        juce::jmax (1e-4f, f.heightFieldMax));
    sceneProg->setUniform ("uHue",         f.hue);
    sceneProg->setUniform ("uSaturation",  f.saturation);
    sceneProg->setUniform ("uCaLowHue",    f.caLowHue);
    sceneProg->setUniform ("uCaHighHue",   f.caHighHue);
    sceneProg->setUniform ("uBass",        f.bassEnergy);
    sceneProg->setUniform ("uEnergy",      f.overallEnergy);
    sceneProg->setUniform ("uTime",        f.animTime);
    sceneProg->setUniform ("uAberPx",      aberPx);
    sceneProg->setUniform ("uHasRiver",    f.hasRiver ? 1.0f : 0.0f);
    sceneProg->setUniform ("uMinGrad",     0.02f);

    // Label gap capsules (map-local logical px). Loop is skipped when count == 0.
    const int gapCount = juce::jlimit (0, Frame::maxGaps, f.gapCount);
    sceneProg->setUniform ("uGapCount", gapCount);
    if (gapCount > 0)
    {
        glUniform4fv (glGetUniformLocation (sceneProg->getProgramID(), "uGapSeg"),
                      gapCount, f.gapSeg.data());
        glUniform1fv (glGetUniformLocation (sceneProg->getProgramID(), "uGapHalfW"),
                      gapCount, f.gapHalfW.data());
        glUniform1fv (glGetUniformLocation (sceneProg->getProgramID(), "uGapLevel"),
                      gapCount, f.gapLevel.data());
    }

    // Hillshade light: slow azimuth drift; brighter (high-centroid) audio lowers
    // the sun for deeper relief.
    {
        const float az = f.animTime * 0.06f;              // ~1 rev / 105 s
        const float lz = 1.2f - 0.6f * f.centroidNorm;
        const float lx = std::cos (az), ly = std::sin (az);
        const float inv = 1.0f / std::sqrt (lx*lx + ly*ly + lz*lz);
        sceneProg->setUniform ("uLightDir", lx*inv, ly*inv, lz*inv);
        sceneProg->setUniform ("uShadeStrength",   f.shadeStrength);
        sceneProg->setUniform ("uSlopeK",          f.slopeK);
        sceneProg->setUniform ("uHatchStrength",   f.hatchStrength);
        sceneProg->setUniform ("uStippleStrength", f.stippleStrength);
    }

    drawQuad (*sceneProg);

    // leave GL in a tame state for whoever renders next (JUCE 2D or post FX)
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, 0);
}

//==============================================================================
void SceneRenderer::uploadRibbons (const RibbonBatch& batch)
{
    if (ribbonVBO == 0 || batch.verts.empty())
        return;

    const size_t bytes = batch.verts.size() * sizeof (RibbonVertex);
    glBindBuffer (GL_ARRAY_BUFFER, ribbonVBO);
    if (bytes > ribbonVBOCapacity)
    {
        // grow with headroom so steady-state frames just orphan + refill
        ribbonVBOCapacity = bytes + bytes / 2;
        glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) ribbonVBOCapacity, nullptr, GL_STREAM_DRAW);
    }
    else
    {
        glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) ribbonVBOCapacity, nullptr, GL_STREAM_DRAW);   // orphan
    }
    glBufferSubData (GL_ARRAY_BUFFER, 0, (GLsizeiptr) bytes, batch.verts.data());
    glBindBuffer (GL_ARRAY_BUFFER, 0);
}

void SceneRenderer::bindRibbonState (const Frame& f, juce::OpenGLFrameBuffer& target)
{
    glBindFramebuffer (GL_FRAMEBUFFER, target.getFrameBufferID());
    glViewport (0, 0, target.getWidth(), target.getHeight());

    ribbonProg->use();
    ribbonProg->setUniform ("uLogicalSize", f.logicalW, f.logicalH);

    glBindVertexArray (ribbonVAO);
    glBindBuffer (GL_ARRAY_BUFFER, ribbonVBO);
    const GLint posLoc  = glGetAttribLocation (ribbonProg->getProgramID(), "aPos");
    const GLint edgeLoc = glGetAttribLocation (ribbonProg->getProgramID(), "aEdge");
    glEnableVertexAttribArray ((GLuint) posLoc);
    glEnableVertexAttribArray ((GLuint) edgeLoc);
    glVertexAttribPointer ((GLuint) posLoc,  2, GL_FLOAT, GL_FALSE, sizeof (RibbonVertex), nullptr);
    glVertexAttribPointer ((GLuint) edgeLoc, 2, GL_FLOAT, GL_FALSE, sizeof (RibbonVertex),
                           (const void*) (2 * sizeof (float)));

    // Map-box confinement, same as the CPU path's rectangular clip. The river's
    // backbone starts off-screen above the box, so this is load-bearing.
    applyMapBoxScissor (f, target);
}

void SceneRenderer::applyMapBoxScissor (const Frame& f, juce::OpenGLFrameBuffer& target)
{
    const float boxSz = f.mapScale * (float) (f.gridSize - 1);
    const int sx = (int) std::floor (f.mapOffX * f.renderScale);
    const int sw = (int) std::ceil  (boxSz * f.renderScale);
    const int sh = (int) std::ceil  (boxSz * f.renderScale);
    const int sy = target.getHeight() - (int) std::ceil ((f.mapOffY + boxSz) * f.renderScale);
    glEnable (GL_SCISSOR_TEST);
    glScissor (sx, juce::jmax (0, sy), sw, sh);
}

void SceneRenderer::drawRibbonRange (int first, int count, juce::Colour colour)
{
    if (count < 3)
        return;
    ribbonProg->setUniform ("uColor", colour.getFloatRed(), colour.getFloatGreen(),
                            colour.getFloatBlue(), colour.getFloatAlpha());
    glDrawArrays (GL_TRIANGLE_STRIP, first, count);
}

void SceneRenderer::renderRiverMask (const Frame& f, const RibbonBatch& batch,
                                     juce::OpenGLFrameBuffer& mask)
{
    if (ribbonProg == nullptr)
        return;

    glDisable (GL_BLEND);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_SCISSOR_TEST);
    glDisable (GL_STENCIL_TEST);

    glBindFramebuffer (GL_FRAMEBUFFER, mask.getFrameBufferID());
    glViewport (0, 0, mask.getWidth(), mask.getHeight());
    glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    if (batch.fillCount >= 3)
    {
        bindRibbonState (f, mask);
        glDisable (GL_BLEND);   // solid white, hard coverage handled by the feather
        drawRibbonRange (batch.fillFirst, batch.fillCount, juce::Colours::white);
        glDisable (GL_SCISSOR_TEST);
        glBindVertexArray (0);
        glBindBuffer (GL_ARRAY_BUFFER, 0);
    }
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
}

void SceneRenderer::drawRibbons (const Frame& f, const RibbonBatch& batch,
                                 juce::OpenGLFrameBuffer& sceneFBO)
{
    if (ribbonProg == nullptr || batch.draws.empty())
        return;

    glDisable (GL_DEPTH_TEST);
    glDisable (GL_STENCIL_TEST);

    bindRibbonState (f, sceneFBO);
    glEnable (GL_BLEND);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (const auto& d : batch.draws)
        drawRibbonRange (d.first, d.count, d.colour);

    glDisable (GL_SCISSOR_TEST);
    glDisable (GL_BLEND);
    glBindVertexArray (0);
    glBindBuffer (GL_ARRAY_BUFFER, 0);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
}

//==============================================================================
void SceneRenderer::bindGlyphState (const Frame& f, juce::OpenGLFrameBuffer& target, bool clipToMap)
{
    glBindFramebuffer (GL_FRAMEBUFFER, target.getFrameBufferID());
    glViewport (0, 0, target.getWidth(), target.getHeight());

    glyphProg->use();
    glyphProg->setUniform ("uLogicalSize", f.logicalW, f.logicalH);

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, glyphAtlasTex);
    glyphProg->setUniform ("uAtlas", 0);

    glBindVertexArray (glyphVAO);
    glBindBuffer (GL_ARRAY_BUFFER, glyphVBO);
    const GLint posLoc = glGetAttribLocation (glyphProg->getProgramID(), "aPos");
    const GLint uvLoc  = glGetAttribLocation (glyphProg->getProgramID(), "aUV");
    const GLint aLoc   = glGetAttribLocation (glyphProg->getProgramID(), "aAlpha");
    glEnableVertexAttribArray ((GLuint) posLoc);
    glEnableVertexAttribArray ((GLuint) uvLoc);
    glEnableVertexAttribArray ((GLuint) aLoc);
    glVertexAttribPointer ((GLuint) posLoc, 2, GL_FLOAT, GL_FALSE, sizeof (GlyphVertex), nullptr);
    glVertexAttribPointer ((GLuint) uvLoc,  2, GL_FLOAT, GL_FALSE, sizeof (GlyphVertex),
                           (const void*) (2 * sizeof (float)));
    glVertexAttribPointer ((GLuint) aLoc,   1, GL_FLOAT, GL_FALSE, sizeof (GlyphVertex),
                           (const void*) (4 * sizeof (float)));

    // Same map-box scissor as the ribbons; screen-UI batches draw unclipped.
    if (clipToMap)
        applyMapBoxScissor (f, target);
    else
        glDisable (GL_SCISSOR_TEST);
}

void SceneRenderer::drawGlyphs (const Frame& f, const GlyphBatch& batch,
                                juce::OpenGLFrameBuffer& sceneFBO, bool clipToMap)
{
    if (glyphProg == nullptr || glyphAtlasTex == 0 || batch.draws.empty() || batch.verts.empty())
        return;

    // Upload this frame's glyph vertices (orphan + refill, like the ribbons).
    const size_t bytes = batch.verts.size() * sizeof (GlyphVertex);
    glBindBuffer (GL_ARRAY_BUFFER, glyphVBO);
    if (bytes > glyphVBOCapacity)
        glyphVBOCapacity = bytes + bytes / 2;
    glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) glyphVBOCapacity, nullptr, GL_STREAM_DRAW);   // orphan
    glBufferSubData (GL_ARRAY_BUFFER, 0, (GLsizeiptr) bytes, batch.verts.data());
    glBindBuffer (GL_ARRAY_BUFFER, 0);

    glDisable (GL_DEPTH_TEST);
    glDisable (GL_STENCIL_TEST);

    bindGlyphState (f, sceneFBO, clipToMap);
    glEnable (GL_BLEND);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (const auto& d : batch.draws)
    {
        if (d.count < 3)
            continue;
        glyphProg->setUniform ("uColor", d.colour.getFloatRed(), d.colour.getFloatGreen(),
                               d.colour.getFloatBlue(), d.colour.getFloatAlpha());
        glDrawArrays (GL_TRIANGLES, d.first, d.count);
    }

    glDisable (GL_SCISSOR_TEST);
    glDisable (GL_BLEND);
    glBindVertexArray (0);
    glBindBuffer (GL_ARRAY_BUFFER, 0);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, 0);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
}
