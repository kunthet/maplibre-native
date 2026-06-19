#include "gpu_surface_d3d.h"

#ifdef MAPLIBRE_EMBED_GPU_SURFACE

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <d3d11.h>
#include <dxgi.h>

#include <mbgl/util/logging.hpp>

#include <sstream>
#include <string>

// ANGLE-specific EGL tokens (defined defensively in case the installed
// eglext headers predate them).
#ifndef EGL_DEVICE_EXT
#define EGL_DEVICE_EXT 0x322C
#endif
#ifndef EGL_D3D11_DEVICE_ANGLE
#define EGL_D3D11_DEVICE_ANGLE 0x33A1
#endif
#ifndef EGL_D3D_TEXTURE_ANGLE
#define EGL_D3D_TEXTURE_ANGLE 0x33A3
#endif

namespace maplibre_windows {
namespace {

void LogError(const std::string& message) {
    mbgl::Log::Error(mbgl::Event::OpenGL, "[GpuSurface] " + message);
}

void LogInfo(const std::string& message) {
    mbgl::Log::Info(mbgl::Event::OpenGL, "[GpuSurface] " + message);
}

std::string EglErr() {
    std::ostringstream out;
    out << "eglError=0x" << std::hex << eglGetError();
    return out.str();
}

}  // namespace

GpuSurface::GpuSurface() = default;

GpuSurface::~GpuSurface() {
    ReleaseCurrentTexture();
    // device_ is owned by ANGLE; intentionally not released.
}

bool GpuSurface::Initialize() {
    EGLDisplay display = eglGetCurrentDisplay();
    EGLContext context = eglGetCurrentContext();
    if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) {
        LogError("no current EGL display/context (not an ANGLE build?)");
        return false;
    }
    display_ = display;

    auto query_display =
        reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(eglGetProcAddress("eglQueryDisplayAttribEXT"));
    auto query_device =
        reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(eglGetProcAddress("eglQueryDeviceAttribEXT"));
    if (!query_display || !query_device) {
        LogError("EGL_EXT_device_query unavailable; GPU surface disabled");
        return false;
    }

    EGLAttrib device_attrib = 0;
    if (!query_display(display, EGL_DEVICE_EXT, &device_attrib)) {
        LogError("eglQueryDisplayAttribEXT(EGL_DEVICE_EXT) failed; " + EglErr());
        return false;
    }

    EGLAttrib d3d_attrib = 0;
    auto egl_device = reinterpret_cast<EGLDeviceEXT>(device_attrib);
    if (!query_device(egl_device, EGL_D3D11_DEVICE_ANGLE, &d3d_attrib) || d3d_attrib == 0) {
        LogError("ANGLE is not running on the D3D11 backend; GPU surface disabled");
        return false;
    }
    device_ = reinterpret_cast<void*>(d3d_attrib);

    // Reuse mbgl's exact EGLConfig so the pbuffer is guaranteed compatible with
    // the context we will make current for the blit.
    EGLint config_id = 0;
    if (!eglQueryContext(display, context, EGL_CONFIG_ID, &config_id)) {
        LogError("eglQueryContext(EGL_CONFIG_ID) failed; " + EglErr());
        return false;
    }
    const EGLint config_attribs[] = {EGL_CONFIG_ID, config_id, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint num_configs = 0;
    if (!eglChooseConfig(display, config_attribs, &config, 1, &num_configs) || num_configs != 1) {
        LogError("eglChooseConfig(by config id) failed; " + EglErr());
        return false;
    }
    config_ = config;

    available_ = true;
    LogInfo("initialized (ANGLE/D3D11 zero-copy surface)");
    return true;
}

void GpuSurface::ReleaseCurrentTexture() {
    if (display_ && pbuffer_) {
        eglDestroySurface(static_cast<EGLDisplay>(display_), static_cast<EGLSurface>(pbuffer_));
    }
    pbuffer_ = nullptr;
    if (texture_) {
        static_cast<ID3D11Texture2D*>(texture_)->Release();
        texture_ = nullptr;
    }
    shared_handle_ = nullptr;
    width_ = 0;
    height_ = 0;
}

bool GpuSurface::EnsureSize(int width, int height) {
    if (texture_ && width == width_ && height == height_) {
        return true;
    }
    if (width <= 0 || height <= 0) {
        return false;
    }

    auto* device = static_cast<ID3D11Device*>(device_);
    if (!device) {
        return false;
    }

    // Create the new resources into locals first; only swap in on full success
    // so a failure leaves the previously working surface untouched.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // BGRA8 (not RGBA8) is mandatory: Flutter opens the shared handle via
    // EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE and then eglBindTexImage()s it as a
    // sampled texture, and ANGLE's legacy share-handle + bind-to-texture path
    // only supports DXGI_FORMAT_B8G8R8A8_UNORM. Colors stay correct because
    // glBlitFramebuffer copies by GL component (R->R, G->G, B->B), and ANGLE
    // maps those components onto the BGRA texture's bytes on both ends.
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // RENDER_TARGET: we blit into it via an EGL pbuffer on the producer side.
    // SHADER_RESOURCE: Flutter samples it on the consumer side.
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // Legacy shared resource: GetSharedHandle yields a HANDLE that Flutter's
    // engine opens via EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr) || !texture) {
        LogError("ID3D11Device::CreateTexture2D failed");
        return false;
    }

    HANDLE shared_handle = nullptr;
    IDXGIResource* dxgi_resource = nullptr;
    if (SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&dxgi_resource))) &&
        dxgi_resource) {
        dxgi_resource->GetSharedHandle(&shared_handle);
        dxgi_resource->Release();
    }
    if (!shared_handle) {
        LogError("IDXGIResource::GetSharedHandle failed");
        texture->Release();
        return false;
    }

    const EGLint pbuffer_attribs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    EGLSurface pbuffer = eglCreatePbufferFromClientBuffer(static_cast<EGLDisplay>(display_),
                                                          EGL_D3D_TEXTURE_ANGLE,
                                                          static_cast<EGLClientBuffer>(texture),
                                                          static_cast<EGLConfig>(config_),
                                                          pbuffer_attribs);
    if (pbuffer == EGL_NO_SURFACE) {
        LogError("eglCreatePbufferFromClientBuffer(EGL_D3D_TEXTURE_ANGLE) failed; " + EglErr());
        texture->Release();
        return false;
    }

    ReleaseCurrentTexture();
    texture_ = texture;
    shared_handle_ = shared_handle;
    pbuffer_ = pbuffer;
    width_ = width;
    height_ = height;

    std::ostringstream out;
    out << "shared texture created " << width << "x" << height;
    LogInfo(out.str());
    return true;
}

bool GpuSurface::SelfTest(int width, int height) {
    if (!available_) {
        return false;
    }
    // Create the actual shared texture + EGL pbuffer at the initial size. This
    // exercises ID3D11Device::CreateTexture2D(SHARED), GetSharedHandle, and
    // eglCreatePbufferFromClientBuffer(EGL_D3D_TEXTURE_ANGLE) -- the parts most
    // likely to be unsupported on a given GPU/driver.
    if (!EnsureSize(width, height)) {
        return false;
    }

    EGLDisplay display = static_cast<EGLDisplay>(display_);
    EGLContext context = eglGetCurrentContext();
    EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);
    if (context == EGL_NO_CONTEXT) {
        return false;
    }

    // Make the D3D-backed surface current and render into it, proving ANGLE can
    // actually present GL output into the shared texture.
    if (!eglMakeCurrent(display, static_cast<EGLSurface>(pbuffer_), static_cast<EGLSurface>(pbuffer_), context)) {
        LogError("self-test eglMakeCurrent(pbuffer) failed; " + EglErr());
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    const GLenum gl_err = glGetError();

    // Restore whatever surface mbgl had current before the probe.
    eglMakeCurrent(display, prev_draw, prev_read, context);

    if (gl_err != GL_NO_ERROR) {
        std::ostringstream out;
        out << "self-test GL error 0x" << std::hex << gl_err;
        LogError(out.str());
        return false;
    }
    LogInfo("self-test passed (rendered into shared D3D11 surface)");
    return true;
}

bool GpuSurface::Publish(int width, int height) {
    if (!available_) {
        return false;
    }
    if (!EnsureSize(width, height)) {
        return false;
    }

    EGLDisplay display = static_cast<EGLDisplay>(display_);
    EGLContext context = eglGetCurrentContext();
    EGLSurface mbgl_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface mbgl_read = eglGetCurrentSurface(EGL_READ);
    if (context == EGL_NO_CONTEXT) {
        return false;
    }

    // mbgl renders into its own FBO and leaves it bound; capture it as the blit
    // source before switching the default framebuffer to our D3D-backed surface.
    GLint source_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &source_fbo);
    if (source_fbo == 0) {
        // Nothing rendered into an offscreen FBO; defer to the CPU path.
        return false;
    }

    if (!eglMakeCurrent(display, static_cast<EGLSurface>(pbuffer_), static_cast<EGLSurface>(pbuffer_), context)) {
        LogError("eglMakeCurrent(pbuffer) failed; " + EglErr());
        return false;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(source_fbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    // Drain any GL errors left in the queue by mbgl's own render (e.g. the
    // GL_INVALID_OPERATION from the unsupported UBO glMapBufferRange path, which
    // mbgl's release-mode error checks do not clear) so the post-blit check below
    // reflects only the blit itself. Bounded so a context-lost driver can't spin.
    for (int drained = 0; drained < 32 && glGetError() != GL_NO_ERROR; ++drained) {
    }

    // Vertical flip: GL's origin is bottom-left, Flutter samples top-left.
    glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // Log a real blit error once; never per-frame (this runs on every frame).
    static bool logged_blit_error = false;
    if (!logged_blit_error) {
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            logged_blit_error = true;
            std::ostringstream out;
            out << "blit GL error 0x" << std::hex << err;
            LogError(out.str());
        }
    }

    // Ensure the GPU has finished writing before Flutter's device reads the
    // shared texture (the Windows embedder does not drive a keyed mutex).
    glFinish();

    // Restore mbgl's surfaces and framebuffer binding so the next render is
    // unaffected by the blit.
    eglMakeCurrent(display, mbgl_draw, mbgl_read, context);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(source_fbo));

    // One-time confirmation that the zero-copy path is live.
    static bool logged_first = false;
    if (!logged_first) {
        logged_first = true;
        LogInfo("zero-copy publish active (mbgl FBO -> shared D3D11 texture)");
    }
    return true;
}

}  // namespace maplibre_windows

#endif  // MAPLIBRE_EMBED_GPU_SURFACE
