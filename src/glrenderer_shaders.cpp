#include "glrenderer.h"

#include <QDateTime>
#include <QDebug>
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
    "uniform float u_cursorWidth;\n"
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
    // Wide-char cursor: u_cursorPos.x is the head column, u_cursorWidth is
    // 1.0 (narrow) or 2.0 (wide). The fragment is inside the cursor when
    // cellCoord.x spans [u_cursorPos.x, u_cursorPos.x + u_cursorWidth).
    "        if (cellCoord.y == u_cursorPos.y &&\n"
    "            cellCoord.x >= u_cursorPos.x &&\n"
    "            cellCoord.x < u_cursorPos.x + u_cursorWidth) {\n"
    // cx/cy are measured from the cursor's top-left (not the cell's), so
    // BAR/HOLLOW render at the cursor's edges across both wide cells.
    "            float cursorW = u_cursorWidth * u_cellSize.x;\n"
    "            float cx = adj_cell.x - u_cursorPos.x * u_cellSize.x;\n"
    "            float cy = adj_cell.y - cellCoord.y * u_cellSize.y;\n"
    "            if (u_cursorStyle < 1.5) {\n"
    "                fg = v_bg_color;\n"
    "                bg = v_fg_color;\n"
    "            } else if (u_cursorStyle < 2.5) {\n"
    "                if (cx < 2.0) {\n"
    "                    gl_FragColor = v_fg_color;\n"
    "                    return;\n"
    "                }\n"
    "            } else if (u_cursorStyle < 3.5) {\n"
    "                if (cy > u_cellSize.y - 2.0) {\n"
    "                    gl_FragColor = v_fg_color;\n"
    "                    return;\n"
    "                }\n"
    "            } else {\n"
    "                if (cx < 1.0 || cx > cursorW - 1.0 ||\n"
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
    "uniform sampler2D u_sceneTex;\n"
    "uniform vec4 u_destRect;\n"
    "uniform vec4 u_srcRect;\n"
    "uniform vec2 u_srcTexSize;\n"
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
    "    vec2 srcUV = (u_srcRect.xy + v_texcoord * u_srcRect.zw) / u_srcTexSize;\n"
    "    vec4 texColor = texture2D(u_sceneTex, srcUV);\n"
    "    float edgeAlpha = smoothstep(0.0, 1.5, -dist);\n"
    "    float borderMask = smoothstep(u_borderWidth, u_borderWidth - 1.5, -dist);\n"
    "    vec4 color = mix(texColor, u_borderColor, borderMask);\n"
    "    gl_FragColor = vec4(color.rgb * edgeAlpha, color.a * edgeAlpha);\n"
    "}\n";

// GLSL ES 2.0 blit shaders — pipeline FBO -> Qt FBO copy
static const char *blitVertexShaderSource =
    "attribute vec2 position;\n"
    "attribute vec2 texcoord;\n"
    "uniform mat4 u_matrix;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = u_matrix * vec4(position, 0.0, 1.0);\n"
    "    v_texcoord = texcoord;\n"
    "}\n";

static const char *blitFragmentShaderSource =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_texture, v_texcoord);\n"
    "}\n";

// ES 3.0 post-processing shaders

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
    m_cursorWidthUniform = m_program->uniformLocation("u_cursorWidth");
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
    m_magTexUniform = m_magProgram->uniformLocation("u_sceneTex");
    m_magDestRectUniform = m_magProgram->uniformLocation("u_destRect");
    m_magSrcRectUniform = m_magProgram->uniformLocation("u_srcRect");
    m_magSrcTexSizeUniform = m_magProgram->uniformLocation("u_srcTexSize");
    m_magCornerRadiusUniform = m_magProgram->uniformLocation("u_cornerRadius");
    m_magBorderColorUniform = m_magProgram->uniformLocation("u_borderColor");
    m_magBorderWidthUniform = m_magProgram->uniformLocation("u_borderWidth");
    m_magPositionAttr = m_magProgram->attributeLocation("position");
    m_magTexcoordAttr = m_magProgram->attributeLocation("texcoord");
}

void GLRenderer::Renderer::createBlitShader()
{
    m_blitProgram = new QOpenGLShaderProgram;
    if (!m_blitProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, blitVertexShaderSource)) {
        qWarning() << "GLRenderer: blit vertex shader compilation failed:" << m_blitProgram->log();
        delete m_blitProgram;
        m_blitProgram = nullptr;
        return;
    }
    if (!m_blitProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, blitFragmentShaderSource)) {
        qWarning() << "GLRenderer: blit fragment shader compilation failed:" << m_blitProgram->log();
        delete m_blitProgram;
        m_blitProgram = nullptr;
        return;
    }
    if (!m_blitProgram->link()) {
        qWarning() << "GLRenderer: blit shader linking failed:" << m_blitProgram->log();
        delete m_blitProgram;
        m_blitProgram = nullptr;
        return;
    }

    m_blitMatrixUniform = m_blitProgram->uniformLocation("u_matrix");
    m_blitTexUniform = m_blitProgram->uniformLocation("u_texture");
    m_blitPositionAttr = m_blitProgram->attributeLocation("position");
    m_blitTexcoordAttr = m_blitProgram->attributeLocation("texcoord");
}

// Post shader loading & compilation

void GLRenderer::Renderer::createPostShaders()
{
    m_postShader.program = new QOpenGLShaderProgram;
    if (!m_postShader.program->addShaderFromSourceCode(QOpenGLShader::Vertex, postVertexShaderSource)) {
        qWarning() << "GLRenderer: post vertex shader compilation failed:" << m_postShader.program->log();
        delete m_postShader.program;
        m_postShader.program = nullptr;
        return;
    }

    // Fragment shader is loaded on demand via loadPostShader() (user-supplied file).
    qDebug() << "GLRenderer: post shader infrastructure ready (ES 3.0, vertex shader compiled)";
}

void GLRenderer::Renderer::loadPostShader(const QString &path)
{
    if (!m_es300) {
        qWarning() << "GLRenderer: cannot load post shader — ES 3.0 not available";
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "GLRenderer: cannot open post shader file:" << path;
        m_postShaderActive = false;
        return;
    }
    QByteArray userShaderSrc = file.readAll();
    file.close();

    if (m_postShader.program) {
        delete m_postShader.program;
        m_postShader.program = nullptr;
        m_postShaderActive = false;
    }

    m_postShader.program = new QOpenGLShaderProgram;

    if (!m_postShader.program->addShaderFromSourceCode(QOpenGLShader::Vertex, postVertexShaderSource)) {
        qWarning() << "GLRenderer: post vertex shader failed:" << m_postShader.program->log();
        delete m_postShader.program;
        m_postShader.program = nullptr;
        m_postShaderActive = false;
        return;
    }

    QByteArray fragSrc = QByteArray(shadertoyPrefixES300) + "\n" + userShaderSrc;
    if (!m_postShader.program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragSrc.constData())) {
        qWarning() << "GLRenderer: post fragment shader compilation failed:" << m_postShader.program->log();
        delete m_postShader.program;
        m_postShader.program = nullptr;
        m_postShaderActive = false;
        return;
    }

    if (!m_postShader.program->link()) {
        qWarning() << "GLRenderer: post shader linking failed:" << m_postShader.program->log();
        delete m_postShader.program;
        m_postShader.program = nullptr;
        m_postShaderActive = false;
        return;
    }

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
    qDebug() << "GLRenderer: post shader loaded from" << path;
}

void GLRenderer::Renderer::uploadPostShaderUniforms(PostShader &shader, int fboW, int fboH)
{
    const PostShaderUniforms &loc = shader.loc;

    if (loc.iResolution >= 0)
        shader.program->setUniformValue(loc.iResolution,
            static_cast<float>(fboW), static_cast<float>(fboH), 1.0f);
    if (loc.iTime >= 0)
        shader.program->setUniformValue(loc.iTime, m_postTime);
    if (loc.iTimeDelta >= 0)
        shader.program->setUniformValue(loc.iTimeDelta, m_postTimeDelta);
    if (loc.iFrameRate >= 0)
        shader.program->setUniformValue(loc.iFrameRate, m_postFrameRate);
    if (loc.iFrame >= 0)
        shader.program->setUniformValue(loc.iFrame, static_cast<int>(m_postFrame));
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
        shader.program->setUniformValue(loc.iMouse, 0.0f, 0.0f, 0.0f, 0.0f);
    if (loc.iDate >= 0) {
        QDateTime now = QDateTime::currentDateTime();
        const QDate d = now.date();
        const QTime t = now.time();
        float secs = t.hour() * 3600.0f + t.minute() * 60.0f
                     + t.second() + t.msec() / 1000.0f;
        shader.program->setUniformValue(loc.iDate,
            static_cast<float>(d.year()),
            static_cast<float>(d.month()),
            static_cast<float>(d.day()),
            secs);
    }
    if (loc.iSampleRate >= 0)
        shader.program->setUniformValue(loc.iSampleRate, 0.0f);

    // Cursor uniforms — Ghostty shader expects (x, y_top, w, h)
    // In our Y-up ortho, cy = row*cellHeight + topPadding is the bottom edge.
    // Add cellHeight to get the top edge (higher Y in Y-up = top of cell).
    // Width is m_cursorWidth cells (1 narrow / 2 wide) so the trail rect
    // matches the wide-char cursor block.
    if (loc.iCurrentCursor >= 0) {
        float cx = m_cursorX * m_cellWidth;
        float cy = m_cursorY * m_cellHeight + m_topPadding + static_cast<float>(m_cellHeight);
        shader.program->setUniformValue(loc.iCurrentCursor,
            cx, cy,
            static_cast<float>(m_cursorWidth * m_cellWidth),
            static_cast<float>(m_cellHeight));
    }
    if (loc.iPreviousCursor >= 0) {
        float px = m_prevCursorX * m_cellWidth;
        float py = m_prevCursorY * m_cellHeight + m_topPadding + static_cast<float>(m_cellHeight);
        shader.program->setUniformValue(loc.iPreviousCursor,
            px, py,
            static_cast<float>(m_cellWidth),
            static_cast<float>(m_cellHeight));
    }
    // Cursor color: use explicit color if available, else fall back to foreground
    const float cr = m_postCursorColorHasValue ? m_postCursorR : m_postFgR;
    const float cg = m_postCursorColorHasValue ? m_postCursorG : m_postFgG;
    const float cb = m_postCursorColorHasValue ? m_postCursorB : m_postFgB;
    if (loc.iCurrentCursorColor >= 0)
        shader.program->setUniformValue(loc.iCurrentCursorColor, cr, cg, cb, 1.0f);
    if (loc.iPreviousCursorColor >= 0)
        shader.program->setUniformValue(loc.iPreviousCursorColor, 0.0f, 0.0f, 0.0f, 0.0f);
    if (loc.iCurrentCursorStyle >= 0)
        shader.program->setUniformValue(loc.iCurrentCursorStyle, m_cursorStyle);
    if (loc.iPreviousCursorStyle >= 0)
        shader.program->setUniformValue(loc.iPreviousCursorStyle, 0);
    if (loc.iCursorVisible >= 0)
        shader.program->setUniformValue(loc.iCursorVisible, m_cursorVisible ? 1 : 0);
    if (loc.iTimeCursorChange >= 0)
        shader.program->setUniformValue(loc.iTimeCursorChange, m_cursorChangeTime);
    if (loc.iTimeFocus >= 0)
        shader.program->setUniformValue(loc.iTimeFocus, 0.0f);
    if (loc.iFocus >= 0)
        shader.program->setUniformValue(loc.iFocus, 1);

    if (loc.iPalette >= 0)
        glUniform3fv(loc.iPalette, 256, m_postPaletteData);

    if (loc.iBackgroundColor >= 0)
        shader.program->setUniformValue(loc.iBackgroundColor,
            m_postBgR, m_postBgG, m_postBgB);
    if (loc.iForegroundColor >= 0)
        shader.program->setUniformValue(loc.iForegroundColor,
            m_postFgR, m_postFgG, m_postFgB);
    if (loc.iCursorColor >= 0)
        shader.program->setUniformValue(loc.iCursorColor, cr, cg, cb);
    if (loc.iCursorText >= 0)
        shader.program->setUniformValue(loc.iCursorText,
            m_postBgR, m_postBgG, m_postBgB);

    // Selection colors — use reasonable defaults (not exposed by render state API)
    if (loc.iSelectionForegroundColor >= 0)
        shader.program->setUniformValue(loc.iSelectionForegroundColor, 0.0f, 0.0f, 0.0f);
    if (loc.iSelectionBackgroundColor >= 0)
        shader.program->setUniformValue(loc.iSelectionBackgroundColor, 0.4f, 0.6f, 1.0f);

    // iChannel0 = pipeline texture on unit 0
    if (loc.iChannel0 >= 0)
        shader.program->setUniformValue(loc.iChannel0, 0);
}
