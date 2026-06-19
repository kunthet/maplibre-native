#pragma once

// Zero-copy GPU producer for the Windows ANGLE/EGL build.
//
// Only compiled when the renderer is ANGLE/EGL (MAPLIBRE_EMBED_GPU_SURFACE).
// Under the desktop-GL (WGL) build this header is inert, so the embedder keeps
// using the CPU pixel-buffer path.
#ifdef MAPLIBRE_EMBED_GPU_SURFACE

namespace maplibre_windows {

/// Renders mbgl frames into a shareable D3D11 texture so Flutter can composite
/// them with zero CPU round-trips, via a DXGI shared handle.
///
/// Threading / lifetime: every method must be called on the map render thread
/// while mbgl's EGL context is current (i.e. inside a `gfx::BackendScope`). All
/// methods fail gracefully (return false) when the GPU path is unavailable so
/// the caller can fall back to the CPU pixel-buffer path.
class GpuSurface {
public:
    GpuSurface();
    ~GpuSurface();

    GpuSurface(const GpuSurface&) = delete;
    GpuSurface& operator=(const GpuSurface&) = delete;

    /// Queries the live ANGLE EGL display + backing D3D11 device. Returns false
    /// if the current context is not an ANGLE/D3D11 context exposing the
    /// required device-query extensions.
    bool Initialize();

    /// Validates the full GPU capability without depending on mbgl's render
    /// state: creates the shared D3D11 texture at the given size, wraps it in an
    /// EGL pbuffer, makes it current and clears it. Returns false (and leaves no
    /// committed state) if any step fails, so the caller can fall back to CPU.
    /// On success the sized texture is retained for the first Publish.
    bool SelfTest(int width, int height);

    /// Blits the currently-bound (mbgl) framebuffer into the shared texture,
    /// (re)creating the texture when the size changes. Leaves mbgl's surface and
    /// framebuffer binding restored. Returns false on any failure.
    bool Publish(int width, int height);

    bool available() const { return available_; }
    void* shared_handle() const { return shared_handle_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    bool EnsureSize(int width, int height);
    void ReleaseCurrentTexture();

    // EGL/D3D handles are stored as void* to keep platform GL headers out of
    // this header (it is included by the embedder, which must stay GL-agnostic).
    void* display_ = nullptr;        // EGLDisplay
    void* config_ = nullptr;         // EGLConfig (reused from mbgl's context)
    void* pbuffer_ = nullptr;        // EGLSurface wrapping the D3D texture
    void* device_ = nullptr;         // ID3D11Device* (owned by ANGLE; not ref-counted)
    void* texture_ = nullptr;        // ID3D11Texture2D* (owned here)
    void* shared_handle_ = nullptr;  // DXGI legacy shared HANDLE for Flutter
    int width_ = 0;
    int height_ = 0;
    bool available_ = false;
};

}  // namespace maplibre_windows

#endif  // MAPLIBRE_EMBED_GPU_SURFACE
