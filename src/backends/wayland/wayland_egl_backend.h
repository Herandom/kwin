/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2013 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2019 Roman Gilg <subdiff@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/outputlayer.h"
#include "platformsupport/scenes/opengl/abstract_egl_backend.h"
#include "utils/damagejournal.h"

#include <memory>

namespace KWin
{
class GLFramebuffer;
class GraphicsBufferAllocator;

namespace Wayland
{
class WaylandBackend;
class WaylandOutput;
class WaylandEglBackend;

class WaylandEglLayerBuffer
{
public:
    WaylandEglLayerBuffer(GraphicsBuffer *buffer, WaylandEglBackend *backend);
    ~WaylandEglLayerBuffer();

    GraphicsBuffer *graphicsBuffer() const;
    GLFramebuffer *framebuffer() const;
    std::shared_ptr<GLTexture> texture() const;
    int age() const;

private:
    GraphicsBuffer *m_graphicsBuffer;
    std::unique_ptr<GLFramebuffer> m_framebuffer;
    std::shared_ptr<GLTexture> m_texture;
    int m_age = 0;
    friend class WaylandEglLayerSwapchain;
};

class WaylandEglLayerSwapchain
{
public:
    WaylandEglLayerSwapchain(const QSize &size, uint32_t format, const QVector<uint64_t> &modifiers, WaylandEglBackend *backend);
    ~WaylandEglLayerSwapchain();

    QSize size() const;

    std::shared_ptr<WaylandEglLayerBuffer> acquire();
    void release(std::shared_ptr<WaylandEglLayerBuffer> buffer);

private:
    WaylandEglBackend *m_backend;
    QSize m_size;
    uint32_t m_format;
    QVector<uint64_t> m_modifiers;
    std::unique_ptr<GraphicsBufferAllocator> m_allocator;
    QVector<std::shared_ptr<WaylandEglLayerBuffer>> m_buffers;
};

class WaylandEglPrimaryLayer : public OutputLayer
{
public:
    WaylandEglPrimaryLayer(WaylandOutput *output, WaylandEglBackend *backend);
    ~WaylandEglPrimaryLayer() override;

    GLFramebuffer *fbo() const;
    std::shared_ptr<GLTexture> texture() const;
    void present();

    std::optional<OutputLayerBeginFrameInfo> beginFrame() override;
    bool endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion) override;
    quint32 format() const override;

private:
    WaylandOutput *m_waylandOutput;
    DamageJournal m_damageJournal;
    std::unique_ptr<WaylandEglLayerSwapchain> m_swapchain;
    std::shared_ptr<WaylandEglLayerBuffer> m_buffer;
    WaylandEglBackend *const m_backend;

    friend class WaylandEglBackend;
};

class WaylandEglCursorLayer : public OutputLayer
{
    Q_OBJECT

public:
    WaylandEglCursorLayer(WaylandOutput *output, WaylandEglBackend *backend);
    ~WaylandEglCursorLayer() override;

    std::optional<OutputLayerBeginFrameInfo> beginFrame() override;
    bool endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion) override;
    quint32 format() const override;

private:
    WaylandOutput *m_output;
    WaylandEglBackend *m_backend;
    std::unique_ptr<WaylandEglLayerSwapchain> m_swapchain;
    std::shared_ptr<WaylandEglLayerBuffer> m_buffer;
};

/**
 * @brief OpenGL Backend using Egl on a Wayland surface.
 *
 * This Backend is the basis for a session compositor running on top of a Wayland system compositor.
 * It creates a Surface as large as the screen and maps it as a fullscreen shell surface on the
 * system compositor. The OpenGL context is created on the Wayland surface, so for rendering X11 is
 * not involved.
 *
 * Also in repainting the backend is currently still rather limited. Only supported mode is fullscreen
 * repaints, which is obviously not optimal. Best solution is probably to go for buffer_age extension
 * and make it the only available solution next to fullscreen repaints.
 */
class WaylandEglBackend : public AbstractEglBackend
{
    Q_OBJECT
public:
    WaylandEglBackend(WaylandBackend *b);
    ~WaylandEglBackend() override;

    WaylandBackend *backend() const;

    std::unique_ptr<SurfaceTexture> createSurfaceTextureInternal(SurfacePixmapInternal *pixmap) override;
    std::unique_ptr<SurfaceTexture> createSurfaceTextureWayland(SurfacePixmapWayland *pixmap) override;

    void init() override;
    void present(Output *output) override;
    OutputLayer *primaryLayer(Output *output) override;
    OutputLayer *cursorLayer(Output *output) override;

    std::pair<std::shared_ptr<KWin::GLTexture>, ColorDescription> textureForOutput(KWin::Output *output) const override;

private:
    bool initializeEgl();
    bool initRenderingContext();
    bool createEglWaylandOutput(Output *output);
    void cleanupSurfaces() override;

    struct Layers
    {
        std::unique_ptr<WaylandEglPrimaryLayer> primaryLayer;
        std::unique_ptr<WaylandEglCursorLayer> cursorLayer;
    };

    WaylandBackend *m_backend;
    std::map<Output *, Layers> m_outputs;
};

} // namespace Wayland
} // namespace KWin
