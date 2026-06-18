#pragma once

#include "glfw_backend.hpp"

#include <mbgl/gfx/renderable.hpp>
#include <mbgl/gl/renderer_backend.hpp>

#include <memory>

struct GLFWwindow;

class GLFWGLBackend final : public GLFWBackend, public mbgl::gl::RendererBackend, public mbgl::gfx::Renderable {
public:
    GLFWGLBackend(GLFWwindow*, bool capFrameRate);
    ~GLFWGLBackend() override;

    void swap();
    void setVSyncEnabled(bool enabled) override;

    void requestPanSnapshotCaptureOnNextSwap() override;
    bool captureDisplayedPanSnapshot() override;
    bool drawPanSnapshot(float offsetX, float offsetY) override;
    void releasePanSnapshot() override;
    bool hasPanSnapshot() const override;

    // GLFWRendererBackend implementation
public:
    mbgl::gfx::RendererBackend& getRendererBackend() override { return *this; }
    mbgl::Size getSize() const override;
    void setSize(mbgl::Size) override;

    // mbgl::gfx::RendererBackend implementation
public:
    mbgl::gfx::Renderable& getDefaultRenderable() override { return *this; }

protected:
    void activate() override;
    void deactivate() override;

    // mbgl::gl::RendererBackend implementation
protected:
    mbgl::gl::ProcAddress getExtensionFunctionPointer(const char*) override;
    void updateAssumedState() override;
    void resetRendererStateAfterExternalDraw();

private:
    struct PanSnapshot;

    GLFWwindow* window;
    bool vsyncEnabled = true;
    bool panSnapshotCaptureBeforeSwap = false;
    std::unique_ptr<PanSnapshot> panSnapshot;
};
