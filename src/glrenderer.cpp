#include "glrenderer.h"
#include "terminalview.h"
#include "ghosttyvt.h"
#include "settings.h"
#include "ptymanager.h"
#include "textutil.h"

#include <cmath>
#include <cstring>
#include <QDebug>
#include <QMatrix4x4>
#include <QFontMetrics>
#include <QOpenGLContext>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QFont>
#include <QMetaObject>

// --- GLRenderer implementation ---

GLRenderer::GLRenderer(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
    // Connect settings signals once — Settings is a singleton that never changes.
    // Font family/opacity changes invalidate cached metrics via atomic generation counter.
    // Font size is per-session and is wired from the TerminalView source in setSource().
    Settings *s = Settings::instance();
    connect(s, &Settings::fontFamilyChanged, this, &GLRenderer::invalidateMetrics);
    connect(s, &Settings::backgroundOpacityChanged, this, &GLRenderer::invalidateMetrics);

    // Populate initial cached values so the first render uses actual settings.
    // fontSize is updated from the TerminalView source in setSource().
    m_cachedMetrics.fontFamily = s->fontFamily();
    m_cachedMetrics.fontSize = s->fontSize();
    m_cachedMetrics.backgroundOpacity = s->backgroundOpacity();

    m_wantsCursorTrails = s->cursorTrails();
    m_customShaderPath = s->customShaderPath();
    // Start the anim timer whenever trails are wanted OR a custom shader is set —
    // both consume iTime and need animation from startup; otherwise iTime stays
    // frozen until the first settings toggle.
    if (m_wantsCursorTrails || !m_customShaderPath.isEmpty())
        m_shaderAnimTimer.start(33, this); // ~30fps
    connect(s, &Settings::cursorTrailsChanged, this, [this]() {
        m_wantsCursorTrails = Settings::instance()->cursorTrails();
        if (!m_customShaderPath.isEmpty() || m_wantsCursorTrails)
            m_shaderAnimTimer.start(33, this);
        else
            m_shaderAnimTimer.stop();
        m_animCfgGeneration.fetchAndAddOrdered(1);
        update();
    });

    connect(s, &Settings::customShaderPathChanged, this, [this]() {
        m_customShaderPath = Settings::instance()->customShaderPath();
        m_customShaderDirty = true;
        if (!m_customShaderPath.isEmpty() || m_wantsCursorTrails)
            m_shaderAnimTimer.start(33, this);
        m_animCfgGeneration.fetchAndAddOrdered(1);
        update();
    });

    // Seed the generation so the first synchronize() sees the initial state.
    m_animCfgGeneration.fetchAndAddOrdered(1);
}

void GLRenderer::setSource(QObject *source)
{
    if (m_source == source)
        return;

    if (m_source) {
        disconnect(m_source, nullptr, this, nullptr);
    }

    m_source = source;

    TerminalView *tv = qobject_cast<TerminalView *>(m_source);
    if (tv) {
        connect(tv, &TerminalView::repaintRequested, this, &QQuickItem::update);
        connect(tv, &TerminalView::fontSizeChanged, this, &GLRenderer::invalidateMetrics);
        invalidateMetrics();
    }

    Q_EMIT sourceChanged();
}

void GLRenderer::invalidateMetrics()
{
    // Snapshot settings for Renderer (consumed on render thread via generation counter)
    Settings *s = Settings::instance();
    m_cachedMetrics.fontFamily = s->fontFamily();
    m_cachedMetrics.backgroundOpacity = s->backgroundOpacity();

    // Font size is per-session — read from the TerminalView source, not Settings
    TerminalView *tv = qobject_cast<TerminalView *>(m_source);
    m_cachedMetrics.fontSize = tv ? tv->fontSize() : s->fontSize();

    // Increment generation counter — synchronize() will detect the mismatch
    // and re-initialize font metrics, atlas, and opacity on the render thread.
    m_metricsGeneration.fetchAndAddOrdered(1);
    update();
}

void GLRenderer::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_shaderAnimTimer.timerId()) {
        update();
    } else {
        QQuickItem::timerEvent(event);
    }
}

void GLRenderer::startShaderAnimTimer()
{
    // QBasicTimer is thread-affine; control is marshalled here (see m_shaderAnimTimer doc).
    if (!m_shaderAnimTimer.isActive())
        m_shaderAnimTimer.start(33, this); // ~30fps
}

void GLRenderer::stopShaderAnimTimer()
{
    // See startShaderAnimTimer() — must run on the GUI thread.
    if (m_shaderAnimTimer.isActive())
        m_shaderAnimTimer.stop();
}

QQuickFramebufferObject::Renderer *GLRenderer::createRenderer() const
{
    return new Renderer;
}

// --- Renderer implementation ---

GLRenderer::Renderer::Renderer()
{
    memset(m_postPaletteData, 0, sizeof(m_postPaletteData));
}

GLRenderer::Renderer::~Renderer()
{
    // QOpenGLShaderProgram handles context absence gracefully in its destructor
    // (skips GL cleanup, frees CPU memory). Always safe to delete.
    delete m_program;
    delete m_flatProgram;
    delete m_magProgram;
    delete m_blitProgram;
    delete m_kittyProgram;
    m_kittyProgram = nullptr;
    delete m_postShader.program;
    m_postShader.program = nullptr;

    if (QOpenGLContext::currentContext()) {
        // Delete all kitty textures directly. Eviction logic (cleanupKittyCache)
        // is irrelevant at teardown — we want to free everything, including
        // textures deferred to m_kittyTexturesToDelete by the last frame's
        // snapshotKittyGraphics call (which render() never got to drain).
        for (auto it = m_kittyTextures.constBegin(); it != m_kittyTextures.constEnd(); ++it)
            glDeleteTextures(1, &it.value().texture);
        m_kittyTextures.clear();
        for (GLuint tex : m_kittyTexturesToDelete)
            glDeleteTextures(1, &tex);
        m_kittyTexturesToDelete.clear();

        if (m_vbo.isCreated())
            m_vbo.destroy();
        if (m_flatVbo.isCreated())
            m_flatVbo.destroy();
        if (m_bandVbo.isCreated())
            m_bandVbo.destroy();
        destroyPipelineFbo();
    }
}

void GLRenderer::Renderer::initialize()
{
    if (m_initialized)
        return;

    initializeOpenGLFunctions();
    createShaders();
    createVBO();
    createFlatShaders();
    createFlatVBO();
    createBandVBO();
    createMagShaders();
    createBlitShader();
    createKittyShaders();

    detectES300();
    if (m_es300)
        createPostShaders();

    m_initialized = true;

    qDebug() << "GLRenderer: initialized, program=" << m_program
             << "posAttr=" << m_positionAttr
             << "texAttr=" << m_texcoordAttr
             << "fgAttr=" << m_fgColorAttr
             << "bgAttr=" << m_bgColorAttr
             << "decoAttr=" << m_decoAttr
             << "GL_VERSION:" << (const char*)glGetString(GL_VERSION)
             << "GL_RENDERER:" << (const char*)glGetString(GL_RENDERER)
             << "ES300:" << m_es300
              << "postShaderActive:" << m_postShaderActive;
}

void GLRenderer::Renderer::rebuildVBO()
{
    m_vertexCount = m_cellVertices.size();
    if (m_vertexCount == 0)
        return;

    m_vbo.bind();
    m_vbo.allocate(m_cellVertices.constData(),
                   m_vertexCount * sizeof(CellVertex));
    m_vbo.release();
}

QOpenGLFramebufferObject *GLRenderer::Renderer::createFramebufferObject(const QSize &size)
{
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    format.setInternalTextureFormat(GL_RGBA);
    return new QOpenGLFramebufferObject(size, format);
}

void GLRenderer::Renderer::synchronize(QQuickFramebufferObject *item)
{
    GLRenderer *q = static_cast<GLRenderer *>(item);

    if (!q->isVisible())
        return;

    // m_source is a QPointer — it auto-nulls if the item was destroyed, so this
    // cast (and the null-check below) is safe without a destroyed-hook.
    m_terminalView = qobject_cast<TerminalView *>(q->m_source);
    if (!m_terminalView || !m_terminalView->vt())
        return;

    GhosttyVt *vt = m_terminalView->vt();
    vt->updateRenderState();

    GhosttyRenderState state = vt->renderState();
    if (!state)
        return;

    // Re-initialize font metrics, atlas, and opacity when settings changed
    int gen = q->m_metricsGeneration.loadAcquire();
    if (gen != m_lastMetricsGeneration) {
        m_lastMetricsGeneration = gen;
        m_cellWidth = 0; // force re-init below
        // A font family/size change wipes the atlas (setFont below), so the UVs
        // baked in m_cellVertices would sample an empty atlas. Ghostty never
        // marks the grid dirty for a settings change, so force a vertex rebuild
        // this frame regardless of its dirty flag.
        m_forceVertexRebuild = true;
    }

    uint16_t cols = 0, rows = 0;
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_COLS, &cols);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROWS, &rows);

    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors);

    float bgR = colors.background.r / 255.0f;
    float bgG = colors.background.g / 255.0f;
    float bgB = colors.background.b / 255.0f;
    float fgR = colors.foreground.r / 255.0f;
    float fgG = colors.foreground.g / 255.0f;
    float fgB = colors.foreground.b / 255.0f;

    // Cache colors for post shader uniforms
    m_postBgR = bgR; m_postBgG = bgG; m_postBgB = bgB;
    m_postFgR = fgR; m_postFgG = fgG; m_postFgB = fgB;
    m_postCursorColorHasValue = colors.cursor_has_value;
    if (colors.cursor_has_value) {
        m_postCursorR = colors.cursor.r / 255.0f;
        m_postCursorG = colors.cursor.g / 255.0f;
        m_postCursorB = colors.cursor.b / 255.0f;
    }
    // Pack 256-color palette into flat float array (vec3 per entry = 768 floats)
    for (int i = 0; i < 256; ++i) {
        m_postPaletteData[i * 3 + 0] = colors.palette[i].r / 255.0f;
        m_postPaletteData[i * 3 + 1] = colors.palette[i].g / 255.0f;
        m_postPaletteData[i * 3 + 2] = colors.palette[i].b / 255.0f;
    }
    // Raw palette for the notch band's grid_ref style resolution (grid_ref
    // returns palette indexes, not pre-resolved RGB like the render state).
    memcpy(m_bandPalette, colors.palette, sizeof(m_bandPalette));

    GhosttyRenderStateCursor cursor = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cursor);

    bool cursorVisible = cursor.visible;
    bool cursorInViewport = cursor.viewport_has_value;
    bool cursorWideTail = false;
    uint16_t cursorX = 0, cursorY = 0;
    GhosttyRenderStateCursorVisualStyle cursorStyle = GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK;
    if (cursorVisible && cursorInViewport) {
        // Cursor X sits on the spacer-tail of a wide char (head at X-1).
        // See ghostty src/renderer/generic.zig:2521-2534.
        cursorX = cursor.viewport_x;
        cursorY = cursor.viewport_y;
        cursorStyle = cursor.visual_style;
        cursorWideTail = cursor.wide_tail;
    }

    // Read blink state from TerminalView (single source of truth)
    bool cursorBlinkVisible = m_terminalView->cursorBlinkVisible();

    // Initialize or re-initialize font metrics when settings changed
    if (m_cellWidth == 0) {
        // Read cell dimensions from TerminalView (single source of truth)
        m_cellWidth = m_terminalView->cellWidth();
        m_cellHeight = m_terminalView->cellHeight();
        m_bgOpacity = q->m_cachedMetrics.backgroundOpacity;
        m_cachedFontSize = q->m_cachedMetrics.fontSize;

        QFont font = TextUtil::makeTerminalFont(q->m_cachedMetrics.fontFamily,
                                                q->m_cachedMetrics.fontSize);

        if (!m_atlasInitialized) {
            m_atlas.initialize();
            m_atlasInitialized = true;
        }
        // setFont() clears both glyph caches and re-uploads the whole atlas
        // texture, so only call it when the family/size actually changed.
        // Opacity-only changes (backgroundOpacity slider) must not rebuild it.
        // makeTerminalFont resolves an empty family to "monospace", so compare
        // the resolved family to keep the skip decision consistent with the
        // QFont the atlas is actually built from.
        const QString family = q->m_cachedMetrics.fontFamily.isEmpty()
            ? QStringLiteral("monospace") : q->m_cachedMetrics.fontFamily;
        if (m_atlasFamily != family || m_atlasFontSize != q->m_cachedMetrics.fontSize) {
            m_atlas.setFont(font, m_cellWidth, m_cellHeight);
            m_atlasFamily = family;
            m_atlasFontSize = q->m_cachedMetrics.fontSize;
        }
    }

    // Read topPadding every frame (can change without font change)
    m_topPadding = m_terminalView->topPadding();

    bool gridSizeChanged = (cols != m_cols || rows != m_rows);
    m_cols = cols;
    m_rows = rows;
    // Terminal item size — cols/rows were derived from width()/height() in
    // recalculateDimensions; needed by buildCellVertices for the edge-strip fill.
    m_viewportWidth = static_cast<int>(m_terminalView->width());
    m_viewportHeight = static_cast<int>(m_terminalView->height());

    // Update cursor state every frame (blink changes don't set dirty flag).
    // This is cheap — no vertex rebuild, just updating uniform values.
    //
    // Only update m_cursorX/m_cursorY when ghostty is actually tracking the
    // cursor (visible + in viewport). When hidden, ghostty leaves cursorX/
    // cursorY at default (0,0); writing that would flag a spurious move from
    // the real position to (0,0) and fire a trail leg to the top-left.
    bool cursorTracked = cursorVisible && cursorInViewport;
    if (cursorTracked) {
        // Shift X to the wide-char head when on a spacer-tail so the shader
        // cursor test ([cursorPos.x, cursorPos.x + cursorWidth)) covers both cells.
        int effectiveX = static_cast<int>(cursorX);
        int newWidth = 1;
        if (cursorWideTail && effectiveX > 0) {
            effectiveX -= 1;
            newWidth = 2;
        }
        int newCursorX = effectiveX;
        int newCursorY = static_cast<int>(cursorY);
        // Guard: m_cursorX starts at kCursorUnset, skip first-frame spurious move
        if (m_cursorX != kCursorUnset && (newCursorX != static_cast<int>(m_cursorX) || newCursorY != static_cast<int>(m_cursorY))) {
            m_prevCursorX = static_cast<int>(m_cursorX);
            m_prevCursorY = static_cast<int>(m_cursorY);
            m_cursorMoved = true;
        }
        m_cursorX = static_cast<float>(newCursorX);
        m_cursorY = static_cast<float>(newCursorY);
        m_cursorWidth = newWidth;
    }
    bool cursorEffective = cursorVisible && cursorInViewport && cursorBlinkVisible;
    if (cursorEffective != m_cursorVisible)
        m_gridDirty = true; // blink toggle needs redraw
    m_cursorVisible = cursorEffective;
    // Map cursor style to shader uniform: 0=none, 1=block, 2=bar, 3=underline, 4=hollow
    if (!cursorEffective) {
        m_cursorStyle = 0;
    } else switch (cursorStyle) {
        case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK:        m_cursorStyle = 1; break;
        case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR:          m_cursorStyle = 2; break;
        case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE:    m_cursorStyle = 3; break;
        case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW: m_cursorStyle = 4; break;
        default: m_cursorStyle = 1; break;
    }
    // A style flip that changes no cells (block<->hollow, vi modes) still needs a scene redraw.
    if (m_cursorStyle != m_prevCursorStyleForSkip)
        m_sceneExternalChanged = true;
    m_prevCursorStyleForSkip = m_cursorStyle;

    // --- Overlay state snapshot (always, even when grid is clean) ---
    // This is cheap (just copying values) and must run every frame so that
    // overlay-only changes (selection, search, magnifier) are picked up
    // even when the terminal grid itself is not dirty.
    if (m_terminalView) {
        // Selection
        m_selecting = m_terminalView->isSelecting();
        m_selStart = m_terminalView->selectionStart();
        m_selEnd = m_terminalView->selectionEnd();
        m_handlesVisible = m_terminalView->handlesVisible();
        m_selectionHighlightColor = m_terminalView->selectionHighlightColor();
        m_selectionHandleColor = m_terminalView->selectionHandleColor();
        m_selectionHandleBorderColor = m_terminalView->selectionHandleBorderColor();

        // Search
        m_searchActive = m_terminalView->searchActive();
        if (m_searchActive) {
            m_searchMatches = m_terminalView->searchMatches();
            m_currentMatchIndex = m_terminalView->currentMatchIndex();
        } else {
            m_searchMatches.clear();
        }
        m_searchHighlightColor = m_terminalView->searchHighlightColor();
        m_searchCurrentColor = m_terminalView->searchCurrentColor();

        // Shell exit
        m_shellExited = m_terminalView->shellExited();
        m_shellExitCode = m_terminalView->shellExitCode();
        m_shellExitOverlayColor = m_terminalView->shellExitOverlayColor();
        m_shellExitTextColor = m_terminalView->shellExitTextColor();

        // Magnifier
        m_magnifierVisible = m_terminalView->magnifierVisible();
        m_draggingHandle = m_terminalView->draggingHandle();
        m_magnifierBorderColor = m_terminalView->magnifierBorderColor();

        if (m_draggingHandle == 1)
            m_magnifierFingerPos = m_selStart;
        else
            m_magnifierFingerPos = m_selEnd;

        // Link underlines — trigger scan if dirty, then extract viewport-relative spans
        if (m_terminalView->isLinkScanDirty())
            m_terminalView->refreshLinks();
        m_linkSpans.clear();
        const auto &links = m_terminalView->currentLinks();
        quint64 linkSig = 0xcbf29ce484222325ULL;
        for (const auto &link : links) {
            LinkSpan span;
            span.startRow = link.startRow;
            span.endRow = link.endRow;
            span.startCol = link.startCol;
            span.endCol = link.endCol;
            m_linkSpans.append(span);
            // Links scroll into view without a grid change; fold span geometry into the signature.
            linkSig = (linkSig ^ (quint64)link.startRow) * 0x100000001b3ULL;
            linkSig = (linkSig ^ (quint64)link.endRow) * 0x100000001b3ULL;
            linkSig = (linkSig ^ (quint64)link.startCol) * 0x100000001b3ULL;
            linkSig = (linkSig ^ (quint64)link.endCol) * 0x100000001b3ULL;
        }
        if (linkSig != m_prevLinkSig)
            m_sceneExternalChanged = true;
        m_prevLinkSig = linkSig;
    }

    // QBasicTimer is thread-affine; control is marshalled to the GUI thread (see m_shaderAnimTimer doc).
    //
    // A settings change may have already started/stopped the timer on the GUI
    // thread, so treat a generation bump as a clean slate for the queue flags.
    int animCfgGen = q->m_animCfgGeneration.loadAcquire();
    if (animCfgGen != m_lastAnimCfgGen) {
        m_lastAnimCfgGen = animCfgGen;
        m_animStartQueued = false;
        m_animStopQueuedForImpossible = false;
    }

    const bool trailsRenderable = m_es300 && q->m_wantsCursorTrails && !m_cursorTrailsFailed;
    const bool animPossible = trailsRenderable || !q->m_customShaderPath.isEmpty();

    // Consume the "settled" flag — it's only a hint. Re-validate against current
    // GUI-thread state; custom-shader path or overlay may have changed since
    // render() set it.
    if (m_shouldStopAnimTimer) {
        m_shouldStopAnimTimer = false;  // always consume, even if we skip the stop
        // Custom shaders read iTime — stopping the timer would freeze them.
        if (q->m_wantsCursorTrails
                && q->m_customShaderPath.isEmpty()
                && !m_selecting && !m_searchActive
                && !m_magnifierVisible && !m_shellExited) {
            m_animStartQueued = false;
            QMetaObject::invokeMethod(q, "stopShaderAnimTimer", Qt::QueuedConnection);
        }
    }

    // Impossible-stop: trails can never render (ES2-only GPU or trail shader load
    // failure) and no custom shader is active — the constructor-started 33ms timer
    // would otherwise run at 30fps forever, draining battery on exactly the weakest
    // devices. Fires once per config generation, not per sync.
    if (!animPossible) {
        m_animStartQueued = false;
        if (!m_animStopQueuedForImpossible) {
            m_animStopQueuedForImpossible = true;
            QMetaObject::invokeMethod(q, "stopShaderAnimTimer", Qt::QueuedConnection);
        }
    } else {
        m_animStopQueuedForImpossible = false;
    }

    // Marshalled to the GUI thread (see m_shaderAnimTimer doc); the queued invocation
    // lands right after this frame's render, so the first trail frame arrives one frame later — imperceptible.
    if (m_cursorMoved
            && trailsRenderable
            && q->m_customShaderPath.isEmpty()
            && !m_animStartQueued) {
        m_animStartQueued = true;
        QMetaObject::invokeMethod(q, "startShaderAnimTimer", Qt::QueuedConnection);
    }

    // Cache scrollbar offset for use in render() — avoids calling
    // ghostty_terminal_get() from the render thread (data race with GUI thread).
    GhosttyTerminalScrollbar scrollbar;
    memset(&scrollbar, 0, sizeof(scrollbar));
    if (m_terminalView && m_terminalView->vt()) {
        ghostty_terminal_get(m_terminalView->vt()->terminal(),
                             GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar);
        m_scrollOffset = scrollbar.offset;
    }

    // --- Notch-band overflow rows ---
    // The top padding band renders the k scrollback rows immediately above
    // the viewport — both while scrolled up AND at the bottom, where lines
    // scrolling off the top keep the band utilized. Offset > 0 is the gate:
    // it is exactly "rows exist above the viewport". The grid_ref walk is
    // signature-gated: it re-runs only when the band's inputs change — while
    // output streams that is per frame, bounded to k rows (a few), and never
    // when idle (grid_ref is not built for render-loop rates).
    GhosttyRenderStateDirty gridDirtyNow = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_DIRTY, &gridDirtyNow);
    GhosttyTerminalScreen activeScreen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    ghostty_terminal_get(m_terminalView->vt()->terminal(),
                         GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &activeScreen);
    const int bandHeight = m_terminalView->notchBandHeight();
    int bandK = 0;
    if (bandHeight > 0 && m_cellHeight > 0)
        bandK = qMax(1, m_topPadding / m_cellHeight);
    const bool bandActive = m_scrollOffset > 0
                            && activeScreen == GHOSTTY_TERMINAL_SCREEN_PRIMARY
                            && bandHeight > 0;

    // Palette hash: band colors are baked into vertices, so a palette
    // change (OSC 10/11) must re-fetch the rows.
    quint64 palHash = 0xcbf29ce484222325ULL;
    if (bandActive) {
        const auto *p = reinterpret_cast<const unsigned char *>(m_bandPalette);
        for (size_t i = 0; i < sizeof(m_bandPalette); ++i)
            palHash = (palHash ^ p[i]) * 0x100000001b3ULL;
    }

    BandSignature sig;
    sig.active = bandActive;
    sig.offset = m_scrollOffset;
    sig.k = bandK;
    sig.metricsGen = m_lastMetricsGeneration;
    sig.topPadding = m_topPadding;
    sig.cols = m_cols;
    sig.paletteHash = palHash;
    sig.gridDirty = (gridDirtyNow != GHOSTTY_RENDER_STATE_DIRTY_FALSE);
    if (sig != m_bandSignature) {
        const bool bandFlipped = bandActive != m_bandActive;
        // A k/topPadding change without a flip re-sizes the shrunk top strip
        // as well; the early return below would keep the old strip geometry
        // until the next dirty frame.
        const bool bandGeomChanged = (bandActive || m_bandActive)
            && (bandK != m_bandSignature.k || m_topPadding != m_bandSignature.topPadding);
        m_bandSignature = sig;
        m_bandActive = bandActive;
        m_bandK = bandK;
        if (bandActive) {
            fetchBandRows(m_terminalView->vt()->terminal());
        } else {
            m_bandVertices.clear();
            m_bandVertexCount = 0;
        }
        // A scroll/offset shift can leave ghostty's grid clean (prune-while-
        // pinned); without these the band would lag one position behind.
        // m_dirty uploads the new band VBO, m_gridDirty defeats the idle
        // pipeline skip. A bandActive flip or geometry change re-sizes the
        // top strip, which only re-emits on a full rebuild — force one so
        // the strip doesn't keep the old size behind the band.
        m_gridDirty = true;
        m_dirty = true;
        if (bandFlipped || bandGeomChanged)
            m_forceVertexRebuild = true;
    }

    // Snapshot kitty graphics enabled flag (avoids render-thread Settings access)
    m_kittyGraphicsEnabled = Settings::instance()->kittyGraphics();

    // Snapshot kitty placement data and sync texture cache (GUI thread — safe)
    if (m_terminalView && m_terminalView->vt()) {
        GhosttyTerminal kittyTerm = m_terminalView->vt()->terminal();
        snapshotKittyGraphics(kittyTerm, m_terminalView->vt());

        // Change detection via ghostty's storage-wide generation stamp
        // (see m_prevKittySceneSig doc); it also catches placement-only
        // mutations (re-crop, re-offset) that per-image generations miss,
        // and disabled graphics read as 0 so the enabled->disabled flip
        // redraws.
        quint64 kittySig = 0;
        if (m_kittyGraphicsEnabled && kittyTerm) {
            GhosttyKittyGraphics graphics = nullptr;
            ghostty_terminal_get(kittyTerm, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &graphics);
            if (graphics)
                ghostty_kitty_graphics_get(graphics,
                    GHOSTTY_KITTY_GRAPHICS_DATA_GENERATION, &kittySig);
        }
        if (kittySig != m_prevKittySceneSig)
            m_sceneExternalChanged = true;
        m_prevKittySceneSig = kittySig;
    }

    // Load/unload cursor trail shader based on setting
    // Custom shader path takes priority over cursor trails
    bool wantCustom = !q->m_customShaderPath.isEmpty() && m_es300;
    bool wantTrails = q->m_wantsCursorTrails && m_es300 && !wantCustom;

    // Custom shader: load when path is set, unload when cleared, reload when changed
    if (wantCustom && (!m_customShaderLoaded || q->m_customShaderDirty)) {
        // Clear cursor trails state if switching
        m_cursorTrailsLoaded = false;
        m_cursorTrailsFailed = false;
        loadPostShader(q->m_customShaderPath);
        m_customShaderLoaded = m_postShaderActive;
    } else if (!wantCustom && m_customShaderLoaded) {
        m_customShaderLoaded = false;
        // Don't delete program here — let cursor trails or full cleanup handle it
    }
    q->m_customShaderDirty = false;

    // Cursor trails: only when no custom shader is active
    if (wantTrails && !m_cursorTrailsLoaded && !m_cursorTrailsFailed) {
        loadPostShader(QStringLiteral(":/shaders/cursor_trail.glsl"));
        m_cursorTrailsLoaded = m_postShaderActive;
        if (!m_postShaderActive)
            m_cursorTrailsFailed = true; // don't retry every frame
    } else if (!wantTrails && (m_cursorTrailsLoaded || m_cursorTrailsFailed)) {
        m_cursorTrailsLoaded = false;
        m_cursorTrailsFailed = false;
    }

    // Full cleanup when no shader is wanted and none is active
    if (!wantCustom && !wantTrails && m_postShader.program && !m_cursorTrailsLoaded && !m_customShaderLoaded) {
        delete m_postShader.program;
        m_postShader.program = nullptr;
        m_postShaderActive = false;
        // Pipeline FBO teardown handled below (magnifier may still need it).
    }

    // Tear down pipeline FBO when magnifier was the only reason it existed
    // (no post-processing active, magnifier now hidden)
    if (!m_postShaderActive && !m_magnifierVisible && m_pipelineFbo)
        destroyPipelineFbo();

    // gridDirtyNow was read before the band block; render_state_clean() only
    // happens below, so it still holds the current value.
    if (gridDirtyNow == GHOSTTY_RENDER_STATE_DIRTY_FALSE && !m_forceVertexRebuild)
        return;

    // Full rebuild when ghostty reports FULL, when a metrics change
    // invalidated the atlas, when the grid geometry changed (incl. the
    // first frame), or when the top padding or viewport dimensions changed
    // (padding is baked into every row's y, and both padding and viewport
    // dims are baked into the strip geometry, so a partial would splice
    // clean rows/strips at the old values). Otherwise splice only the dirty
    // rows.
    bool full = (gridDirtyNow == GHOSTTY_RENDER_STATE_DIRTY_FULL) || m_forceVertexRebuild
                || gridSizeChanged || (m_topPadding != m_topPaddingAtBuild)
                || (m_viewportWidth != m_viewportWidthAtBuild)
                || (m_viewportHeight != m_viewportHeightAtBuild)
                || (m_bandActive != m_bandActiveAtBuild);
    if (full)
        buildCellVertices(state);
    else
        updateCellVertices(state);

    // Consume ghostty's dirty state on every path that leaves the branch
    // above (even when the partial walk found zero dirty rows); skipping
    // this leaves the dirty state undrained, causing redundant rebuild work
    // every frame. The early-return path above must NOT clean (that would
    // drop unrendered frames).
    ghostty_render_state_clean(state);
    m_forceVertexRebuild = false;
    m_gridDirty = true;
    m_dirty = true;
}

void GLRenderer::Renderer::renderMagnifier(const QMatrix4x4 &proj, int fboW, int fboH)
{
    if (!m_magnifierVisible || !m_selecting || m_selStart == m_selEnd
        || !m_magProgram || !m_magProgram->isLinked() || !m_pipelineTex)
        return;

    int srcW = TerminalView::MagnifierWidth / TerminalView::MagnifierZoom;
    int srcH = TerminalView::MagnifierHeight / TerminalView::MagnifierZoom;
    int srcX = static_cast<int>(m_magnifierFingerPos.x()) - srcW / 2;
    int srcY = static_cast<int>(m_magnifierFingerPos.y()) - srcH / 2;
    if (fboW > srcW && fboH > srcH) {
        srcX = qBound(0, srcX, fboW - srcW);
        srcY = qBound(0, srcY, fboH - srcH);
    } else {
        srcX = qMax(0, srcX);
        srcY = qMax(0, srcY);
    }

    // Sample directly from m_pipelineTex to avoid a TBDR pipeline stall (glCopyTexSubImage2D would flush the tiled renderer).

    buildMagnifierVertices(fboW, fboH);

    if (m_magVertexCount > 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        m_magProgram->bind();
        m_magProgram->setUniformValue(m_magMatrixUniform, proj);
        m_magProgram->setUniformValue(m_magTexUniform, 0);

        int destX = static_cast<int>(m_magnifierFingerPos.x()) - TerminalView::MagnifierWidth / 2;
        int destY = static_cast<int>(m_magnifierFingerPos.y()) - TerminalView::MagnifierHeight - TerminalView::MagnifierOffset;
        if (destY < 0)
            destY = static_cast<int>(m_magnifierFingerPos.y()) + TerminalView::MagnifierOffset;
        destX = qBound(0, destX, fboW - TerminalView::MagnifierWidth);
        destY = qBound(0, destY, fboH - TerminalView::MagnifierHeight);

        m_magProgram->setUniformValue(m_magDestRectUniform,
            static_cast<float>(destX), static_cast<float>(destY),
            static_cast<float>(TerminalView::MagnifierWidth),
            static_cast<float>(TerminalView::MagnifierHeight));
        m_magProgram->setUniformValue(m_magCornerRadiusUniform, 8.0f);

        // Source rectangle in m_pipelineTex and its dimensions (for UV normalization)
        m_magProgram->setUniformValue(m_magSrcRectUniform,
            static_cast<float>(srcX), static_cast<float>(srcY),
            static_cast<float>(srcW), static_cast<float>(srcH));
        m_magProgram->setUniformValue(m_magSrcTexSizeUniform,
            static_cast<float>(m_pipelineTexW), static_cast<float>(m_pipelineTexH));

        float ba = m_magnifierBorderColor.alphaF();
        float br = m_magnifierBorderColor.redF() * ba;
        float bg = m_magnifierBorderColor.greenF() * ba;
        float bb = m_magnifierBorderColor.blueF() * ba;
        m_magProgram->setUniformValue(m_magBorderColorUniform, br, bg, bb, ba);
        m_magProgram->setUniformValue(m_magBorderWidthUniform, 2.0f);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_pipelineTex);

        const int magStride = 4 * sizeof(float);
        glVertexAttribPointer(m_magPositionAttr, 2, GL_FLOAT, GL_FALSE, magStride,
                              m_magVertices.constData());
        glVertexAttribPointer(m_magTexcoordAttr, 2, GL_FLOAT, GL_FALSE, magStride,
                              m_magVertices.constData() + 2);
        glEnableVertexAttribArray(m_magPositionAttr);
        glEnableVertexAttribArray(m_magTexcoordAttr);
        glDrawArrays(GL_TRIANGLES, 0, m_magVertexCount);
        glDisableVertexAttribArray(m_magPositionAttr);
        glDisableVertexAttribArray(m_magTexcoordAttr);

        glBindTexture(GL_TEXTURE_2D, 0);
        m_magProgram->release();
        glDisable(GL_BLEND);
    }
}

bool GLRenderer::Renderer::renderPostProcessPipeline(QOpenGLFramebufferObject *fbo)
{
    if (!m_timerStarted) {
        m_elapsedTimer.start();
        m_lastFrameNs = 0;
        m_postTime = 0.0f;
        m_postTimeDelta = 0.0f;
        m_postFrameRate = 0.0f;
        m_postFrame = 0;
        m_timerStarted = true;
    }
    qint64 nowNs = m_elapsedTimer.nsecsElapsed();
    if (m_lastFrameNs > 0) {
        m_postTimeDelta = static_cast<float>((nowNs - m_lastFrameNs) / 1000000000.0);
        if (m_postTimeDelta > 0.0f)
            m_postFrameRate = 1.0f / m_postTimeDelta;
    }
    m_lastFrameNs = nowNs;
    m_postTime = static_cast<float>(nowNs / 1000000000.0);
    m_postFrame++;

    if (m_cursorMoved) {
        m_cursorChangeTime = m_postTime;
        m_cursorMoved = false;
        m_animationSettled = false;
    }

    if (!m_animationSettled && m_postTime - m_cursorChangeTime > kAnimationSettleDelay) {
        m_animationSettled = true;
    }

    bool overlayActive = m_selecting || m_searchActive || m_magnifierVisible || m_shellExited;
    // m_sceneExternalChanged covers changes ghostty's dirty flag misses
    // (kitty image updates, cursor style flips, link span changes) — see
    // the members' doc in glrenderer.h.
    bool canSkipPipeline = m_animationSettled && !m_gridDirty && !overlayActive
                           && !m_sceneExternalChanged;
    m_gridDirty = false;
    m_sceneExternalChanged = false;

    // Safe to set unconditionally — synchronize() re-validates, and the next
    // render() overwrites this if conditions change.
    if (canSkipPipeline)
        m_shouldStopAnimTimer = true;

    int oldPipeW = m_pipelineTexW, oldPipeH = m_pipelineTexH;
    createPipelineFbo(fbo->width(), fbo->height());
    if (m_pipelineTexW != oldPipeW || m_pipelineTexH != oldPipeH)
        canSkipPipeline = false;

    if (!m_pipelineFbo)
        return false;

    if (!canSkipPipeline) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_pipelineFbo);
        glViewport(0, 0, fbo->width(), fbo->height());
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        drawScene(fbo->width(), fbo->height());
        glDisable(GL_BLEND);
    }

    runPostProcessPass(m_postShader, m_pipelineTex, fbo->handle(), fbo->width(), fbo->height());

    return true;
}

void GLRenderer::Renderer::drawScene(int width, int height)
{
    QMatrix4x4 proj;
    proj.ortho(0, width, 0, height, -1, 1);

    glActiveTexture(GL_TEXTURE0);
    m_atlas.bind();

    m_program->bind();
    m_program->setUniformValue(m_matrixUniform, proj);
    m_program->setUniformValue(m_atlasUniform, 0);
    m_program->setUniformValue(m_cursorPosUniform, m_cursorX, m_cursorY);
    m_program->setUniformValue(m_cursorWidthUniform, static_cast<float>(m_cursorWidth));
    m_program->setUniformValue(m_cellSizeUniform,
                               static_cast<float>(m_cellWidth),
                               static_cast<float>(m_cellHeight));
    m_program->setUniformValue(m_cursorBlinkUniform, m_cursorVisible ? 1.0f : 0.0f);
    m_program->setUniformValue(m_cursorStyleUniform, static_cast<float>(m_cursorStyle));
    m_program->setUniformValue(m_topPaddingUniform, static_cast<float>(m_topPadding));

    // The cell pass is split so below-text kitty images composite between the
    // cell backgrounds and the glyphs (Kitty semantics: z<0 = above the
    // background, under the text only). u_pass: 0 = background only, 1 = text.
    // BELOW_BG placements are never snapshotted and are not rendered here.
    const bool splitCellPass = m_passUniform >= 0;
    if (splitCellPass)
        m_program->setUniformValue(m_passUniform, 0.0f);
    bindCellVertexFormat(m_vbo);
    glDrawArrays(GL_TRIANGLES, 0, m_vertexCount);

    // Kitty below-text layer: over the backgrounds, under the glyphs.
    drawKittyImageLayer(GHOSTTY_KITTY_PLACEMENT_LAYER_BELOW_TEXT, proj,
                        width, height);

    if (splitCellPass) {
        // The kitty pass bound its own program, textures, and (in ES2, global)
        // vertex attrib state — re-bind ours and re-assert the cell vertex
        // format before drawing the text pass. Uniform values persist in the
        // program object; re-binding restores them.
        m_program->bind();
        m_atlas.bind();
        m_program->setUniformValue(m_passUniform, 1.0f);
        bindCellVertexFormat(m_vbo);
        glDrawArrays(GL_TRIANGLES, 0, m_vertexCount);
    }

    // Notch-band overflow rows: own VBO, drawn after the main cells so the
    // band's scrollback content composites over the (shrunk) top strip. Same
    // two-draw bg/glyph split as the main cells when the pass is split.
    if (m_bandVertexCount > 0) {
        if (splitCellPass)
            m_program->setUniformValue(m_passUniform, 0.0f);
        bindCellVertexFormat(m_bandVbo);
        glDrawArrays(GL_TRIANGLES, 0, m_bandVertexCount);
        if (splitCellPass) {
            m_program->setUniformValue(m_passUniform, 1.0f);
            bindCellVertexFormat(m_bandVbo);
            glDrawArrays(GL_TRIANGLES, 0, m_bandVertexCount);
        }
        m_bandVbo.release();
    }

    if (m_positionAttr >= 0) glDisableVertexAttribArray(m_positionAttr);
    if (m_texcoordAttr >= 0) glDisableVertexAttribArray(m_texcoordAttr);
    if (m_fgColorAttr >= 0) glDisableVertexAttribArray(m_fgColorAttr);
    if (m_bgColorAttr >= 0) glDisableVertexAttribArray(m_bgColorAttr);
    if (m_decoAttr >= 0) glDisableVertexAttribArray(m_decoAttr);
    m_vbo.release();
    m_program->release();

    buildOverlayVertices(width, height);
    if (m_flatVertexCount > 0 && m_flatProgram && m_flatProgram->isLinked()) {
        m_flatProgram->bind();
        m_flatProgram->setUniformValue(m_flatMatrixUniform, proj);
        m_flatVbo.bind();
        const int flatStride = 6 * sizeof(float);
        if (m_flatPositionAttr >= 0) {
            glEnableVertexAttribArray(m_flatPositionAttr);
            glVertexAttribPointer(m_flatPositionAttr, 2, GL_FLOAT, GL_FALSE, flatStride, nullptr);
        }
        if (m_flatColorAttr >= 0) {
            glEnableVertexAttribArray(m_flatColorAttr);
            glVertexAttribPointer(m_flatColorAttr, 4, GL_FLOAT, GL_FALSE, flatStride,
                                  reinterpret_cast<void*>(2 * sizeof(float)));
        }
        glDrawArrays(GL_TRIANGLES, 0, m_flatVertexCount);
        if (m_flatPositionAttr >= 0) glDisableVertexAttribArray(m_flatPositionAttr);
        if (m_flatColorAttr >= 0) glDisableVertexAttribArray(m_flatColorAttr);
        m_flatVbo.release();
        m_flatProgram->release();
    }

    drawKittyImageLayer(GHOSTTY_KITTY_PLACEMENT_LAYER_ABOVE_TEXT, proj,
                        width, height);
}

void GLRenderer::Renderer::renderDirectToFbo(QOpenGLFramebufferObject *fbo)
{
    // createPipelineFbo() unconditionally ends with glBindFramebuffer(0), so a
    // failed pipeline-FBO creation leaves GL bound to framebuffer 0 — re-bind
    // Qt's FBO before clearing/drawing the scene.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle());
    glViewport(0, 0, fbo->width(), fbo->height());
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    drawScene(fbo->width(), fbo->height());

    glDisable(GL_BLEND);
}

void GLRenderer::Renderer::renderShellExitText(QOpenGLFramebufferObject *fbo)
{
    QOpenGLPaintDevice device(fbo->size());
    device.setPaintFlipped(true);
    QPainter painter(&device);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(m_shellExitTextColor);
    QFont font = painter.font();
    font.setPointSize(m_cachedFontSize + 4);
    font.setBold(true);
    painter.setFont(font);

    QString exitText;
    if (m_shellExitCode == PtyManager::kExecFailedExitCode) {
        exitText = GLRenderer::tr("Command not found");
    } else {
        exitText = GLRenderer::tr("Shell exited with code %1").arg(m_shellExitCode);
    }
    painter.drawText(QRectF(0, 0, fbo->width(), fbo->height()),
                     Qt::AlignCenter, exitText);

    painter.end();
}

void GLRenderer::Renderer::render()
{
    if (!m_initialized)
        initialize();

    // Drain deferred kitty texture deletions (GL context is current on render thread)
    drainPendingKittyDeletions();

    if (!m_program || !m_program->isLinked()) {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    if (m_dirty) {
        rebuildVBO();
        rebuildBandVBO();
        m_dirty = false;
    }

    if (m_vertexCount == 0) {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    // Reset state that Qt's scene graph may have left behind
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);

    QOpenGLFramebufferObject *fbo = framebufferObject();

    bool magnifierActive = m_magnifierVisible && m_selecting && m_selStart != m_selEnd;

    bool didPostProcess = false;
    if (m_postShaderActive && m_es300 && m_postShader.program) {
        didPostProcess = renderPostProcessPipeline(fbo);
    }

    if (!didPostProcess) {
        // Guard the detour on the blit shader: blitPipelineToFbo() silently no-ops
        // without it, leaving the terminal blank.
        if (magnifierActive && m_blitProgram && m_blitProgram->isLinked()) {
            // Magnifier samples m_pipelineTex, which only exists when post-processing
            // is active. Create it on demand so the magnifier pass below has a source.
            createPipelineFbo(fbo->width(), fbo->height());
            if (m_pipelineFbo) {
                // Always re-render the scene to the pipeline FBO so the
                // magnifier shows live content (cursor blink, new output).
                glBindFramebuffer(GL_FRAMEBUFFER, m_pipelineFbo);
                glViewport(0, 0, fbo->width(), fbo->height());
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                drawScene(fbo->width(), fbo->height());
                glDisable(GL_BLEND);
                blitPipelineToFbo(fbo);
            } else {
                // Fallback: render directly (magnifier won't show if m_pipelineTex is null)
                renderDirectToFbo(fbo);
            }
        } else {
            renderDirectToFbo(fbo);
        }
    }

    // Render the magnifier as a separate pass after the scene is in Qt's FBO.
    // At this point Qt's FBO is the current render target and m_pipelineTex
    // is NOT the current render target — safe to sample from without a
    // self-texture feedback loop.
    if (m_magnifierVisible && m_pipelineTex) {
        QMatrix4x4 proj;
        proj.ortho(0, fbo->width(), 0, fbo->height(), -1, 1);
        renderMagnifier(proj, fbo->width(), fbo->height());
    }

    if (m_shellExited) {
        renderShellExitText(fbo);
    }
}
