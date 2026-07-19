#include "glrenderer.h"

#include "settings.h"

#include <QDebug>
#include <QMetaObject>
#include <QMatrix4x4>
#include <QOpenGLContext>

void GLRenderer::Renderer::blitPipelineToFbo(QOpenGLFramebufferObject *fbo)
{
    if (!m_blitProgram || !m_blitProgram->isLinked() || !m_pipelineTex)
        return;

    int w = fbo->width();
    int h = fbo->height();
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);

    QMatrix4x4 proj;
    proj.ortho(0, w, 0, h, -1, 1);

    // Full-screen quad: 2 triangles, 6 vertices, each pos2+texcoord2 = 4 floats
    float blitVerts[] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        fw, 0.0f, 1.0f, 0.0f,
        fw, fh, 1.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        fw, fh, 1.0f, 1.0f,
        0.0f, fh, 0.0f, 1.0f,
    };

    glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle());
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_pipelineTex);

    m_blitProgram->bind();
    m_blitProgram->setUniformValue(m_blitMatrixUniform, proj);
    m_blitProgram->setUniformValue(m_blitTexUniform, 0);

    const int stride = 4 * sizeof(float);
    glVertexAttribPointer(m_blitPositionAttr, 2, GL_FLOAT, GL_FALSE, stride, blitVerts);
    glVertexAttribPointer(m_blitTexcoordAttr, 2, GL_FLOAT, GL_FALSE, stride, blitVerts + 2);
    glEnableVertexAttribArray(m_blitPositionAttr);
    glEnableVertexAttribArray(m_blitTexcoordAttr);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(m_blitPositionAttr);
    glDisableVertexAttribArray(m_blitTexcoordAttr);

    glBindTexture(GL_TEXTURE_2D, 0);
    m_blitProgram->release();
}


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
    // Marshal to the GUI thread — Settings is main-thread-only (settings.h:11-12)
    QMetaObject::invokeMethod(Settings::instance(),
        "setShaderPipelineAvailable", Qt::QueuedConnection, Q_ARG(bool, true));
}

void GLRenderer::Renderer::createPipelineFbo(int w, int h)
{
    if (m_pipelineFbo && m_pipelineTexW == w && m_pipelineTexH == h)
        return;

    destroyPipelineFbo();

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

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTex);

    shader.program->bind();
    uploadPostShaderUniforms(shader, w, h);

    // No VBO: post vertex shader synthesises a fullscreen triangle from
    // gl_VertexID (see glrenderer.cpp:191). 3 verts cover [-1,-1]..[3,3].
    glDrawArrays(GL_TRIANGLES, 0, 3);

    shader.program->release();
    glBindTexture(GL_TEXTURE_2D, 0);
}
