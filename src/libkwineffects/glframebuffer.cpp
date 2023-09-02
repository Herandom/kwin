/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006-2007 Rivo Laks <rivolaks@hot.ee>
    SPDX-FileCopyrightText: 2010, 2011 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2023 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "glframebuffer.h"
#include "kwinglplatform.h"
#include "kwingltexture.h"
#include "kwinglutils.h"
#include "logging_p.h"
#include "rendertarget.h"
#include "renderviewport.h"

namespace KWin
{

bool GLFramebuffer::sSupported = false;
bool GLFramebuffer::sSupportsPackedDepthStencil = false;
bool GLFramebuffer::sSupportsDepth24 = false;
bool GLFramebuffer::s_blitSupported = false;
QStack<GLFramebuffer *> GLFramebuffer::s_fbos = QStack<GLFramebuffer *>();

void GLFramebuffer::initStatic()
{
    if (GLPlatform::instance()->isGLES()) {
        sSupported = true;
        sSupportsPackedDepthStencil = hasGLVersion(3, 0) || hasGLExtension(QByteArrayLiteral("GL_OES_packed_depth_stencil"));
        sSupportsDepth24 = hasGLVersion(3, 0) || hasGLExtension(QByteArrayLiteral("GL_OES_depth24"));
        s_blitSupported = hasGLVersion(3, 0);
    } else {
        sSupported = hasGLVersion(3, 0) || hasGLExtension(QByteArrayLiteral("GL_ARB_framebuffer_object")) || hasGLExtension(QByteArrayLiteral("GL_EXT_framebuffer_object"));
        sSupportsPackedDepthStencil = hasGLVersion(3, 0) || hasGLExtension(QByteArrayLiteral("GL_ARB_framebuffer_object")) || hasGLExtension(QByteArrayLiteral("GL_EXT_packed_depth_stencil"));
        s_blitSupported = hasGLVersion(3, 0) || hasGLExtension(QByteArrayLiteral("GL_ARB_framebuffer_object")) || hasGLExtension(QByteArrayLiteral("GL_EXT_framebuffer_blit"));
    }
}

void GLFramebuffer::cleanup()
{
    Q_ASSERT(s_fbos.isEmpty());
    sSupported = false;
    s_blitSupported = false;
}

bool GLFramebuffer::blitSupported()
{
    return s_blitSupported;
}

GLFramebuffer *GLFramebuffer::currentFramebuffer()
{
    return s_fbos.isEmpty() ? nullptr : s_fbos.top();
}

void GLFramebuffer::pushFramebuffer(GLFramebuffer *fbo)
{
    fbo->bind();
    s_fbos.push(fbo);
}

GLFramebuffer *GLFramebuffer::popFramebuffer()
{
    GLFramebuffer *ret = s_fbos.pop();
    if (!s_fbos.isEmpty()) {
        s_fbos.top()->bind();
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    return ret;
}

GLFramebuffer::GLFramebuffer()
    : m_colorAttachment(nullptr)
{
}

static QString formatFramebufferStatus(GLenum status)
{
    switch (status) {
    case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
        // An attachment is the wrong type / is invalid / has 0 width or height
        return QStringLiteral("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT");
    case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
        // There are no images attached to the framebuffer
        return QStringLiteral("GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT");
    case GL_FRAMEBUFFER_UNSUPPORTED:
        // A format or the combination of formats of the attachments is unsupported
        return QStringLiteral("GL_FRAMEBUFFER_UNSUPPORTED");
    case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS_EXT:
        // Not all attached images have the same width and height
        return QStringLiteral("GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS_EXT");
    case GL_FRAMEBUFFER_INCOMPLETE_FORMATS_EXT:
        // The color attachments don't have the same format
        return QStringLiteral("GL_FRAMEBUFFER_INCOMPLETE_FORMATS_EXT");
    case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE_EXT:
        // The attachments don't have the same number of samples
        return QStringLiteral("GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE");
    case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER_EXT:
        // The draw buffer is missing
        return QStringLiteral("GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER");
    case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER_EXT:
        // The read buffer is missing
        return QStringLiteral("GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER");
    default:
        return QStringLiteral("Unknown (0x") + QString::number(status, 16) + QStringLiteral(")");
    }
}

GLFramebuffer::GLFramebuffer(GLTexture *colorAttachment, Attachment attachment)
    : mSize(colorAttachment->size())
    , m_colorAttachment(colorAttachment)
{
    if (!sSupported) {
        qCCritical(LIBKWINGLUTILS) << "Framebuffer objects aren't supported!";
        return;
    }

    GLuint prevFbo = 0;
    if (const GLFramebuffer *current = currentFramebuffer()) {
        prevFbo = current->handle();
    }

    glGenFramebuffers(1, &mFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffer);

    initColorAttachment(colorAttachment);
    if (attachment == Attachment::CombinedDepthStencil) {
        initDepthStencilAttachment();
    }

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        // We have an incomplete framebuffer, consider it invalid
        qCCritical(LIBKWINGLUTILS) << "Invalid framebuffer status: " << formatFramebufferStatus(status);
        glDeleteFramebuffers(1, &mFramebuffer);
        return;
    }

    mValid = true;
}

GLFramebuffer::GLFramebuffer(GLuint handle, const QSize &size)
    : mFramebuffer(handle)
    , mSize(size)
    , mValid(true)
    , mForeign(true)
    , m_colorAttachment(nullptr)
{
}

GLFramebuffer::~GLFramebuffer()
{
    if (!mForeign && mValid) {
        glDeleteFramebuffers(1, &mFramebuffer);
    }
    if (mDepthBuffer) {
        glDeleteRenderbuffers(1, &mDepthBuffer);
    }
    if (mStencilBuffer && mStencilBuffer != mDepthBuffer) {
        glDeleteRenderbuffers(1, &mStencilBuffer);
    }
}

bool GLFramebuffer::bind()
{
    if (!valid()) {
        qCCritical(LIBKWINGLUTILS) << "Can't enable invalid framebuffer object!";
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, handle());
    glViewport(0, 0, mSize.width(), mSize.height());

    return true;
}

void GLFramebuffer::initColorAttachment(GLTexture *colorAttachment)
{
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           colorAttachment->target(), colorAttachment->texture(), 0);
}

void GLFramebuffer::initDepthStencilAttachment()
{
    GLuint buffer = 0;

    // Try to attach a depth/stencil combined attachment.
    if (sSupportsPackedDepthStencil) {
        glGenRenderbuffers(1, &buffer);
        glBindRenderbuffer(GL_RENDERBUFFER, buffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, mSize.width(), mSize.height());
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, buffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, buffer);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteRenderbuffers(1, &buffer);
        } else {
            mDepthBuffer = buffer;
            mStencilBuffer = buffer;
            return;
        }
    }

    // Try to attach a depth attachment separately.
    GLenum depthFormat;
    if (GLPlatform::instance()->isGLES()) {
        if (sSupportsDepth24) {
            depthFormat = GL_DEPTH_COMPONENT24;
        } else {
            depthFormat = GL_DEPTH_COMPONENT16;
        }
    } else {
        depthFormat = GL_DEPTH_COMPONENT;
    }

    glGenRenderbuffers(1, &buffer);
    glBindRenderbuffer(GL_RENDERBUFFER, buffer);
    glRenderbufferStorage(GL_RENDERBUFFER, depthFormat, mSize.width(), mSize.height());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, buffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteRenderbuffers(1, &buffer);
    } else {
        mDepthBuffer = buffer;
    }

    // Try to attach a stencil attachment separately.
    GLenum stencilFormat;
    if (GLPlatform::instance()->isGLES()) {
        stencilFormat = GL_STENCIL_INDEX8;
    } else {
        stencilFormat = GL_STENCIL_INDEX;
    }

    glGenRenderbuffers(1, &buffer);
    glBindRenderbuffer(GL_RENDERBUFFER, buffer);
    glRenderbufferStorage(GL_RENDERBUFFER, stencilFormat, mSize.width(), mSize.height());
    glFramebufferRenderbuffer(GL_RENDERBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, buffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteRenderbuffers(1, &buffer);
    } else {
        mStencilBuffer = buffer;
    }
}

void GLFramebuffer::blitFromFramebuffer(const QRect &source, const QRect &destination, GLenum filter, bool flipX, bool flipY)
{
    if (!valid()) {
        return;
    }

    const GLFramebuffer *top = currentFramebuffer();
    GLFramebuffer::pushFramebuffer(this);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, handle());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, top->handle());

    const QRect s = source.isNull() ? QRect(QPoint(0, 0), top->size()) : source;
    const QRect d = destination.isNull() ? QRect(QPoint(0, 0), size()) : destination;

    GLuint srcX0 = s.x();
    GLuint srcY0 = top->size().height() - (s.y() + s.height());
    GLuint srcX1 = s.x() + s.width();
    GLuint srcY1 = top->size().height() - s.y();
    if (flipX) {
        std::swap(srcX0, srcX1);
    }
    if (flipY) {
        std::swap(srcY0, srcY1);
    }

    const GLuint dstX0 = d.x();
    const GLuint dstY0 = mSize.height() - (d.y() + d.height());
    const GLuint dstX1 = d.x() + d.width();
    const GLuint dstY1 = mSize.height() - d.y();

    glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, GL_COLOR_BUFFER_BIT, filter);

    GLFramebuffer::popFramebuffer();
}

bool GLFramebuffer::blitFromRenderTarget(const RenderTarget &sourceRenderTarget, const RenderViewport &sourceViewport, const QRect &source, const QRect &destination)
{
    TextureTransforms transform = sourceRenderTarget.texture() ? sourceRenderTarget.texture()->contentTransforms() : TextureTransforms();
    const bool hasRotation = (transform & TextureTransform::Rotate90) || (transform & TextureTransform::Rotate180) || (transform & TextureTransform::Rotate270);
    if (!hasRotation && blitSupported()) {
        // either no transformation or flipping only
        blitFromFramebuffer(sourceViewport.mapToRenderTarget(source), destination, GL_LINEAR, transform & TextureTransform::MirrorX, transform & TextureTransform::MirrorY);
        return true;
    } else {
        const auto texture = sourceRenderTarget.texture();
        if (!texture) {
            // rotations aren't possible without a texture
            return false;
        }

        GLFramebuffer::pushFramebuffer(this);

        QMatrix4x4 mat;
        mat.ortho(QRectF(QPointF(), size()));
        // GLTexture::render renders with origin (0, 0), move it to the correct place
        mat.translate(destination.x(), destination.y());

        ShaderBinder binder(ShaderTrait::MapTexture);
        binder.shader()->setUniform(GLShader::ModelViewProjectionMatrix, mat);

        texture->render(sourceViewport.mapToRenderTargetTexture(source), infiniteRegion(), destination.size(), 1);

        GLFramebuffer::popFramebuffer();
        return true;
    }
}

GLTexture *GLFramebuffer::colorAttachment() const
{
    return m_colorAttachment;
}

}
