#ifndef GLRENDERER_H
#define GLRENDERER_H

#include <QQuickFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QHash>
#include <QVector>
#include <QList>
#include <QPointF>
#include <QColor>
#include <QFont>
#include <QAtomicInt>
#include <QElapsedTimer>
#include <QBasicTimer>
#include "glyphatlas.h"
#include "terminalview.h"
#include "textutil.h"

class GhosttyVt;

// Ghostty-compatible post-processing shader uniform locations
struct PostShaderUniforms {
    int iResolution = -1;
    int iTime = -1;
    int iTimeDelta = -1;
    int iFrameRate = -1;
    int iFrame = -1;
    int iChannelTime = -1;
    int iChannelResolution = -1;
    int iMouse = -1;
    int iDate = -1;
    int iSampleRate = -1;
    int iCurrentCursor = -1;
    int iPreviousCursor = -1;
    int iCurrentCursorColor = -1;
    int iPreviousCursorColor = -1;
    int iCurrentCursorStyle = -1;
    int iPreviousCursorStyle = -1;
    int iCursorVisible = -1;
    int iTimeCursorChange = -1;
    int iTimeFocus = -1;
    int iFocus = -1;
    int iPalette = -1;
    int iBackgroundColor = -1;
    int iForegroundColor = -1;
    int iCursorColor = -1;
    int iCursorText = -1;
    int iSelectionForegroundColor = -1;
    int iSelectionBackgroundColor = -1;
    int iChannel0 = -1;
};

// Post-processing shader program + uniform locations
struct PostShader {
    QOpenGLShaderProgram *program = nullptr;
    PostShaderUniforms loc;
};

// Per-cell vertex: 13 floats
struct CellVertex {
    float x, y;       // position in pixel coords
    float u, v;       // atlas texture coords
    float fgR, fgG, fgB, fgA; // foreground (premultiplied)
    float bgR, bgG, bgB, bgA; // background (premultiplied)
    float deco;       // decoration type: 0=none, 1=underline, 2=strikethrough
};

class GLRenderer : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* source READ source WRITE setSource NOTIFY sourceChanged)

public:
    explicit GLRenderer(QQuickItem *parent = nullptr);

    QObject *source() const { return m_source; }
    void setSource(QObject *source);

    QQuickFramebufferObject::Renderer *createRenderer() const override;

Q_SIGNALS:
    void sourceChanged();

private:
    void invalidateMetrics();
    void timerEvent(QTimerEvent *event) override;

    QObject *m_source = nullptr;
    QAtomicInt m_metricsGeneration; // Incremented on font/opacity change, checked in synchronize()
    QBasicTimer m_shaderAnimTimer;
    bool m_wantsCursorTrails = false;
    bool m_customShaderDirty = false;
    QString m_customShaderPath;

    // Settings snapshots (GUI thread) consumed by Renderer on render thread
    struct CachedMetrics {
        QString fontFamily;
        int fontSize = 18;
        float backgroundOpacity = 1.0f;
    } m_cachedMetrics;

    class Renderer : public QQuickFramebufferObject::Renderer, protected QOpenGLExtraFunctions
    {
    public:
        Renderer();
        ~Renderer();

        void render() override;
        void synchronize(QQuickFramebufferObject *item) override;
        QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override;

    private:
        static constexpr float kCursorUnset = -1.0f;
        static constexpr float kAnimationSettleDelay = 0.5f; // Must exceed shader animation window (~0.49s)
        static const int kPaletteSize = 256;
        static const int kPaletteFloats = kPaletteSize * 3; // vec3 per entry

        void initialize();
        void createShaders();
        void createVBO();
        void rebuildVBO();
        void createFlatShaders();
        void createFlatVBO();
        void buildOverlayVertices(int fboW, int fboH);
        void appendCircle(float cx, float cy, float radius, float r, float g, float b, float a, int segments = 24);
        void createMagShaders();
        void createMagTexture();
        void buildMagnifierVertices(int fboW, int fboH);

        // Refactored render sub-methods
        bool renderPostProcessPipeline(QOpenGLFramebufferObject *fbo);
        void renderDirectToFbo(QOpenGLFramebufferObject *fbo);
        void renderShellExitText(QOpenGLFramebufferObject *fbo);
        void buildCellVertices(GhosttyRenderState state);

        // Phase 5B: post-processing pipeline
        void detectES300();
        void createPipelineFbo(int w, int h);
        void destroyPipelineFbo();
        void createPingPongFbo(int w, int h);
        void destroyPingPongFbo();
        void createPostShaders();
        void loadPostShader(const QString &path);
        void uploadPostShaderUniforms(PostShader &shader, int fboW, int fboH);
        void runPostProcessPass(PostShader &shader, GLuint inputTex, GLuint outputFbo, int w, int h);
        void renderMagnifier(const QMatrix4x4 &proj, int fboW, int fboH);

        // Kitty Graphics Protocol — rendering helpers
        void createKittyShaders();
        void drawKittyImageLayer(GhosttyKittyPlacementLayer layer,
                                 const QMatrix4x4 &proj, int fboW, int fboH);
        void syncKittyImages(GhosttyTerminal terminal, GhosttyVt *vt);
        void cleanupKittyCache();

        QOpenGLShaderProgram *m_program = nullptr;
        QOpenGLBuffer m_vbo;
        bool m_initialized = false;
        int m_matrixUniform = -1;
        int m_atlasUniform = -1;
        int m_cursorPosUniform = -1;
        int m_cellSizeUniform = -1;
        int m_cursorBlinkUniform = -1;
        int m_cursorStyleUniform = -1;
        int m_topPaddingUniform = -1;
        int m_positionAttr = -1;
        int m_texcoordAttr = -1;
        int m_fgColorAttr = -1;
        int m_bgColorAttr = -1;
        int m_decoAttr = -1;

        GlyphAtlas m_atlas;
        bool m_atlasInitialized = false;

        // Terminal state snapshot (populated in synchronize, consumed in render)
        QVector<CellVertex> m_cellVertices;
        int m_vertexCount = 0;
        bool m_dirty = false;
        int m_cellWidth = 0;
        int m_cellHeight = 0;
        int m_topPadding = 0;
        int m_cols = 0;
        int m_rows = 0;
        float m_bgOpacity = 1.0f;
        int m_cachedFontSize = 18;
        int m_lastMetricsGeneration = -1;
        // Cursor state (updated via uniforms, not vertex data)
        float m_cursorX = kCursorUnset;
        float m_cursorY = kCursorUnset;
        bool m_cursorVisible = false;
        int m_cursorStyle = 0; // 0=none, 1=block, 2=bar, 3=underline, 4=hollow
        int m_prevCursorX = -1; // matches kCursorUnset sentinel
        int m_prevCursorY = -1;
        float m_cursorChangeTime = 0.0f;
        bool m_cursorMoved = false;
        bool m_cursorTrailsLoaded = false;
        bool m_cursorTrailsFailed = false;
        bool m_customShaderLoaded = false;

        // Idle optimization: skip pipeline when animation settled + grid unchanged
        bool m_animationSettled = false;
        bool m_gridDirty = true;

        TerminalView *m_terminalView = nullptr;

        bool m_selecting = false;
        QPointF m_selStart;
        QPointF m_selEnd;
        bool m_handlesVisible = false;
        QColor m_selectionHighlightColor;
        QColor m_selectionHandleColor;
        QColor m_selectionHandleBorderColor;

        bool m_searchActive = false;
        QVector<TerminalView::SearchMatch> m_searchMatches;
        int m_currentMatchIndex = -1;
        QColor m_searchHighlightColor;
        QColor m_searchCurrentColor;

        bool m_shellExited = false;
        int m_shellExitCode = 0;
        QColor m_shellExitOverlayColor;
        QColor m_shellExitTextColor;

        bool m_magnifierVisible = false;
        int m_draggingHandle = -1;
        QColor m_magnifierBorderColor;
        QPointF m_magnifierFingerPos;

        int m_scrollOffset = 0;

        struct LinkSpan { int startRow; int endRow; int startCol; int endCol; };
        QVector<LinkSpan> m_linkSpans;

        // Flat-color shader (selection, search, link overlays)
        QOpenGLShaderProgram *m_flatProgram = nullptr;
        QOpenGLBuffer m_flatVbo;
        int m_flatMatrixUniform = -1;
        int m_flatPositionAttr = -1;
        int m_flatColorAttr = -1;
        QVector<float> m_flatVertices; // interleaved pos2+color4 = 6 floats per vertex
        int m_flatVertexCount = 0;

        // Magnifier shader (textured rounded-rect with SDF clip)
        QOpenGLShaderProgram *m_magProgram = nullptr;
        GLuint m_magnifierTex = 0;
        int m_magMatrixUniform = -1;
        int m_magTexUniform = -1;
        int m_magDestRectUniform = -1;
        int m_magCornerRadiusUniform = -1;
        int m_magBorderColorUniform = -1;
        int m_magBorderWidthUniform = -1;
        int m_magPositionAttr = -1;
        int m_magTexcoordAttr = -1;
        QVector<float> m_magVertices;
        int m_magVertexCount = 0;

        // Phase 5B: ES 3.0 post-processing pipeline
        bool m_es300 = false;
        bool m_postShaderActive = false;

        GLuint m_pipelineTex = 0;
        GLuint m_pipelineFbo = 0;
        int m_pipelineTexW = 0;
        int m_pipelineTexH = 0;

        // Ping-pong FBO for multi-pass shaders
        GLuint m_pingPongTex = 0;
        GLuint m_pingPongFbo = 0;
        int m_pingPongTexW = 0;
        int m_pingPongTexH = 0;

        PostShader m_postShader;

        // Multi-pass shader list — infrastructure for future bloom/blur effects.
        // Currently unused; ping-pong FBO is only created when this list has >1 entry.
        QList<PostShader> m_postShaders;

        QElapsedTimer m_elapsedTimer;
        bool m_timerStarted = false;
        qint64 m_lastFrameNs = 0;
        float m_postTime = 0.0f;
        float m_postTimeDelta = 0.0f;
        float m_postFrameRate = 0.0f;
        unsigned int m_postFrame = 0;

        // Terminal color state for post shader uniforms (populated in synchronize)
        float m_postPaletteData[kPaletteFloats]; // 256 * vec3
        float m_postBgR = 0.0f, m_postBgG = 0.0f, m_postBgB = 0.0f;
        float m_postFgR = 1.0f, m_postFgG = 1.0f, m_postFgB = 1.0f;
        float m_postCursorR = 1.0f, m_postCursorG = 1.0f, m_postCursorB = 1.0f;
        bool m_postCursorColorHasValue = false;

        // Kitty Graphics Protocol — image rendering
        struct KittyCachedTexture {
            GLuint texture;
            uint32_t width, height;
            uint32_t lastSeenFrame;
            size_t dataLen;  // track for replacement detection
        };
        QHash<uint32_t, KittyCachedTexture> m_kittyTextures;
        uint32_t m_kittyFrameCounter = 0;
        static const int MAX_KITTY_TEXTURES = 32;
        static const int KITTY_EVICTION_FRAMES = 120;

        // Kitty image shader (textured quad, premultiplied alpha)
        QOpenGLShaderProgram *m_kittyProgram = nullptr;
        int m_kittyMatrixUniform = -1;
        int m_kittyTexUniform = -1;
        int m_kittyPositionAttr = -1;
        int m_kittyTexcoordAttr = -1;

    };
};

#endif // GLRENDERER_H
