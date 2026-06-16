#include "glrenderer.h"
#include "terminalview.h"
#include "ghosttyvt.h"
#include "settings.h"

#include <cmath>
#include <cstring>
#include <QDebug>
#include <QMatrix4x4>
#include <QFontMetrics>
#include <QOpenGLContext>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QDateTime>
#include <QFile>

// GLSL ES 1.00 shaders — textured cell quads with per-cell fg/bg colors
// Cursor blink and style are handled in the fragment shader via uniforms to avoid
// rebuilding vertex data on every blink toggle or cursor style change.
static const char *vertexShaderSource =
    "attribute vec2 position;\n"
    "attribute vec2 texcoord;\n"
    "attribute vec4 fg_color;\n"
    "attribute vec4 bg_color;\n"
    "attribute float deco_type;\n"
    "uniform mat4 u_matrix;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec2 v_cell;\n"
    "varying vec4 v_fg_color;\n"
    "varying vec4 v_bg_color;\n"
    "varying float v_deco;\n"
    "void main() {\n"
    "    gl_Position = u_matrix * vec4(position, 0.0, 1.0);\n"
    "    v_texcoord = texcoord;\n"
    "    v_cell = position;\n"
    "    v_fg_color = fg_color;\n"
    "    v_bg_color = bg_color;\n"
    "    v_deco = deco_type;\n"
    "}\n";

static const char *fragmentShaderSource =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec2 v_cell;\n"
    "varying vec4 v_fg_color;\n"
    "varying vec4 v_bg_color;\n"
    "varying float v_deco;\n"
    "uniform sampler2D u_atlas;\n"
    "uniform vec2 u_cursorPos;\n"
    "uniform vec2 u_cellSize;\n"
    "uniform float u_cursorBlink;\n"
    "uniform float u_cursorStyle;\n"
    "uniform float u_topPadding;\n"
    "void main() {\n"
    "    vec4 fg = v_fg_color;\n"
    "    vec4 bg = v_bg_color;\n"
    "    vec2 adj_cell = v_cell - vec2(0.0, u_topPadding);\n"
    "    if (u_cursorBlink > 0.5 && u_cellSize.x > 0.0) {\n"
    "        vec2 cellCoord = floor(adj_cell / u_cellSize);\n"
    "        if (cellCoord == u_cursorPos) {\n"
    "            if (u_cursorStyle < 1.5) {\n"
    "                fg = v_bg_color;\n"
    "                bg = v_fg_color;\n"
    "            } else if (u_cursorStyle < 2.5) {\n"
    "                float cx = adj_cell.x - cellCoord.x * u_cellSize.x;\n"
    "                if (cx < 2.0) {\n"
    "                    gl_FragColor = v_fg_color;\n"
    "                    return;\n"
    "                }\n"
    "            } else if (u_cursorStyle < 3.5) {\n"
    "                float cy = adj_cell.y - cellCoord.y * u_cellSize.y;\n"
    "                if (cy > u_cellSize.y - 2.0) {\n"
    "                    gl_FragColor = v_fg_color;\n"
    "                    return;\n"
    "                }\n"
    "            } else {\n"
    "                float cx = adj_cell.x - cellCoord.x * u_cellSize.x;\n"
    "                float cy = adj_cell.y - cellCoord.y * u_cellSize.y;\n"
    "                if (cx < 1.0 || cx > u_cellSize.x - 1.0 ||\n"
    "                    cy < 1.0 || cy > u_cellSize.y - 1.0) {\n"
    "                    gl_FragColor = v_fg_color;\n"
    "                    return;\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    float glyph_alpha = texture2D(u_atlas, v_texcoord).a;\n"
    "    vec4 color = mix(bg, fg, glyph_alpha);\n"
    "    // Text decorations: v_deco encodes type (0=none, 1=underline, 2=strikethrough)\n"
    "    if (v_deco > 0.5 && u_cellSize.x > 0.0) {\n"
    "        float cy = adj_cell.y - floor(adj_cell.y / u_cellSize.y) * u_cellSize.y;\n"
    "        if (v_deco < 1.5) {\n"
    "            // Underline: 2px at bottom of cell\n"
    "            if (cy > u_cellSize.y - 2.0)\n"
    "                color = vec4(fg.rgb, 1.0);\n"
    "        } else {\n"
    "            // Strikethrough: 2px at middle of cell\n"
    "            if (cy > u_cellSize.y * 0.5 - 1.0 && cy < u_cellSize.y * 0.5 + 1.0)\n"
    "                color = vec4(fg.rgb, 1.0);\n"
    "        }\n"
    "    }\n"
    "    gl_FragColor = color;\n"
    "}\n";

// GLSL ES 1.00 flat-color shaders — solid-color rects for selection, search, link overlays
static const char *flatVertexShaderSource =
    "attribute vec2 position;\n"
    "attribute vec4 color;\n"
    "uniform mat4 u_matrix;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    gl_Position = u_matrix * vec4(position, 0.0, 1.0);\n"
    "    v_color = color;\n"
    "}\n";

static const char *flatFragmentShaderSource =
    "precision mediump float;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    gl_FragColor = v_color;\n"
    "}\n";

// GLSL ES 1.00 magnifier shaders — textured rounded-rect with SDF clip
static const char *magVertexShaderSource =
    "attribute vec2 position;\n"
    "attribute vec2 texcoord;\n"
    "uniform mat4 u_matrix;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec2 v_pos;\n"
    "void main() {\n"
    "    gl_Position = u_matrix * vec4(position, 0.0, 1.0);\n"
    "    v_texcoord = texcoord;\n"
    "    v_pos = position;\n"
    "}\n";

static const char *magFragmentShaderSource =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec2 v_pos;\n"
    "uniform sampler2D u_magnifierTex;\n"
    "uniform vec4 u_destRect;\n"
    "uniform float u_cornerRadius;\n"
    "uniform vec4 u_borderColor;\n"
    "uniform float u_borderWidth;\n"
    "\n"
    "float roundedRectSDF(vec2 p, vec2 center, vec2 halfSize, float radius) {\n"
    "    vec2 d = abs(p - center) - halfSize + radius;\n"
    "    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - radius;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec2 center = u_destRect.xy + u_destRect.zw * 0.5;\n"
    "    vec2 halfSize = u_destRect.zw * 0.5;\n"
    "    float dist = roundedRectSDF(v_pos, center, halfSize, u_cornerRadius);\n"
    "    if (dist > 1.5) discard;\n"
    "    vec4 texColor = texture2D(u_magnifierTex, v_texcoord);\n"
    "    float edgeAlpha = smoothstep(0.0, 1.5, -dist);\n"
    "    float borderMask = smoothstep(u_borderWidth, u_borderWidth - 1.5, -dist);\n"
    "    vec4 color = mix(texColor, u_borderColor, borderMask);\n"
    "    gl_FragColor = vec4(color.rgb * edgeAlpha, color.a * edgeAlpha);\n"
    "}\n";

// --- Phase 5B: ES 3.0 post-processing shaders ---

// Full-screen triangle vertex shader (ES 3.0, no VBO needed — gl_VertexID trick)
static const char *postVertexShaderSource =
    "#version 300 es\n"
    "void main() {\n"
    "    vec4 position;\n"
    "    position.x = (gl_VertexID == 2) ? 3.0 : -1.0;\n"
    "    position.y = (gl_VertexID == 0) ? -3.0 : 1.0;\n"
    "    position.z = 1.0;\n"
    "    position.w = 1.0;\n"
    "    gl_Position = position;\n"
    "}\n";

// Ghostty-compatible shadertoy prefix for ES 3.0 (individual uniforms, no UBO)
static const char *shadertoyPrefixES300 =
    "#version 300 es\n"
    "precision mediump float;\n"
    "\n"
    "uniform vec3  iResolution;\n"
    "uniform float iTime;\n"
    "uniform float iTimeDelta;\n"
    "uniform float iFrameRate;\n"
    "uniform int   iFrame;\n"
    "uniform float iChannelTime[4];\n"
    "uniform vec3  iChannelResolution[4];\n"
    "uniform vec4  iMouse;\n"
    "uniform vec4  iDate;\n"
    "uniform float iSampleRate;\n"
    "uniform vec4  iCurrentCursor;\n"
    "uniform vec4  iPreviousCursor;\n"
    "uniform vec4  iCurrentCursorColor;\n"
    "uniform vec4  iPreviousCursorColor;\n"
    "uniform int   iCurrentCursorStyle;\n"
    "uniform int   iPreviousCursorStyle;\n"
    "uniform int   iCursorVisible;\n"
    "uniform float iTimeCursorChange;\n"
    "uniform float iTimeFocus;\n"
    "uniform int   iFocus;\n"
    "uniform vec3  iPalette[256];\n"
    "uniform vec3  iBackgroundColor;\n"
    "uniform vec3  iForegroundColor;\n"
    "uniform vec3  iCursorColor;\n"
    "uniform vec3  iCursorText;\n"
    "uniform vec3  iSelectionForegroundColor;\n"
    "uniform vec3  iSelectionBackgroundColor;\n"
    "\n"
    "uniform sampler2D iChannel0;\n"
    "\n"
    "out vec4 _fragColor;\n"
    "\n"
    "#define texture2D texture\n"
    "\n"
    "#define CURSORSTYLE_BLOCK        0\n"
    "#define CURSORSTYLE_BLOCK_HOLLOW 1\n"
    "#define CURSORSTYLE_BAR          2\n"
    "#define CURSORSTYLE_UNDERLINE    3\n"
    "#define CURSORSTYLE_LOCK         4\n"
    "\n"
    "void mainImage( out vec4 fragColor, in vec2 fragCoord );\n"
    "void main() { mainImage (_fragColor, gl_FragCoord.xy); }\n";

// --- GLRenderer implementation ---

GLRenderer::GLRenderer(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
    // Connect settings signals once — Settings is a singleton that never changes.
    // Font/opacity changes invalidate cached metrics via atomic generation counter.
    Settings *s = Settings::instance();
    connect(s, &Settings::fontSizeChanged, this, &GLRenderer::invalidateMetrics);
    connect(s, &Settings::fontFamilyChanged, this, &GLRenderer::invalidateMetrics);
    connect(s, &Settings::backgroundOpacityChanged, this, &GLRenderer::invalidateMetrics);

    // Populate initial cached values so the first render uses actual settings
    m_cachedMetrics.fontFamily = s->fontFamily();
    m_cachedMetrics.fontSize = s->fontSize();
    m_cachedMetrics.backgroundOpacity = s->backgroundOpacity();

    // Cursor trails animation timer
    m_wantsCursorTrails = s->cursorTrails();
    if (m_wantsCursorTrails)
        m_shaderAnimTimer.start(33, this); // ~30fps
    connect(s, &Settings::cursorTrailsChanged, this, [this]() {
        m_wantsCursorTrails = Settings::instance()->cursorTrails();
        if (m_wantsCursorTrails)
            m_shaderAnimTimer.start(33, this);
        else
            m_shaderAnimTimer.stop();
        update();
    });

    // Custom shader path
    m_customShaderPath = s->customShaderPath();
    connect(s, &Settings::customShaderPathChanged, this, [this]() {
        m_customShaderPath = Settings::instance()->customShaderPath();
        m_customShaderDirty = true;
        if (!m_customShaderPath.isEmpty() || m_wantsCursorTrails)
            m_shaderAnimTimer.start(33, this);
        update();
    });
}

void GLRenderer::setSource(QObject *source)
{
    if (m_source == source)
        return;

    // Disconnect previous source
    if (m_source) {
        disconnect(m_source, nullptr, this, nullptr);
    }

    m_source = source;

    // Connect TerminalView's contentChanged to trigger GL repaint
    TerminalView *tv = qobject_cast<TerminalView *>(m_source);
    if (tv) {
        connect(tv, &TerminalView::contentChanged, this, &QQuickItem::update);
    }

    Q_EMIT sourceChanged();
}

void GLRenderer::invalidateMetrics()
{
    // Snapshot settings for Renderer (consumed on render thread via generation counter)
    Settings *s = Settings::instance();
    m_cachedMetrics.fontFamily = s->fontFamily();
    m_cachedMetrics.fontSize = s->fontSize();
    m_cachedMetrics.backgroundOpacity = s->backgroundOpacity();

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

QQuickFramebufferObject::Renderer *GLRenderer::createRenderer() const
{
    return new Renderer;
}

// --- Renderer implementation ---

GLRenderer::Renderer::Renderer()
{
    // Initialize palette data to zeros to avoid uninitialized memory
    memset(m_postPaletteData, 0, sizeof(m_postPaletteData));
}

GLRenderer::Renderer::~Renderer()
{
    // QOpenGLShaderProgram handles context absence gracefully in its destructor
    // (skips GL cleanup, frees CPU memory). Always safe to delete.
    delete m_program;
    delete m_flatProgram;
    delete m_magProgram;
    delete m_postShader.program;
    m_postShader.program = nullptr;
    for (auto &shader : m_postShaders)
        delete shader.program;
    m_postShaders.clear();

    if (QOpenGLContext::currentContext()) {
        if (m_vbo.isCreated())
            m_vbo.destroy();
        if (m_flatVbo.isCreated())
            m_flatVbo.destroy();
        if (m_magnifierTex) {
            glDeleteTextures(1, &m_magnifierTex);
            m_magnifierTex = 0;
        }
        // Phase 5B: cleanup post-processing resources
        destroyPipelineFbo();
        destroyPingPongFbo();
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
    createMagShaders();
    createMagTexture();

    // Phase 5B: detect ES 3.0 and set up post-processing pipeline
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

void GLRenderer::Renderer::createShaders()
{
    m_program = new QOpenGLShaderProgram;
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource)) {
        qWarning() << "GLRenderer: vertex shader compilation failed:" << m_program->log();
        delete m_program;
        m_program = nullptr;
        return;
    }
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSource)) {
        qWarning() << "GLRenderer: fragment shader compilation failed:" << m_program->log();
        delete m_program;
        m_program = nullptr;
        return;
    }
    if (!m_program->link()) {
        qWarning() << "GLRenderer: shader linking failed:" << m_program->log();
        delete m_program;
        m_program = nullptr;
        return;
    }

    m_matrixUniform = m_program->uniformLocation("u_matrix");
    m_atlasUniform = m_program->uniformLocation("u_atlas");
    m_cursorPosUniform = m_program->uniformLocation("u_cursorPos");
    m_cellSizeUniform = m_program->uniformLocation("u_cellSize");
    m_cursorBlinkUniform = m_program->uniformLocation("u_cursorBlink");
    m_cursorStyleUniform = m_program->uniformLocation("u_cursorStyle");
    m_topPaddingUniform = m_program->uniformLocation("u_topPadding");
    m_positionAttr = m_program->attributeLocation("position");
    m_texcoordAttr = m_program->attributeLocation("texcoord");
    m_fgColorAttr = m_program->attributeLocation("fg_color");
    m_bgColorAttr = m_program->attributeLocation("bg_color");
    m_decoAttr = m_program->attributeLocation("deco_type");
}

void GLRenderer::Renderer::createVBO()
{
    // Create VBO (will be populated in render() when dirty)
    m_vbo = QOpenGLBuffer(QOpenGLBuffer::VertexBuffer);
    m_vbo.create();
    m_vbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
}

void GLRenderer::Renderer::createFlatShaders()
{
    m_flatProgram = new QOpenGLShaderProgram;
    if (!m_flatProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, flatVertexShaderSource)) {
        qWarning() << "GLRenderer: flat vertex shader compilation failed:" << m_flatProgram->log();
        delete m_flatProgram;
        m_flatProgram = nullptr;
        return;
    }
    if (!m_flatProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, flatFragmentShaderSource)) {
        qWarning() << "GLRenderer: flat fragment shader compilation failed:" << m_flatProgram->log();
        delete m_flatProgram;
        m_flatProgram = nullptr;
        return;
    }
    if (!m_flatProgram->link()) {
        qWarning() << "GLRenderer: flat shader linking failed:" << m_flatProgram->log();
        delete m_flatProgram;
        m_flatProgram = nullptr;
        return;
    }

    m_flatMatrixUniform = m_flatProgram->uniformLocation("u_matrix");
    m_flatPositionAttr = m_flatProgram->attributeLocation("position");
    m_flatColorAttr = m_flatProgram->attributeLocation("color");
}

void GLRenderer::Renderer::createFlatVBO()
{
    m_flatVbo = QOpenGLBuffer(QOpenGLBuffer::VertexBuffer);
    m_flatVbo.create();
    m_flatVbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
}

void GLRenderer::Renderer::createMagShaders()
{
    m_magProgram = new QOpenGLShaderProgram;
    if (!m_magProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, magVertexShaderSource)) {
        qWarning() << "GLRenderer: magnifier vertex shader compilation failed:" << m_magProgram->log();
        delete m_magProgram;
        m_magProgram = nullptr;
        return;
    }
    if (!m_magProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, magFragmentShaderSource)) {
        qWarning() << "GLRenderer: magnifier fragment shader compilation failed:" << m_magProgram->log();
        delete m_magProgram;
        m_magProgram = nullptr;
        return;
    }
    if (!m_magProgram->link()) {
        qWarning() << "GLRenderer: magnifier shader linking failed:" << m_magProgram->log();
        delete m_magProgram;
        m_magProgram = nullptr;
        return;
    }

    m_magMatrixUniform = m_magProgram->uniformLocation("u_matrix");
    m_magTexUniform = m_magProgram->uniformLocation("u_magnifierTex");
    m_magDestRectUniform = m_magProgram->uniformLocation("u_destRect");
    m_magCornerRadiusUniform = m_magProgram->uniformLocation("u_cornerRadius");
    m_magBorderColorUniform = m_magProgram->uniformLocation("u_borderColor");
    m_magBorderWidthUniform = m_magProgram->uniformLocation("u_borderWidth");
    m_magPositionAttr = m_magProgram->attributeLocation("position");
    m_magTexcoordAttr = m_magProgram->attributeLocation("texcoord");
}

void GLRenderer::Renderer::createMagTexture()
{
    // POT 128×64: srcW=90 (MagnifierWidth/Zoom=180/2), srcH=50 (MagnifierHeight/Zoom=100/2), rounded up
    glGenTextures(1, &m_magnifierTex);
    glBindTexture(GL_TEXTURE_2D, m_magnifierTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Allocate 128×64 RGBA texture (initial data is garbage, will be overwritten by glCopyTexSubImage2D)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GLRenderer::Renderer::appendCircle(float cx, float cy, float radius,
                                         float r, float g, float b, float a, int segments)
{
    const float step = 2.0f * static_cast<float>(M_PI) / segments;
    for (int i = 0; i < segments; ++i) {
        float angle0 = step * i;
        float angle1 = step * (i + 1);
        // Center vertex
        m_flatVertices << cx << cy << r << g << b << a;
        // Perimeter vertex i
        m_flatVertices << (cx + radius * cosf(angle0))
                       << (cy + radius * sinf(angle0))
                       << r << g << b << a;
        // Perimeter vertex i+1
        m_flatVertices << (cx + radius * cosf(angle1))
                       << (cy + radius * sinf(angle1))
                       << r << g << b << a;
    }
}

void GLRenderer::Renderer::buildMagnifierVertices(int fboW, int fboH)
{
    m_magVertices.clear();
    m_magVertexCount = 0;

    if (!m_magnifierVisible || !m_selecting || m_selStart == m_selEnd)
        return;

    QPointF fingerPos = m_magnifierFingerPos;

    // Source region: area around finger to zoom into
    int srcW = TerminalView::MagnifierWidth / TerminalView::MagnifierZoom;  // 90
    int srcH = TerminalView::MagnifierHeight / TerminalView::MagnifierZoom; // 50
    int srcX = static_cast<int>(fingerPos.x()) - srcW / 2;
    int srcY = static_cast<int>(fingerPos.y()) - srcH / 2;

    // Clamp source to FBO bounds
    if (fboW > srcW && fboH > srcH) {
        srcX = qBound(0, srcX, fboW - srcW);
        srcY = qBound(0, srcY, fboH - srcH);
    } else {
        srcX = qMax(0, srcX);
        srcY = qMax(0, srcY);
    }

    // Destination: above finger, centered horizontally
    int destX = static_cast<int>(fingerPos.x()) - TerminalView::MagnifierWidth / 2;
    int destY = static_cast<int>(fingerPos.y()) - TerminalView::MagnifierHeight - TerminalView::MagnifierOffset;

    // Flip below finger when near top edge
    if (destY < 0)
        destY = static_cast<int>(fingerPos.y()) + TerminalView::MagnifierOffset;
    destX = qBound(0, destX, fboW - TerminalView::MagnifierWidth);
    destY = qBound(0, destY, fboH - TerminalView::MagnifierHeight);

    float dx0 = static_cast<float>(destX);
    float dy0 = static_cast<float>(destY);
    float dx1 = static_cast<float>(destX + TerminalView::MagnifierWidth);
    float dy1 = static_cast<float>(destY + TerminalView::MagnifierHeight);

    // UV coords: source region mapped to 0-1 within the 128×64 texture
    // srcW=90, srcH=50 are copied to texture starting at (0,0)
    float texW = 128.0f;
    float texH = 64.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = static_cast<float>(srcW) / texW;  // 90/128
    float v1 = static_cast<float>(srcH) / texH;   // 50/64

    // 2 triangles (6 vertices), each: pos2 + texcoord2 = 4 floats
    // Triangle 1: top-left, top-right, bottom-right
    m_magVertices << dx0 << dy0 << u0 << v0;
    m_magVertices << dx1 << dy0 << u1 << v0;
    m_magVertices << dx1 << dy1 << u1 << v1;
    // Triangle 2: top-left, bottom-right, bottom-left
    m_magVertices << dx0 << dy0 << u0 << v0;
    m_magVertices << dx1 << dy1 << u1 << v1;
    m_magVertices << dx0 << dy1 << u0 << v1;

    m_magVertexCount = 6;
}

// --- Phase 5B: ES 3.0 detection ---

void GLRenderer::Renderer::detectES300()
{
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    if (!ctx)
        return;

    QSurfaceFormat fmt = ctx->format();
    qDebug() << "GLRenderer: GL context: major=" << fmt.majorVersion()
             << "minor=" << fmt.minorVersion()
             << "ES=" << ctx->isOpenGLES()
             << "GL_VERSION:" << (const char*)glGetString(GL_VERSION)
             << "GL_RENDERER:" << (const char*)glGetString(GL_RENDERER);

    // Don't trust QSurfaceFormat version — SailfishOS libhybris often reports ES 2.0
    // even when the driver supports ES 3.0+. Just try compiling and see.
    const char *testFragSrc =
        "#version 300 es\n"
        "precision mediump float;\n"
        "out vec4 _out;\n"
        "void main() { _out = vec4(1.0); }\n";

    QOpenGLShaderProgram testProg;
    bool ok = testProg.addShaderFromSourceCode(QOpenGLShader::Fragment, testFragSrc);
    if (!ok) {
        qDebug() << "GLRenderer: ES 3.0 probe failed — shader pipeline disabled";
        m_es300 = false;
        return;
    }

    m_es300 = true;
    qDebug() << "GLRenderer: ES 3.0 confirmed (probe shader compiled)";
    // Note: setShaderPipelineAvailable is a single bool write (atomic on ARM).
    // Technically a render-thread write but practically safe.
    Settings::instance()->setShaderPipelineAvailable(true);
}

// --- Phase 5B: Pipeline FBO (render-to-texture) ---

void GLRenderer::Renderer::createPipelineFbo(int w, int h)
{
    // Skip if already correct size
    if (m_pipelineFbo && m_pipelineTexW == w && m_pipelineTexH == h)
        return;

    // Destroy old if resizing
    destroyPipelineFbo();

    // Create color texture
    glGenTextures(1, &m_pipelineTex);
    glBindTexture(GL_TEXTURE_2D, m_pipelineTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Create FBO with color attachment only (no depth/stencil — not needed for 2D terminal rendering)
    glGenFramebuffers(1, &m_pipelineFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_pipelineFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_pipelineTex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        qWarning() << "GLRenderer: pipeline FBO incomplete, status=" << status;
        destroyPipelineFbo();
    } else {
        m_pipelineTexW = w;
        m_pipelineTexH = h;
        qDebug() << "GLRenderer: pipeline FBO created" << w << "x" << h;
    }

    // Unbind — caller will rebind as needed
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLRenderer::Renderer::destroyPipelineFbo()
{
    if (m_pipelineFbo) {
        glDeleteFramebuffers(1, &m_pipelineFbo);
        m_pipelineFbo = 0;
    }
    if (m_pipelineTex) {
        glDeleteTextures(1, &m_pipelineTex);
        m_pipelineTex = 0;
    }
    m_pipelineTexW = 0;
    m_pipelineTexH = 0;
}

void GLRenderer::Renderer::createPingPongFbo(int w, int h)
{
    // Skip if already correct size
    if (m_pingPongFbo && m_pingPongTexW == w && m_pingPongTexH == h)
        return;

    destroyPingPongFbo();

    glGenTextures(1, &m_pingPongTex);
    glBindTexture(GL_TEXTURE_2D, m_pingPongTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &m_pingPongFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_pingPongFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_pingPongTex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        qWarning() << "GLRenderer: ping-pong FBO incomplete, status=" << status;
        destroyPingPongFbo();
    } else {
        m_pingPongTexW = w;
        m_pingPongTexH = h;
        qDebug() << "GLRenderer: ping-pong FBO created" << w << "x" << h;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLRenderer::Renderer::destroyPingPongFbo()
{
    if (m_pingPongFbo) {
        glDeleteFramebuffers(1, &m_pingPongFbo);
        m_pingPongFbo = 0;
    }
    if (m_pingPongTex) {
        glDeleteTextures(1, &m_pingPongTex);
        m_pingPongTex = 0;
    }
    m_pingPongTexW = 0;
    m_pingPongTexH = 0;
}

void GLRenderer::Renderer::runPostProcessPass(PostShader &shader, GLuint inputTex, GLuint outputFbo, int w, int h)
{
    glBindFramebuffer(GL_FRAMEBUFFER, outputFbo);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Bind input texture as iChannel0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTex);

    // Bind shader and upload uniforms
    shader.program->bind();
    PostShader savedShader = m_postShader;
    m_postShader = shader; // temporarily alias for uploadPostShaderUniforms
    uploadPostShaderUniforms(w, h);
    m_postShader = savedShader; // restore

    // Draw full-screen triangle (no VBO — gl_VertexID trick)
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Cleanup
    shader.program->release();
    glBindTexture(GL_TEXTURE_2D, 0);
}

// --- Phase 5B: Post shader loading & compilation ---

void GLRenderer::Renderer::createPostShaders()
{
    // Compile full-screen triangle vertex shader
    m_postShader.program = new QOpenGLShaderProgram;
    if (!m_postShader.program->addShaderFromSourceCode(QOpenGLShader::Vertex, postVertexShaderSource)) {
        qWarning() << "GLRenderer: post vertex shader compilation failed:" << m_postShader.program->log();
        m_shaderError = m_postShader.program->log();
        delete m_postShader.program;
        m_postShader.program = nullptr;
        return;
    }

    // Try loading a custom post shader from Settings
    // For now, just verify the prefix compiles on its own (no actual effect shader yet)
    // loadPostShader() will be called when a shader path is configured
    qDebug() << "GLRenderer: post shader infrastructure ready (ES 3.0, vertex shader compiled)";
}

void GLRenderer::Renderer::loadPostShader(const QString &path)
{
    if (!m_es300) {
        qWarning() << "GLRenderer: cannot load post shader — ES 3.0 not available";
        return;
    }

    // Read shader source file
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "GLRenderer: cannot open post shader file:" << path;
        m_shaderError = QStringLiteral("Cannot open file: ") + path;
        m_postShaderActive = false;
        return;
    }
    QByteArray userShaderSrc = file.readAll();
    file.close();

    // Clean up previous post shader program if any
    if (m_postShader.program) {
        delete m_postShader.program;
        m_postShader.program = nullptr;
        m_postShaderActive = false;
    }

    // Build full program: vertex shader + prefix + user fragment shader
    m_postShader.program = new QOpenGLShaderProgram;

    // Vertex shader
    if (!m_postShader.program->addShaderFromSourceCode(QOpenGLShader::Vertex, postVertexShaderSource)) {
        qWarning() << "GLRenderer: post vertex shader failed:" << m_postShader.program->log();
        m_shaderError = m_postShader.program->log();
        delete m_postShader.program;
        m_postShader.program = nullptr;
        m_postShaderActive = false;
        return;
    }

    // Fragment: prefix + user code
    QByteArray fragSrc = QByteArray(shadertoyPrefixES300) + "\n" + userShaderSrc;
    if (!m_postShader.program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragSrc.constData())) {
        qWarning() << "GLRenderer: post fragment shader compilation failed:" << m_postShader.program->log();
        m_shaderError = m_postShader.program->log();
        delete m_postShader.program;
        m_postShader.program = nullptr;
        m_postShaderActive = false;
        return;
    }

    if (!m_postShader.program->link()) {
        qWarning() << "GLRenderer: post shader linking failed:" << m_postShader.program->log();
        m_shaderError = m_postShader.program->log();
        delete m_postShader.program;
        m_postShader.program = nullptr;
        m_postShaderActive = false;
        return;
    }

    // Cache all uniform locations
    m_postShader.loc.iResolution = m_postShader.program->uniformLocation("iResolution");
    m_postShader.loc.iTime = m_postShader.program->uniformLocation("iTime");
    m_postShader.loc.iTimeDelta = m_postShader.program->uniformLocation("iTimeDelta");
    m_postShader.loc.iFrameRate = m_postShader.program->uniformLocation("iFrameRate");
    m_postShader.loc.iFrame = m_postShader.program->uniformLocation("iFrame");
    m_postShader.loc.iChannelTime = m_postShader.program->uniformLocation("iChannelTime[0]");
    m_postShader.loc.iChannelResolution = m_postShader.program->uniformLocation("iChannelResolution[0]");
    m_postShader.loc.iMouse = m_postShader.program->uniformLocation("iMouse");
    m_postShader.loc.iDate = m_postShader.program->uniformLocation("iDate");
    m_postShader.loc.iSampleRate = m_postShader.program->uniformLocation("iSampleRate");
    m_postShader.loc.iCurrentCursor = m_postShader.program->uniformLocation("iCurrentCursor");
    m_postShader.loc.iPreviousCursor = m_postShader.program->uniformLocation("iPreviousCursor");
    m_postShader.loc.iCurrentCursorColor = m_postShader.program->uniformLocation("iCurrentCursorColor");
    m_postShader.loc.iPreviousCursorColor = m_postShader.program->uniformLocation("iPreviousCursorColor");
    m_postShader.loc.iCurrentCursorStyle = m_postShader.program->uniformLocation("iCurrentCursorStyle");
    m_postShader.loc.iPreviousCursorStyle = m_postShader.program->uniformLocation("iPreviousCursorStyle");
    m_postShader.loc.iCursorVisible = m_postShader.program->uniformLocation("iCursorVisible");
    m_postShader.loc.iTimeCursorChange = m_postShader.program->uniformLocation("iTimeCursorChange");
    m_postShader.loc.iTimeFocus = m_postShader.program->uniformLocation("iTimeFocus");
    m_postShader.loc.iFocus = m_postShader.program->uniformLocation("iFocus");
    m_postShader.loc.iPalette = m_postShader.program->uniformLocation("iPalette[0]");
    m_postShader.loc.iBackgroundColor = m_postShader.program->uniformLocation("iBackgroundColor");
    m_postShader.loc.iForegroundColor = m_postShader.program->uniformLocation("iForegroundColor");
    m_postShader.loc.iCursorColor = m_postShader.program->uniformLocation("iCursorColor");
    m_postShader.loc.iCursorText = m_postShader.program->uniformLocation("iCursorText");
    m_postShader.loc.iSelectionForegroundColor = m_postShader.program->uniformLocation("iSelectionForegroundColor");
    m_postShader.loc.iSelectionBackgroundColor = m_postShader.program->uniformLocation("iSelectionBackgroundColor");
    m_postShader.loc.iChannel0 = m_postShader.program->uniformLocation("iChannel0");

    m_postShaderActive = true;
    m_shaderError.clear();
    qDebug() << "GLRenderer: post shader loaded from" << path;
}

void GLRenderer::Renderer::uploadPostShaderUniforms(int fboW, int fboH)
{
    const PostShaderUniforms &loc = m_postShader.loc;

    if (loc.iResolution >= 0)
        m_postShader.program->setUniformValue(loc.iResolution,
            static_cast<float>(fboW), static_cast<float>(fboH), 1.0f);
    if (loc.iTime >= 0)
        m_postShader.program->setUniformValue(loc.iTime, m_postTime);
    if (loc.iTimeDelta >= 0)
        m_postShader.program->setUniformValue(loc.iTimeDelta, m_postTimeDelta);
    if (loc.iFrameRate >= 0)
        m_postShader.program->setUniformValue(loc.iFrameRate, m_postFrameRate);
    if (loc.iFrame >= 0)
        m_postShader.program->setUniformValue(loc.iFrame, m_postFrame);
    if (loc.iChannelTime >= 0) {
        float chTime[4] = {m_postTime, 0.0f, 0.0f, 0.0f};
        glUniform1fv(loc.iChannelTime, 4, chTime);
    }
    if (loc.iChannelResolution >= 0) {
        float chRes[12] = {
            static_cast<float>(fboW), static_cast<float>(fboH), 1.0f,
            0, 0, 0, 0, 0, 0, 0, 0, 0
        };
        glUniform3fv(loc.iChannelResolution, 4, chRes);
    }
    if (loc.iMouse >= 0)
        m_postShader.program->setUniformValue(loc.iMouse, 0.0f, 0.0f, 0.0f, 0.0f);
    if (loc.iDate >= 0) {
        QDateTime now = QDateTime::currentDateTime();
        float secs = now.time().hour() * 3600.0f + now.time().minute() * 60.0f
                     + now.time().second() + now.time().msec() / 1000.0f;
        m_postShader.program->setUniformValue(loc.iDate,
            static_cast<float>(now.date().year()),
            static_cast<float>(now.date().month()),
            static_cast<float>(now.date().day()),
            secs);
    }
    if (loc.iSampleRate >= 0)
        m_postShader.program->setUniformValue(loc.iSampleRate, 0.0f);

    // Cursor uniforms — Ghostty shader expects (x, y_top, w, h)
    // In our Y-up ortho, cy = row*cellHeight + topPadding is the bottom edge.
    // Add cellHeight to get the top edge (higher Y in Y-up = top of cell).
    if (loc.iCurrentCursor >= 0) {
        float cx = m_cursorX * m_cellWidth;
        float cy = m_cursorY * m_cellHeight + m_topPadding + static_cast<float>(m_cellHeight);
        m_postShader.program->setUniformValue(loc.iCurrentCursor,
            cx, cy,
            static_cast<float>(m_cellWidth),
            static_cast<float>(m_cellHeight));
    }
    if (loc.iPreviousCursor >= 0) {
        float px = m_prevCursorX * m_cellWidth;
        float py = m_prevCursorY * m_cellHeight + m_topPadding + static_cast<float>(m_cellHeight);
        m_postShader.program->setUniformValue(loc.iPreviousCursor,
            px, py,
            static_cast<float>(m_cellWidth),
            static_cast<float>(m_cellHeight));
    }
    if (loc.iCurrentCursorColor >= 0) {
        if (m_postCursorColorHasValue)
            m_postShader.program->setUniformValue(loc.iCurrentCursorColor,
                m_postCursorR, m_postCursorG, m_postCursorB, 1.0f);
        else
            m_postShader.program->setUniformValue(loc.iCurrentCursorColor,
                m_postFgR, m_postFgG, m_postFgB, 1.0f);
    }
    if (loc.iPreviousCursorColor >= 0)
        m_postShader.program->setUniformValue(loc.iPreviousCursorColor, 0.0f, 0.0f, 0.0f, 0.0f);
    if (loc.iCurrentCursorStyle >= 0)
        m_postShader.program->setUniformValue(loc.iCurrentCursorStyle, m_cursorStyle);
    if (loc.iPreviousCursorStyle >= 0)
        m_postShader.program->setUniformValue(loc.iPreviousCursorStyle, 0);
    if (loc.iCursorVisible >= 0)
        m_postShader.program->setUniformValue(loc.iCursorVisible, m_cursorVisible ? 1 : 0);
    if (loc.iTimeCursorChange >= 0)
        m_postShader.program->setUniformValue(loc.iTimeCursorChange, m_cursorChangeTime);
    if (loc.iTimeFocus >= 0)
        m_postShader.program->setUniformValue(loc.iTimeFocus, 0.0f);
    if (loc.iFocus >= 0)
        m_postShader.program->setUniformValue(loc.iFocus, 1);

    // Palette (256 colors, vec3 each = 768 floats)
    if (loc.iPalette >= 0)
        glUniform3fv(loc.iPalette, 256, m_postPaletteData);

    // Terminal colors
    if (loc.iBackgroundColor >= 0)
        m_postShader.program->setUniformValue(loc.iBackgroundColor,
            m_postBgR, m_postBgG, m_postBgB);
    if (loc.iForegroundColor >= 0)
        m_postShader.program->setUniformValue(loc.iForegroundColor,
            m_postFgR, m_postFgG, m_postFgB);
    if (loc.iCursorColor >= 0) {
        if (m_postCursorColorHasValue)
            m_postShader.program->setUniformValue(loc.iCursorColor,
                m_postCursorR, m_postCursorG, m_postCursorB);
        else
            m_postShader.program->setUniformValue(loc.iCursorColor,
                m_postFgR, m_postFgG, m_postFgB);
    }
    if (loc.iCursorText >= 0)
        m_postShader.program->setUniformValue(loc.iCursorText,
            m_postBgR, m_postBgG, m_postBgB);

    // Selection colors — use reasonable defaults (not exposed by render state API)
    if (loc.iSelectionForegroundColor >= 0)
        m_postShader.program->setUniformValue(loc.iSelectionForegroundColor, 0.0f, 0.0f, 0.0f);
    if (loc.iSelectionBackgroundColor >= 0)
        m_postShader.program->setUniformValue(loc.iSelectionBackgroundColor, 0.4f, 0.6f, 1.0f);

    // iChannel0 = pipeline texture on unit 0
    if (loc.iChannel0 >= 0)
        m_postShader.program->setUniformValue(loc.iChannel0, 0);
}

void GLRenderer::Renderer::buildOverlayVertices(int fboW, int fboH)
{
    m_flatVertices.clear();
    m_flatVertexCount = 0;

    // --- Selection highlights ---
    if (m_selecting && m_selStart != m_selEnd) {
        float a = m_selectionHighlightColor.alphaF();
        float r = m_selectionHighlightColor.redF() * a;
        float g = m_selectionHighlightColor.greenF() * a;
        float b = m_selectionHighlightColor.blueF() * a;

        int sr = qBound(0, static_cast<int>(m_selStart.y() - m_topPadding) / m_cellHeight, m_rows - 1);
        int sc = qBound(0, static_cast<int>(m_selStart.x()) / m_cellWidth, m_cols - 1);
        int er = qBound(0, static_cast<int>(m_selEnd.y() - m_topPadding) / m_cellHeight, m_rows - 1);
        int ec = qBound(0, static_cast<int>(m_selEnd.x()) / m_cellWidth, m_cols - 1);

        if (sr > er || (sr == er && sc > ec)) {
            qSwap(sr, er);
            qSwap(sc, ec);
        }

        for (int row = sr; row <= er; ++row) {
            float y = row * m_cellHeight + m_topPadding;
            float x0 = (row == sr) ? sc * m_cellWidth : 0;
            float x1 = (row == er) ? (ec + 1) * m_cellWidth : m_cols * m_cellWidth;
            float y1 = y + m_cellHeight;

            // 2 triangles = 6 vertices, each: pos(2) + color(4) = 6 floats
            // Triangle 1: top-left, top-right, bottom-right
            m_flatVertices << x0 << y  << r << g << b << a;
            m_flatVertices << x1 << y  << r << g << b << a;
            m_flatVertices << x1 << y1 << r << g << b << a;
            // Triangle 2: top-left, bottom-right, bottom-left
            m_flatVertices << x0 << y  << r << g << b << a;
            m_flatVertices << x1 << y1 << r << g << b << a;
            m_flatVertices << x0 << y1 << r << g << b << a;
        }
    }

    // --- Search highlights ---
    if (m_searchActive && !m_searchMatches.isEmpty()) {
        int scrollOffset = m_scrollOffset;
        int visibleStartRow = scrollOffset;
        int visibleEndRow = scrollOffset + m_rows;

        int startIdx = 0;
        for (int i = 0; i < m_searchMatches.size(); ++i) {
            if (m_searchMatches[i].row >= visibleStartRow) {
                startIdx = i;
                break;
            }
            if (m_searchMatches[i].row > visibleEndRow) {
                startIdx = m_searchMatches.size();
                break;
            }
        }

        for (int i = startIdx; i < m_searchMatches.size(); ++i) {
            const auto &match = m_searchMatches[i];
            if (match.row > visibleEndRow) break;

            int viewportRow = match.row - scrollOffset;
            if (viewportRow < 0 || viewportRow >= m_rows) continue;

            float y = viewportRow * m_cellHeight + m_topPadding;
            float x = match.cellCol * m_cellWidth;
            float w = match.cellWidth * m_cellWidth;

            if (x + w > m_cols * m_cellWidth)
                w = m_cols * m_cellWidth - x;

            float x1 = x + w;
            float y1 = y + m_cellHeight;

            QColor color = (i == m_currentMatchIndex) ? m_searchCurrentColor : m_searchHighlightColor;
            float a = color.alphaF();
            float cr = color.redF() * a;
            float cg = color.greenF() * a;
            float cb = color.blueF() * a;

            // 2 triangles = 6 vertices
            m_flatVertices << x  << y  << cr << cg << cb << a;
            m_flatVertices << x1 << y  << cr << cg << cb << a;
            m_flatVertices << x1 << y1 << cr << cg << cb << a;
            m_flatVertices << x  << y  << cr << cg << cb << a;
            m_flatVertices << x1 << y1 << cr << cg << cb << a;
            m_flatVertices << x  << y1 << cr << cg << cb << a;
        }
    }

    // --- Link underlines ---
    if (!m_linkSpans.isEmpty()) {
        // Link color: QColor(100, 180, 255, 200) — premultiplied
        float la = 200.0f / 255.0f;
        float lr = (100.0f / 255.0f) * la;
        float lg = (180.0f / 255.0f) * la;
        float lb = (255.0f / 255.0f) * la;

        for (const auto &span : m_linkSpans) {
            // span.row is viewport-relative (0..rows-1) from refreshLinks()
            if (span.row < 0 || span.row >= m_rows) continue;

            // 2px-tall rect at bottom of cell
            float y = span.row * m_cellHeight + m_topPadding + m_cellHeight - 2;
            float x0 = span.startCol * m_cellWidth;
            float x1 = (span.endCol + 1) * m_cellWidth;
            float y1 = y + 2;

            // 2 triangles = 6 vertices
            m_flatVertices << x0 << y  << lr << lg << lb << la;
            m_flatVertices << x1 << y  << lr << lg << lb << la;
            m_flatVertices << x1 << y1 << lr << lg << lb << la;
            m_flatVertices << x0 << y  << lr << lg << lb << la;
            m_flatVertices << x1 << y1 << lr << lg << lb << la;
            m_flatVertices << x0 << y1 << lr << lg << lb << la;
        }
    }

    // --- Shell exit overlay (full-screen semi-transparent rect) ---
    if (m_shellExited) {
        float a = m_shellExitOverlayColor.alphaF();
        float r = m_shellExitOverlayColor.redF() * a;
        float g = m_shellExitOverlayColor.greenF() * a;
        float b = m_shellExitOverlayColor.blueF() * a;

        float x0 = 0.0f, y0 = 0.0f;
        float x1 = static_cast<float>(fboW);
        float y1 = static_cast<float>(fboH);

        m_flatVertices << x0 << y0 << r << g << b << a;
        m_flatVertices << x1 << y0 << r << g << b << a;
        m_flatVertices << x1 << y1 << r << g << b << a;
        m_flatVertices << x0 << y0 << r << g << b << a;
        m_flatVertices << x1 << y1 << r << g << b << a;
        m_flatVertices << x0 << y1 << r << g << b << a;
    }

    // --- Selection handles (tessellated circles) ---
    if (m_handlesVisible && m_selecting && !m_magnifierVisible && m_selStart != m_selEnd) {
        int sr = qBound(0, static_cast<int>(m_selStart.y() - m_topPadding) / m_cellHeight, m_rows - 1);
        int sc = qBound(0, static_cast<int>(m_selStart.x()) / m_cellWidth, m_cols - 1);
        int er = qBound(0, static_cast<int>(m_selEnd.y() - m_topPadding) / m_cellHeight, m_rows - 1);
        int ec = qBound(0, static_cast<int>(m_selEnd.x()) / m_cellWidth, m_cols - 1);

        if (sr > er || (sr == er && sc > ec)) {
            qSwap(sr, er);
            qSwap(sc, ec);
        }

        // Border color (premultiplied)
        float ba = m_selectionHandleBorderColor.alphaF();
        float br = m_selectionHandleBorderColor.redF() * ba;
        float bg = m_selectionHandleBorderColor.greenF() * ba;
        float bb = m_selectionHandleBorderColor.blueF() * ba;

        // Fill color (premultiplied)
        float fa = m_selectionHandleColor.alphaF();
        float fr = m_selectionHandleColor.redF() * fa;
        float fg = m_selectionHandleColor.greenF() * fa;
        float fb = m_selectionHandleColor.blueF() * fa;

        // Start handle — border then fill
        float sx = sc * m_cellWidth;
        float sy = (sr + 1) * m_cellHeight + m_topPadding;
        appendCircle(sx, sy, TerminalView::HandleRadius + 2, br, bg, bb, ba);
        appendCircle(sx, sy, TerminalView::HandleRadius, fr, fg, fb, fa);

        // End handle — border then fill
        float ex = (ec + 1) * m_cellWidth;
        float ey = (er + 1) * m_cellHeight + m_topPadding;
        appendCircle(ex, ey, TerminalView::HandleRadius + 2, br, bg, bb, ba);
        appendCircle(ex, ey, TerminalView::HandleRadius, fr, fg, fb, fa);
    }

    // --- Magnifier arrow (drawn after magnifier quad via separate draw call) ---
    if (m_magnifierVisible && m_selecting && m_selStart != m_selEnd) {
        // Compute dest rect to find arrow position (same logic as buildMagnifierVertices)
        QPointF fingerPos = m_magnifierFingerPos;
        int destX = static_cast<int>(fingerPos.x()) - TerminalView::MagnifierWidth / 2;
        int unclampedDestY = static_cast<int>(fingerPos.y()) - TerminalView::MagnifierHeight - TerminalView::MagnifierOffset;
        int destY = unclampedDestY;
        if (destY < 0)
            destY = static_cast<int>(fingerPos.y()) + TerminalView::MagnifierOffset;
        destX = qBound(0, destX, fboW - TerminalView::MagnifierWidth);
        destY = qBound(0, destY, fboH - TerminalView::MagnifierHeight);

        float arrowCenterX = destX + TerminalView::MagnifierWidth / 2.0f;
        // Arrow at bottom edge of magnifier on screen, pointing DOWN toward finger.
        // Ortho is Y-up; Qt scene graph flips the FBO so low Y = top on screen.
        // Bottom of magnifier on screen = high FBO Y = destY + MagnifierHeight.
        float arrowBase = static_cast<float>(destY + TerminalView::MagnifierHeight);
        float arrowTip = arrowBase + 8.0f;

        // If magnifier was flipped below finger, arrow at top edge pointing UP
        if (unclampedDestY < 0) {
            arrowBase = static_cast<float>(destY);
            arrowTip = arrowBase - 8.0f;
        }

        float a = m_magnifierBorderColor.alphaF();
        float r = m_magnifierBorderColor.redF() * a;
        float g = m_magnifierBorderColor.greenF() * a;
        float b = m_magnifierBorderColor.blueF() * a;

        // 3 vertices for arrow triangle
        m_flatVertices << (arrowCenterX - 6.0f) << arrowBase << r << g << b << a;
        m_flatVertices << arrowCenterX << arrowTip << r << g << b << a;
        m_flatVertices << (arrowCenterX + 6.0f) << arrowBase << r << g << b << a;
    }

    // Upload to VBO
    m_flatVertexCount = m_flatVertices.size() / 6; // 6 floats per vertex
    if (m_flatVertexCount > 0) {
        m_flatVbo.bind();
        m_flatVbo.allocate(m_flatVertices.constData(),
                           m_flatVertices.size() * sizeof(float));
        m_flatVbo.release();
    }
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

    // Skip sync work when GL overlay is not visible
    if (!q->isVisible())
        return;

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
    }

    // Get grid dimensions
    uint16_t cols = 0, rows = 0;
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_COLS, &cols);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROWS, &rows);

    // Get default colors
    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    ghostty_render_state_colors_get(state, &colors);

    float bgR = colors.background.r / 255.0f;
    float bgG = colors.background.g / 255.0f;
    float bgB = colors.background.b / 255.0f;
    float fgR = colors.foreground.r / 255.0f;
    float fgG = colors.foreground.g / 255.0f;
    float fgB = colors.foreground.b / 255.0f;

    // Phase 5B: cache colors for post shader uniforms
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

    // Get cursor info
    bool cursorVisible = false, cursorInViewport = false;
    bool cursorBlinking = true;
    uint16_t cursorX = 0, cursorY = 0;
    GhosttyRenderStateCursorVisualStyle cursorStyle = GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK;
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &cursorVisible);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &cursorInViewport);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_BLINKING, &cursorBlinking);
    if (cursorVisible && cursorInViewport) {
        ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cursorX);
        ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cursorY);
        ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE, &cursorStyle);
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

        // Build font for atlas rasterization
        QString family = q->m_cachedMetrics.fontFamily;
        if (family.isEmpty())
            family = QStringLiteral("monospace");
        QFont font(family, q->m_cachedMetrics.fontSize);
        font.setStyleHint(QFont::Monospace);
        font.setFixedPitch(true);

        // Initialize atlas with font
        if (!m_atlasInitialized) {
            m_atlas.initialize();
            m_atlasInitialized = true;
        }
        m_atlas.setFont(font, m_cellWidth, m_cellHeight);
    }

    // Read topPadding every frame (can change without font change)
    m_topPadding = m_terminalView->topPadding();

    m_cols = cols;
    m_rows = rows;

    // Update cursor state every frame (blink changes don't set dirty flag).
    // This is cheap — no vertex rebuild, just updating uniform values.
    int newCursorX = static_cast<int>(cursorX);
    int newCursorY = static_cast<int>(cursorY);
    // Guard: m_cursorX starts at -1, skip first-frame spurious move
    if (m_cursorX >= 0 && (newCursorX != static_cast<int>(m_cursorX) || newCursorY != static_cast<int>(m_cursorY))) {
        m_prevCursorX = static_cast<int>(m_cursorX);
        m_prevCursorY = static_cast<int>(m_cursorY);
        m_cursorMoved = true;
    }
    m_cursorX = static_cast<float>(cursorX);
    m_cursorY = static_cast<float>(cursorY);
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

        // Compute finger position: if dragging handle 1, use selStart; else use selEnd
        if (m_draggingHandle == 1)
            m_magnifierFingerPos = m_selStart;
        else
            m_magnifierFingerPos = m_selEnd;

        // Link underlines — trigger scan if dirty, then extract viewport-relative spans
        if (m_terminalView->isLinkScanDirty())
            m_terminalView->refreshLinks();
        m_linkSpans.clear();
        const auto &links = m_terminalView->currentLinks();
        for (const auto &link : links) {
            LinkSpan span;
            span.row = link.startRow;
            span.startCol = link.startCol;
            span.endCol = link.endCol;
            m_linkSpans.append(span);
        }
    }

    // Cache scrollbar offset for use in render() — avoids calling
    // ghostty_terminal_get() from the render thread (data race with GUI thread).
    if (m_terminalView && m_terminalView->vt()) {
        GhosttyTerminalScrollbar scrollbar;
        memset(&scrollbar, 0, sizeof(scrollbar));
        ghostty_terminal_get(m_terminalView->vt()->terminal(),
                             GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar);
        m_scrollOffset = scrollbar.offset;
    }

    // Check if terminal content actually changed (PTY data, resize, etc.)
    // Cursor blink does NOT require a vertex rebuild — it's handled by shader uniforms.

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
    }

    GhosttyRenderStateDirty dirty = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty);
    if (dirty == GHOSTTY_RENDER_STATE_DIRTY_FALSE)
        return;

    m_gridDirty = true;

    // Build cell vertices
    m_cellVertices.clear();
    m_cellVertices.reserve(cols * rows * 6); // 6 vertices per cell

    // Background opacity for premultiplied alpha
    float bgAlpha = m_bgOpacity;

    // Iterate rows
    GhosttyRenderStateRowIterator iterator;
    ghostty_render_state_row_iterator_new(nullptr, &iterator);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &iterator);

    GhosttyRenderStateRowCells cells;
    ghostty_render_state_row_cells_new(nullptr, &cells);

    int y = m_topPadding;
    int rowIdx = 0;
    while (ghostty_render_state_row_iterator_next(iterator)) {
        ghostty_render_state_row_get(iterator,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &cells);

        int x = 0;
        int colIdx = 0;
        while (ghostty_render_state_row_cells_next(cells)) {
            // Cell background color
            GhosttyColorRgb cellBg;
            float cBgR = bgR, cBgG = bgG, cBgB = bgB;
            if (ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                    &cellBg) == GHOSTTY_SUCCESS) {
                cBgR = cellBg.r / 255.0f;
                cBgG = cellBg.g / 255.0f;
                cBgB = cellBg.b / 255.0f;
            }

            // Cell foreground color
            GhosttyColorRgb cellFg;
            float cFgR = fgR, cFgG = fgG, cFgB = fgB;
            if (ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
                    &cellFg) == GHOSTTY_SUCCESS) {
                cFgR = cellFg.r / 255.0f;
                cFgG = cellFg.g / 255.0f;
                cFgB = cellFg.b / 255.0f;
            }

            // Read style for decoration type
            GhosttyStyle cellStyle = GHOSTTY_INIT_SIZED(GhosttyStyle);
            ghostty_render_state_row_cells_get(
                cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
                &cellStyle);
            float deco = 0.0f;
            if (cellStyle.underline > 0) deco = 1.0f;
            else if (cellStyle.strikethrough) deco = 2.0f;

            // Premultiply alpha (fgA is always 1.0 for opaque text)
            float pFgR = cFgR, pFgG = cFgG, pFgB = cFgB, pFgA = 1.0f;
            float pBgR = cBgR * bgAlpha, pBgG = cBgG * bgAlpha, pBgB = cBgB * bgAlpha, pBgA = bgAlpha;

            // Cell text — get glyph UVs
            uint32_t graphemesLen = 0;
            ghostty_render_state_row_cells_get(
                cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
                &graphemesLen);

            float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
            if (graphemesLen > 0 && graphemesLen <= 128) {
                uint32_t buf[128];
                ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                    buf);

                // Look up glyph in atlas (first codepoint of grapheme cluster)
                const GlyphInfo &gi = m_atlas.glyph(buf[0], cellStyle.bold, cellStyle.italic);
                u0 = gi.u0;
                v0 = gi.v0;
                u1 = gi.u1;
                v1 = gi.v1;
            }

            // Build quad (2 triangles, 6 vertices)
            float x0 = static_cast<float>(x);
            float y0 = static_cast<float>(y);
            float x1 = static_cast<float>(x + m_cellWidth);
            float y1 = static_cast<float>(y + m_cellHeight);

            // Triangle 1: top-left, top-right, bottom-right
            m_cellVertices.append({x0, y0, u0, v0, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
            m_cellVertices.append({x1, y0, u1, v0, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
            m_cellVertices.append({x1, y1, u1, v1, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
            // Triangle 2: top-left, bottom-right, bottom-left
            m_cellVertices.append({x0, y0, u0, v0, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
            m_cellVertices.append({x1, y1, u1, v1, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
            m_cellVertices.append({x0, y1, u0, v1, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});

            x += m_cellWidth;
            colIdx++;
        }

        y += m_cellHeight;
        rowIdx++;
    }

    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(iterator);

    m_dirty = true;
}

void GLRenderer::Renderer::render()
{
    if (!m_initialized)
        initialize();

    if (!m_program || !m_program->isLinked()) {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    // Rebuild VBO if dirty
    if (m_dirty) {
        rebuildVBO();
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

    // --- Phase 5B: post-processing pipeline ---
    bool didPostProcess = false;
    bool gridWasDirty = m_gridDirty;
    m_gridDirty = false;
    if (m_postShaderActive && m_es300 && m_postShader.program) {
        // --- Step 1: Update timing uniforms ---
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

        // Update cursor change time when cursor has moved
        if (m_cursorMoved) {
            m_cursorChangeTime = m_postTime;
            m_cursorMoved = false;
            m_animationSettled = false;
        }

        // Check if cursor trail animation has settled
        if (!m_animationSettled && m_postTime - m_cursorChangeTime > 0.5f) {
            m_animationSettled = true;
        }

        // Skip pipeline when animation settled AND grid not changed AND no active overlays
        bool overlayActive = m_selecting || m_searchActive || m_magnifierVisible || m_shellExited;
        bool canSkipPipeline = m_animationSettled && !gridWasDirty && !overlayActive;

        // --- Step 2: Create/resize pipeline FBO ---
        int oldPipeW = m_pipelineTexW, oldPipeH = m_pipelineTexH;
        createPipelineFbo(fbo->width(), fbo->height());
        if (m_postShaders.size() > 1)
            createPingPongFbo(fbo->width(), fbo->height());
        if (m_pipelineTexW != oldPipeW || m_pipelineTexH != oldPipeH)
            canSkipPipeline = false; // FBO recreated — must draw
        if (m_pipelineFbo) {

            if (!canSkipPipeline) {
                // --- Step 3: Render terminal to pipeline FBO ---
                glBindFramebuffer(GL_FRAMEBUFFER, m_pipelineFbo);
                glViewport(0, 0, fbo->width(), fbo->height());
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);

                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

                QMatrix4x4 proj;
                proj.ortho(0, fbo->width(), 0, fbo->height(), -1, 1);

                // --- Cell grid ---
                glActiveTexture(GL_TEXTURE0);
                m_atlas.bind();

                m_program->bind();
                m_program->setUniformValue(m_matrixUniform, proj);
                m_program->setUniformValue(m_atlasUniform, 0);
                m_program->setUniformValue(m_cursorPosUniform, m_cursorX, m_cursorY);
                m_program->setUniformValue(m_cellSizeUniform,
                                           static_cast<float>(m_cellWidth),
                                           static_cast<float>(m_cellHeight));
                m_program->setUniformValue(m_cursorBlinkUniform, m_cursorVisible ? 1.0f : 0.0f);
                m_program->setUniformValue(m_cursorStyleUniform, static_cast<float>(m_cursorStyle));
                m_program->setUniformValue(m_topPaddingUniform, static_cast<float>(m_topPadding));

                m_vbo.bind();
                const int stride = 13 * sizeof(float);
                if (m_positionAttr >= 0) {
                    glEnableVertexAttribArray(m_positionAttr);
                    glVertexAttribPointer(m_positionAttr, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
                }
                if (m_texcoordAttr >= 0) {
                    glEnableVertexAttribArray(m_texcoordAttr);
                    glVertexAttribPointer(m_texcoordAttr, 2, GL_FLOAT, GL_FALSE, stride,
                                          reinterpret_cast<void*>(2 * sizeof(float)));
                }
                if (m_fgColorAttr >= 0) {
                    glEnableVertexAttribArray(m_fgColorAttr);
                    glVertexAttribPointer(m_fgColorAttr, 4, GL_FLOAT, GL_FALSE, stride,
                                          reinterpret_cast<void*>(4 * sizeof(float)));
                }
                if (m_bgColorAttr >= 0) {
                    glEnableVertexAttribArray(m_bgColorAttr);
                    glVertexAttribPointer(m_bgColorAttr, 4, GL_FLOAT, GL_FALSE, stride,
                                          reinterpret_cast<void*>(8 * sizeof(float)));
                }
                if (m_decoAttr >= 0) {
                    glEnableVertexAttribArray(m_decoAttr);
                    glVertexAttribPointer(m_decoAttr, 1, GL_FLOAT, GL_FALSE, stride,
                                          reinterpret_cast<void*>(12 * sizeof(float)));
                }
                glDrawArrays(GL_TRIANGLES, 0, m_vertexCount);

                if (m_positionAttr >= 0) glDisableVertexAttribArray(m_positionAttr);
                if (m_texcoordAttr >= 0) glDisableVertexAttribArray(m_texcoordAttr);
                if (m_fgColorAttr >= 0) glDisableVertexAttribArray(m_fgColorAttr);
                if (m_bgColorAttr >= 0) glDisableVertexAttribArray(m_bgColorAttr);
                if (m_decoAttr >= 0) glDisableVertexAttribArray(m_decoAttr);
                m_vbo.release();
                m_program->release();

                // --- Flat overlay (selection, search, links) ---
                buildOverlayVertices(fbo->width(), fbo->height());
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

                // --- Magnifier ---
                if (m_magnifierVisible && m_selecting && m_selStart != m_selEnd
                    && m_magProgram && m_magProgram->isLinked() && m_magnifierTex) {

                    int srcW = TerminalView::MagnifierWidth / TerminalView::MagnifierZoom;
                    int srcH = TerminalView::MagnifierHeight / TerminalView::MagnifierZoom;
                    int srcX = static_cast<int>(m_magnifierFingerPos.x()) - srcW / 2;
                    int srcY = static_cast<int>(m_magnifierFingerPos.y()) - srcH / 2;
                    if (fbo->width() > srcW && fbo->height() > srcH) {
                        srcX = qBound(0, srcX, fbo->width() - srcW);
                        srcY = qBound(0, srcY, fbo->height() - srcH);
                    } else {
                        srcX = qMax(0, srcX);
                        srcY = qMax(0, srcY);
                    }

                    glBindTexture(GL_TEXTURE_2D, m_magnifierTex);
                    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, srcX, srcY, srcW, srcH);
                    glBindTexture(GL_TEXTURE_2D, 0);

                    buildMagnifierVertices(fbo->width(), fbo->height());

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
                        destX = qBound(0, destX, fbo->width() - TerminalView::MagnifierWidth);
                        destY = qBound(0, destY, fbo->height() - TerminalView::MagnifierHeight);

                        m_magProgram->setUniformValue(m_magDestRectUniform,
                            static_cast<float>(destX), static_cast<float>(destY),
                            static_cast<float>(TerminalView::MagnifierWidth),
                            static_cast<float>(TerminalView::MagnifierHeight));
                        m_magProgram->setUniformValue(m_magCornerRadiusUniform, 8.0f);

                        float ba = m_magnifierBorderColor.alphaF();
                        float br = m_magnifierBorderColor.redF() * ba;
                        float bg = m_magnifierBorderColor.greenF() * ba;
                        float bb = m_magnifierBorderColor.blueF() * ba;
                        m_magProgram->setUniformValue(m_magBorderColorUniform, br, bg, bb, ba);
                        m_magProgram->setUniformValue(m_magBorderWidthUniform, 2.0f);

                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, m_magnifierTex);

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

                glDisable(GL_BLEND);

            } // end canSkipPipeline

            // --- Step 4-7: Post-process to Qt's default FBO ---
            if (m_postShaders.size() > 1 && m_pingPongFbo) {
                // Multi-pass ping-pong
                GLuint textures[2] = { m_pipelineTex, m_pingPongTex };
                GLuint fbos[2] = { m_pipelineFbo, m_pingPongFbo };
                int readIdx = 0; // start reading from pipeline

                for (int i = 0; i < m_postShaders.size(); i++) {
                    bool lastPass = (i == m_postShaders.size() - 1);
                    int writeIdx = 1 - readIdx;
                    GLuint inTex = textures[readIdx];
                    GLuint outFbo = lastPass ? fbo->handle() : fbos[writeIdx];

                    runPostProcessPass(m_postShaders[i], inTex, outFbo, fbo->width(), fbo->height());
                    readIdx = writeIdx; // next pass reads what we just wrote
                }
                didPostProcess = true;
            } else if (m_postShaderActive && m_postShader.program) {
                // Single-pass (existing behavior)
                runPostProcessPass(m_postShader, m_pipelineTex, fbo->handle(), fbo->width(), fbo->height());
                didPostProcess = true;
            }
        } // end if (m_pipelineFbo)
    } // end post-processing check

    if (!didPostProcess) {
        // --- Existing direct-to-FBO path (no post-processing) ---
        glViewport(0, 0, fbo->width(), fbo->height());
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        QMatrix4x4 m;
        m.ortho(0, fbo->width(), 0, fbo->height(), -1, 1);

        // Bind atlas texture to unit 0
        glActiveTexture(GL_TEXTURE0);
        m_atlas.bind();

        // Bind shader and set uniforms
        m_program->bind();
        m_program->setUniformValue(m_matrixUniform, m);
        m_program->setUniformValue(m_atlasUniform, 0); // texture unit 0

        // Cursor uniforms — updated every frame, no vertex rebuild needed for blink
        m_program->setUniformValue(m_cursorPosUniform, m_cursorX, m_cursorY);
        m_program->setUniformValue(m_cellSizeUniform,
                                   static_cast<float>(m_cellWidth),
                                   static_cast<float>(m_cellHeight));
        m_program->setUniformValue(m_cursorBlinkUniform, m_cursorVisible ? 1.0f : 0.0f);
        m_program->setUniformValue(m_cursorStyleUniform, static_cast<float>(m_cursorStyle));
        m_program->setUniformValue(m_topPaddingUniform, static_cast<float>(m_topPadding));

        // Bind VBO and set up vertex attributes (no VAO — ES 2.0 safe)
        m_vbo.bind();
        const int stride = 13 * sizeof(float);
        if (m_positionAttr >= 0) {
            glEnableVertexAttribArray(m_positionAttr);
            glVertexAttribPointer(m_positionAttr, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
        }
        if (m_texcoordAttr >= 0) {
            glEnableVertexAttribArray(m_texcoordAttr);
            glVertexAttribPointer(m_texcoordAttr, 2, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(2 * sizeof(float)));
        }
        if (m_fgColorAttr >= 0) {
            glEnableVertexAttribArray(m_fgColorAttr);
            glVertexAttribPointer(m_fgColorAttr, 4, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(4 * sizeof(float)));
        }
        if (m_bgColorAttr >= 0) {
            glEnableVertexAttribArray(m_bgColorAttr);
            glVertexAttribPointer(m_bgColorAttr, 4, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(8 * sizeof(float)));
        }
        if (m_decoAttr >= 0) {
            glEnableVertexAttribArray(m_decoAttr);
            glVertexAttribPointer(m_decoAttr, 1, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(12 * sizeof(float)));
        }

        glDrawArrays(GL_TRIANGLES, 0, m_vertexCount);

        // Cleanup attribute state
        if (m_positionAttr >= 0) glDisableVertexAttribArray(m_positionAttr);
        if (m_texcoordAttr >= 0) glDisableVertexAttribArray(m_texcoordAttr);
        if (m_fgColorAttr >= 0) glDisableVertexAttribArray(m_fgColorAttr);
        if (m_bgColorAttr >= 0) glDisableVertexAttribArray(m_bgColorAttr);
        if (m_decoAttr >= 0) glDisableVertexAttribArray(m_decoAttr);
        m_vbo.release();

        // Cleanup cell shader
        m_program->release();

        // --- GL overlay draw (flat-color shader for selection, search, links) ---
        buildOverlayVertices(fbo->width(), fbo->height());
        if (m_flatVertexCount > 0 && m_flatProgram && m_flatProgram->isLinked()) {
            m_flatProgram->bind();
            m_flatProgram->setUniformValue(m_flatMatrixUniform, m);
            m_flatVbo.bind();
            const int flatStride = 6 * sizeof(float); // pos2 + color4
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

        // --- GL magnifier (pure GL, no QPainter) ---
        if (m_magnifierVisible && m_selecting && m_selStart != m_selEnd
            && m_magProgram && m_magProgram->isLinked() && m_magnifierTex) {

            // Copy source region from FBO to magnifier texture
            int srcW = TerminalView::MagnifierWidth / TerminalView::MagnifierZoom;  // 90
            int srcH = TerminalView::MagnifierHeight / TerminalView::MagnifierZoom; // 50
            int srcX = static_cast<int>(m_magnifierFingerPos.x()) - srcW / 2;
            int srcY = static_cast<int>(m_magnifierFingerPos.y()) - srcH / 2;
            if (fbo->width() > srcW && fbo->height() > srcH) {
                srcX = qBound(0, srcX, fbo->width() - srcW);
                srcY = qBound(0, srcY, fbo->height() - srcH);
            } else {
                srcX = qMax(0, srcX);
                srcY = qMax(0, srcY);
            }

            glBindTexture(GL_TEXTURE_2D, m_magnifierTex);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, srcX, srcY, srcW, srcH);
            glBindTexture(GL_TEXTURE_2D, 0);

            // Build magnifier quad vertices
            buildMagnifierVertices(fbo->width(), fbo->height());

            if (m_magVertexCount > 0) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

                m_magProgram->bind();
                m_magProgram->setUniformValue(m_magMatrixUniform, m);
                m_magProgram->setUniformValue(m_magTexUniform, 0); // texture unit 0

                // Compute dest rect for SDF uniforms
                int destX = static_cast<int>(m_magnifierFingerPos.x()) - TerminalView::MagnifierWidth / 2;
                int destY = static_cast<int>(m_magnifierFingerPos.y()) - TerminalView::MagnifierHeight - TerminalView::MagnifierOffset;
                if (destY < 0)
                    destY = static_cast<int>(m_magnifierFingerPos.y()) + TerminalView::MagnifierOffset;
                destX = qBound(0, destX, fbo->width() - TerminalView::MagnifierWidth);
                destY = qBound(0, destY, fbo->height() - TerminalView::MagnifierHeight);

                m_magProgram->setUniformValue(m_magDestRectUniform,
                    static_cast<float>(destX), static_cast<float>(destY),
                    static_cast<float>(TerminalView::MagnifierWidth),
                    static_cast<float>(TerminalView::MagnifierHeight));
                m_magProgram->setUniformValue(m_magCornerRadiusUniform, 8.0f);

                float ba = m_magnifierBorderColor.alphaF();
                float br = m_magnifierBorderColor.redF() * ba;
                float bg = m_magnifierBorderColor.greenF() * ba;
                float bb = m_magnifierBorderColor.blueF() * ba;
                m_magProgram->setUniformValue(m_magBorderColorUniform, br, bg, bb, ba);
                m_magProgram->setUniformValue(m_magBorderWidthUniform, 2.0f);

                // Bind magnifier texture
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m_magnifierTex);

                // Upload and draw magnifier quad (dynamic, no persistent VBO)
                const int magStride = 4 * sizeof(float); // pos2 + texcoord2
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

        glDisable(GL_BLEND);
    }

    // --- QPainter overlay pass (gated: only for shell exit text) ---
    // Always draws on the Qt FBO (after post-processing if active)
    if (m_shellExited) {
        QOpenGLPaintDevice device(fbo->size());
        device.setPaintFlipped(true);
        QPainter painter(&device);
        painter.setRenderHint(QPainter::Antialiasing);

        // Shell exit overlay text (rect is drawn by flat shader in buildOverlayVertices)
        painter.setPen(m_shellExitTextColor);
        QFont font = painter.font();
        font.setPointSize(m_cachedFontSize + 4);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(QRectF(0, 0, fbo->width(), fbo->height()),
                         Qt::AlignCenter,
                         QString("Shell exited with code %1").arg(m_shellExitCode));

        painter.end();
    }
}
