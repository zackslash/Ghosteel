#include "glrenderer.h"

#include <QFile>
#include <QMatrix4x4>

namespace {
constexpr int kCleanupIntervalFrames = 60;
}

void GLRenderer::Renderer::createKittyShaders()
{
    // kitty_image.glsl uses //! vertex / //! fragment markers we split on below
    QFile shaderFile(QStringLiteral(":/shaders/kitty_image.glsl"));
    if (!shaderFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open kitty_image.glsl";
        return;
    }
    QByteArray shaderSrc = shaderFile.readAll();
    shaderFile.close();

    int vertIdx = shaderSrc.indexOf("//! vertex");
    int fragIdx = shaderSrc.indexOf("//! fragment");
    if (vertIdx < 0 || fragIdx < 0) {
        qWarning() << "kitty_image.glsl missing vertex/fragment markers";
        return;
    }

    QByteArray vertSrc = shaderSrc.mid(vertIdx, fragIdx - vertIdx);
    QByteArray fragSrc = shaderSrc.mid(fragIdx);

    m_kittyProgram = new QOpenGLShaderProgram;
    m_kittyProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vertSrc);
    m_kittyProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fragSrc);
    if (!m_kittyProgram->link()) {
        qWarning() << "Kitty image shader link failed:" << m_kittyProgram->log();
        delete m_kittyProgram;
        m_kittyProgram = nullptr;
        return;
    }

    m_kittyMatrixUniform = m_kittyProgram->uniformLocation("u_matrix");
    m_kittyTexUniform = m_kittyProgram->uniformLocation("u_image");
    m_kittyPositionAttr = m_kittyProgram->attributeLocation("position");
    m_kittyTexcoordAttr = m_kittyProgram->attributeLocation("texcoord");
}

void GLRenderer::Renderer::snapshotKittyGraphics(GhosttyTerminal terminal, GhosttyVt *vt)
{
    m_kittyPlacements.clear();

    if (!terminal || !vt)
        return;

    if (!m_kittyGraphicsEnabled) {
        // Feature disabled — queue all textures for deferred deletion.
        // GL calls must happen on the render thread, not the GUI thread.
        for (auto it = m_kittyTextures.constBegin(); it != m_kittyTextures.constEnd(); ++it)
            m_kittyTexturesToDelete.append(it.value().texture);
        m_kittyTextures.clear();
        return;
    }

    m_kittyFrameCounter++;

    if (m_kittyFrameCounter % kCleanupIntervalFrames == 0)
        cleanupKittyCache();

    // Snapshot placement data from the terminal (GUI thread — safe).
    // The render thread will draw from this snapshot without touching
    // ghostty_terminal_get, avoiding a data race with vtWrite.
    GhosttyKittyGraphics graphics = nullptr;
    ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &graphics);
    if (!graphics)
        return;

    for (GhosttyKittyPlacementLayer layer : {GHOSTTY_KITTY_PLACEMENT_LAYER_BELOW_TEXT,
                                              GHOSTTY_KITTY_PLACEMENT_LAYER_ABOVE_TEXT}) {
        GhosttyKittyGraphicsPlacementIterator iter = nullptr;
        if (ghostty_kitty_graphics_placement_iterator_new(nullptr, &iter) != GHOSTTY_SUCCESS)
            continue;
        if (ghostty_kitty_graphics_get(graphics,
                GHOSTTY_KITTY_GRAPHICS_DATA_PLACEMENT_ITERATOR, &iter) != GHOSTTY_SUCCESS) {
            ghostty_kitty_graphics_placement_iterator_free(iter);
            continue;
        }
        ghostty_kitty_graphics_placement_iterator_set(iter,
            GHOSTTY_KITTY_GRAPHICS_PLACEMENT_ITERATOR_OPTION_LAYER, &layer);

        while (ghostty_kitty_graphics_placement_next(iter)) {
            bool isVirtual = false;
            ghostty_kitty_graphics_placement_get(iter,
                GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_IS_VIRTUAL, &isVirtual);
            if (isVirtual)
                continue;

            KittyPlacementSnapshot snap;
            snap.layer = layer;

            ghostty_kitty_graphics_placement_get(iter,
                GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_IMAGE_ID, &snap.imageId);
            if (snap.imageId == 0)
                continue;

            GhosttyKittyGraphicsImage image = ghostty_kitty_graphics_image(graphics, snap.imageId);
            if (!image) {
                // Image deleted from storage — snapshot will trigger cache eviction
                snap.imageExists = false;
                m_kittyPlacements.append(snap);
                continue;
            }
            snap.imageExists = true;

            ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_WIDTH, &snap.imgW);
            ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_HEIGHT, &snap.imgH);
            if (snap.imgW == 0 || snap.imgH == 0)
                continue;

            GhosttyKittyGraphicsPlacementRenderInfo info = GHOSTTY_INIT_SIZED(GhosttyKittyGraphicsPlacementRenderInfo);
            if (ghostty_kitty_graphics_placement_render_info(iter, image, terminal, &info) != GHOSTTY_SUCCESS)
                continue;
            if (!info.viewport_visible)
                continue;
            snap.renderInfo = info;

            ghostty_kitty_graphics_placement_get(iter,
                GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_X_OFFSET, &snap.xOffset);
            ghostty_kitty_graphics_placement_get(iter,
                GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_Y_OFFSET, &snap.yOffset);

            // Ghostty exposes a per-image generation stamp (GHOSTTY_KITTY_IMAGE_DATA_GENERATION)
            // that changes whenever pixel contents change — even when dimensions, format,
            // and data length are identical (e.g. an app re-uploading under the same image
            // ID). Keying staleness on dataLen misses those updates and renders stale pixels.
            uint64_t gen = 0;
            ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_GENERATION, &gen);
            snap.generation = gen;

            if (m_kittyTextures.contains(snap.imageId)) {
                if (gen != m_kittyTextures[snap.imageId].generation) {
                    // Image ID reused, or contents changed — queue old texture for deletion
                    m_kittyTexturesToDelete.append(m_kittyTextures[snap.imageId].texture);
                    m_kittyTextures.remove(snap.imageId);
                    snap.needsUpload = true;
                    // A new generation may upload fine — drop any stale failure entry.
                    forgetKittyFailedUpload(snap.imageId);
                } else {
                    // The draw path normally refreshes lastSeenFrame, but
                    // canSkipPipeline skips drawKittyImageLayer on idle frames
                    // while a custom shader keeps the anim timer alive. Refresh
                    // here so an on-screen texture is not evicted and then
                    // re-deep-copied every frame.
                    m_kittyTextures[snap.imageId].lastSeenFrame = m_kittyFrameCounter;
                }
            } else {
                snap.needsUpload = true;
            }

            // Negative cache: if this (image, generation) already failed to
            // upload, don't deep-copy the pixels or re-attempt glTexImage2D
            // every frame. The entry is dropped when the generation changes
            // (handled above in the generation-mismatch branch), when the image
            // is evicted/replaced, or after KITTY_FAILED_RETRY_FRAMES so a
            // transient GL_OUT_OF_MEMORY is retried instead of hiding the image.
            if (snap.needsUpload && kittyUploadFailed(snap.imageId, gen)) {
                snap.needsUpload = false;
            }

            if (snap.needsUpload) {
                const uint8_t *pixels = nullptr;
                size_t pixelsLen = 0;
                ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR, &pixels);
                ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN, &pixelsLen);
                ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_FORMAT, &snap.format);
                if (pixels && pixelsLen > 0) {
                    snap.pixelData = QByteArray(reinterpret_cast<const char*>(pixels),
                                                static_cast<int>(pixelsLen));
                    snap.dataLen = pixelsLen;
                } else {
                    continue;
                }
            }

            m_kittyPlacements.append(snap);
        }
        ghostty_kitty_graphics_placement_iterator_free(iter);
    }
}

void GLRenderer::Renderer::drainPendingKittyDeletions()
{
    if (m_kittyTexturesToDelete.isEmpty())
        return;
    for (GLuint tex : m_kittyTexturesToDelete)
        glDeleteTextures(1, &tex);
    m_kittyTexturesToDelete.clear();
}

bool GLRenderer::Renderer::kittyUploadFailed(uint32_t imageId, uint64_t generation)
{
    for (int i = m_kittyFailedUploads.size() - 1; i >= 0; --i) {
        const auto &f = m_kittyFailedUploads[i];
        if (f.imageId == imageId && f.generation == generation) {
            if (m_kittyFrameCounter - f.lastAttemptFrame > KITTY_FAILED_RETRY_FRAMES) {
                m_kittyFailedUploads.removeAt(i);
                return false;
            }
            return true;
        }
    }
    return false;
}

void GLRenderer::Renderer::forgetKittyFailedUpload(uint32_t imageId)
{
    for (int i = m_kittyFailedUploads.size() - 1; i >= 0; --i) {
        if (m_kittyFailedUploads[i].imageId == imageId)
            m_kittyFailedUploads.removeAt(i);
    }
}

void GLRenderer::Renderer::drawKittyImageLayer(GhosttyKittyPlacementLayer layer,
                                                const QMatrix4x4 &proj, int /* fboW */, int /* fboH */)
{
    if (!m_kittyProgram || !m_kittyProgram->isLinked())
        return;

    if (!m_kittyGraphicsEnabled)
        return;

    // This layer may be drawn between the two cell passes, i.e. while the cell
    // VBO is bound to GL_ARRAY_BUFFER. glVertexAttribPointer below feeds a
    // client-side stack array, which is only valid when no VBO is bound —
    // otherwise the pointer is read as a byte offset into the bound VBO. Save
    // the current binding, unbind, and restore it on exit.
    GLint prevArrayBuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    bool hasAnyPlacement = false;

    // Negative-cache bookkeeping: an image with multiple placements would
    // otherwise append one entry per placement on its first failing frame,
    // crowding out other images' FIFO slots. Linear scan over <=64 entries,
    // trivially cheap.
    auto appendKittyFailedUpload = [this](uint32_t imageId, uint64_t generation) {
        for (auto &f : m_kittyFailedUploads) {
            if (f.imageId == imageId && f.generation == generation) {
                f.lastAttemptFrame = m_kittyFrameCounter;
                return;
            }
        }
        m_kittyFailedUploads.append({imageId, generation, m_kittyFrameCounter});
    };

    for (const auto &snap : m_kittyPlacements) {
        if (snap.layer != layer)
            continue;

        // Image deleted from storage — evict from cache
        if (!snap.imageExists) {
            auto it = m_kittyTextures.find(snap.imageId);
            if (it != m_kittyTextures.end()) {
                m_kittyTexturesToDelete.append(it.value().texture);
                m_kittyTextures.erase(it);
            }
            forgetKittyFailedUpload(snap.imageId);
            continue;
        }

        uint32_t imgW = snap.imgW, imgH = snap.imgH;
        if (imgW == 0 || imgH == 0)
            continue;

        // Negative-cache hit: needsUpload=false combined with an absent texture
        // cache entry uniquely identifies a previously-failed upload — snapshot
        // only clears needsUpload when (imageId, generation) is in
        // m_kittyFailedUploads. Skip silently; the failing path already printed
        // its qWarning once.
        if (!snap.needsUpload && !m_kittyTextures.contains(snap.imageId))
            continue;

        // Clamp against GL_MAX_TEXTURE_SIZE (queried lazily on the render thread).
        // Without this, a malicious/buggy VT can request enormous dimensions and
        // overflow size_t (4*W*H wraps on 32-bit ARM) in the gray->RGBA conversion
        // below, causing a heap over-read before glTexImage2D ever rejects it.
        if (m_maxTextureSize == 0) {
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTextureSize);
            if (m_maxTextureSize <= 0)
                m_maxTextureSize = 2048; // sane fallback if the query fails
        }
        if (imgW > static_cast<uint32_t>(m_maxTextureSize)
            || imgH > static_cast<uint32_t>(m_maxTextureSize)) {
            qWarning() << "Kitty image" << snap.imageId << "dimensions" << imgW << "x" << imgH
                       << "exceed GL_MAX_TEXTURE_SIZE" << m_maxTextureSize << "; skipping upload";
            // Negative-cache: this image can never upload on this GPU — stop
            // deep-copying pixels and re-warning every frame.
            appendKittyFailedUpload(snap.imageId, snap.generation);
            continue;
        }

        const auto &info = snap.renderInfo;
        uint32_t xOffset = snap.xOffset, yOffset = snap.yOffset;

        if (snap.needsUpload && !m_kittyTextures.contains(snap.imageId)) {
            const uint8_t *pixels = reinterpret_cast<const uint8_t*>(snap.pixelData.constData());
            size_t pixelsLen = snap.dataLen;

            if (!pixels || pixelsLen == 0)
                continue;

            GhosttyKittyImageFormat fmt = snap.format;

            // Validate the source buffer has enough bytes for the declared
            // format/dimensions before any read (gray->RGBA loop or glTexImage2D).
            // Guards against truncated payloads and, with the clamp above, the
            // size_t overflow case on 32-bit ARM.
            size_t srcBpp = 4;
            if (fmt == GHOSTTY_KITTY_IMAGE_FORMAT_RGB) srcBpp = 3;
            else if (fmt == GHOSTTY_KITTY_IMAGE_FORMAT_GRAY) srcBpp = 1;
            else if (fmt == GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA) srcBpp = 2;
            size_t requiredSrcBytes = static_cast<size_t>(imgW) * static_cast<size_t>(imgH) * srcBpp;
            if (pixelsLen < requiredSrcBytes) {
                qWarning() << "Kitty image" << snap.imageId << "buffer truncated:" << pixelsLen
                           << "bytes, need" << requiredSrcBytes << "for" << imgW << "x" << imgH
                           << "; skipping upload";
                // Negative-cache: the payload is corrupt for this generation —
                // don't re-copy and re-attempt it every frame.
                appendKittyFailedUpload(snap.imageId, snap.generation);
                continue;
            }

            GLenum glFmt = GL_RGBA;
            bool convertedPixels = false;
            if (fmt == GHOSTTY_KITTY_IMAGE_FORMAT_RGB)
                glFmt = GL_RGB;
            else if (fmt == GHOSTTY_KITTY_IMAGE_FORMAT_GRAY
                     || fmt == GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA) {
                // Convert gray/gray-alpha to RGBA for GL upload
                bool hasAlpha = (fmt == GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA);
                size_t convSrcBpp = hasAlpha ? 2 : 1;
                size_t convertedLen = static_cast<size_t>(imgW) * static_cast<size_t>(imgH) * 4;
                uint8_t* rgba = static_cast<uint8_t*>(malloc(convertedLen));
                if (rgba) {
                    for (size_t i = 0; i < static_cast<size_t>(imgW) * static_cast<size_t>(imgH); ++i) {
                    uint8_t gray = pixels[i * convSrcBpp];
                    uint8_t alpha = hasAlpha ? pixels[i * convSrcBpp + 1] : 255;
                        rgba[i * 4 + 0] = gray;
                        rgba[i * 4 + 1] = gray;
                        rgba[i * 4 + 2] = gray;
                        rgba[i * 4 + 3] = alpha;
                    }
                    pixels = rgba;
                    glFmt = GL_RGBA;
                    convertedPixels = true;
                } else {
                    // Falling through would pass the undersized gray buffer to
                    // glTexImage2D (expects RGBA), causing a heap over-read.
                    qWarning() << "Kitty image: failed to allocate" << convertedLen
                               << "bytes for gray->RGBA conversion (image"
                               << snap.imageId << imgW << "x" << imgH << ")";
                    // Negative-cache: allocation is failing persistently — stop
                    // re-copying and re-attempting every frame.
                    appendKittyFailedUpload(snap.imageId, snap.generation);
                    continue;
                }
            }

            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            // GL_UNPACK_ALIGNMENT defaults to 4, but the GL_RGB path uploads
            // tightly-packed rows (width*3 bytes). For any width not divisible by
            // 4, GL would read past each row's end — over-reading the heap buffer.
            // Alignment=1 is correct for all our formats here (RGBA is 4-byte
            // aligned regardless).
            GLint prevUnpackAlignment = 4;
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevUnpackAlignment);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glGetError(); // clear stale error flags before the allocation
            glTexImage2D(GL_TEXTURE_2D, 0, glFmt, imgW, imgH, 0,
                         glFmt, GL_UNSIGNED_BYTE, pixels);
            const GLenum uploadErr = glGetError();
            glPixelStorei(GL_UNPACK_ALIGNMENT, prevUnpackAlignment);
            glBindTexture(GL_TEXTURE_2D, 0);
            if (convertedPixels)
                free(const_cast<uint8_t*>(pixels));

            if (uploadErr != GL_NO_ERROR) {
                // Driver rejected the allocation (commonly GL_OUT_OF_MEMORY on
                // large uploads) — the texture name is unusable. Drop it and
                // skip the cache entry so the placement is skipped this frame.
                // Negative-cache the (image, generation) so snapshotKittyGraphics
                // stops re-copying and re-attempting until the generation changes.
                qWarning() << "Kitty image" << snap.imageId << "upload failed ("
                           << uploadErr << "), skipping";
                glDeleteTextures(1, &tex);
                appendKittyFailedUpload(snap.imageId, snap.generation);
                continue;
            }

            KittyCachedTexture cached;
            cached.texture = tex;
            cached.lastSeenFrame = m_kittyFrameCounter;
            cached.generation = snap.generation;
            m_kittyTextures.insert(snap.imageId, cached);
        } else if (m_kittyTextures.contains(snap.imageId)) {
            m_kittyTextures[snap.imageId].lastSeenFrame = m_kittyFrameCounter;
        }

        if (!m_kittyTextures.contains(snap.imageId))
            continue;

        float destX = static_cast<float>(info.viewport_col * m_cellWidth) + static_cast<float>(xOffset);
        float destY = m_topPadding + static_cast<float>(info.viewport_row * m_cellHeight) + static_cast<float>(yOffset);
        float destW = static_cast<float>(info.pixel_width);
        float destH = static_cast<float>(info.pixel_height);

        float uvX0 = static_cast<float>(info.source_x) / static_cast<float>(imgW);
        float uvY0 = static_cast<float>(info.source_y) / static_cast<float>(imgH);
        float uvX1 = static_cast<float>(info.source_x + info.source_width) / static_cast<float>(imgW);
        float uvY1 = static_cast<float>(info.source_y + info.source_height) / static_cast<float>(imgH);

        // Clip partial visibility (negative viewport_row). The placement
        // is off-screen by (-viewport_row * cellHeight) - yOffset pixels:
        // yOffset offsets the image downward within its first cell, so a
        // positive yOffset means LESS of the image is clipped than the
        // row count alone suggests, and the visible remainder starts
        // topPadding + max(0, yOffset - (-viewport_row * cellHeight)).
        if (info.viewport_row < 0) {
            int rowClipPx = (-info.viewport_row) * m_cellHeight;
            int clippedPx = qMax(0, rowClipPx - static_cast<int>(yOffset));
            destY = m_topPadding + qMax(0, static_cast<int>(yOffset) - rowClipPx);
            destH -= clippedPx;
            if (destH <= 0)
                continue;
            float uvScale = (uvY1 - uvY0) / static_cast<float>(info.pixel_height);
            uvY0 += uvScale * clippedPx;
        }

        float x0 = destX, y0 = destY;
        float x1 = destX + destW, y1 = destY + destH;

        float verts[24] = {
            x0, y0, uvX0, uvY0,
            x1, y0, uvX1, uvY0,
            x1, y1, uvX1, uvY1,
            x0, y0, uvX0, uvY0,
            x1, y1, uvX1, uvY1,
            x0, y1, uvX0, uvY1,
        };

        if (!hasAnyPlacement) {
            m_kittyProgram->bind();
            m_kittyProgram->setUniformValue(m_kittyMatrixUniform, proj);
            m_kittyProgram->setUniformValue(m_kittyTexUniform, 0);
            glActiveTexture(GL_TEXTURE0);
            glEnableVertexAttribArray(m_kittyPositionAttr);
            glEnableVertexAttribArray(m_kittyTexcoordAttr);
            hasAnyPlacement = true;
        }

        glBindTexture(GL_TEXTURE_2D, m_kittyTextures[snap.imageId].texture);

        const int stride = 4 * sizeof(float);
        glVertexAttribPointer(m_kittyPositionAttr, 2, GL_FLOAT, GL_FALSE,
                              stride, verts);
        glVertexAttribPointer(m_kittyTexcoordAttr, 2, GL_FLOAT, GL_FALSE,
                              stride, verts + 2);

        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    if (hasAnyPlacement) {
        glDisableVertexAttribArray(m_kittyPositionAttr);
        glDisableVertexAttribArray(m_kittyTexcoordAttr);
        glBindTexture(GL_TEXTURE_2D, 0);
        m_kittyProgram->release();
    }

    // Restore the caller's GL_ARRAY_BUFFER binding (the cell VBO when this runs
    // between the two cell passes).
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevArrayBuffer));
}

void GLRenderer::Renderer::cleanupKittyCache()
{
    // Cap the negative-cache vector: failed uploads never enter m_kittyTextures,
    // so the eviction-based forget path cannot reclaim them. FIFO-drop the
    // oldest entries; a dropped entry costs one retry-and-re-fail cycle,
    // which re-adds it.
    const int kMaxKittyFailedUploads = 64;
    while (m_kittyFailedUploads.size() > kMaxKittyFailedUploads)
        m_kittyFailedUploads.removeFirst();

    if (m_kittyTextures.isEmpty())
        return;

    QList<uint32_t> toRemove;
    for (auto it = m_kittyTextures.constBegin(); it != m_kittyTextures.constEnd(); ++it) {
        if (m_kittyFrameCounter - it.value().lastSeenFrame > KITTY_EVICTION_FRAMES)
            toRemove.append(it.key());
    }

    // Also evict oldest if over hard cap
    int excess = m_kittyTextures.size() - MAX_KITTY_TEXTURES;
    for (int i = 0; i < excess && !m_kittyTextures.isEmpty(); ++i) {
        uint32_t oldestFrame = UINT32_MAX;
        uint32_t oldestId = 0;
        for (auto it = m_kittyTextures.constBegin(); it != m_kittyTextures.constEnd(); ++it) {
            if (toRemove.contains(it.key()))
                continue;
            if (it.value().lastSeenFrame < oldestFrame) {
                oldestFrame = it.value().lastSeenFrame;
                oldestId = it.key();
            }
        }
        if (oldestId)
            toRemove.append(oldestId);
    }

    for (uint32_t id : toRemove) {
        auto it = m_kittyTextures.find(id);
        if (it != m_kittyTextures.end()) {
            m_kittyTexturesToDelete.append(it.value().texture);
            m_kittyTextures.erase(it);
        }
        // Evicted — allow a future upload to retry.
        forgetKittyFailedUpload(id);
    }
}
