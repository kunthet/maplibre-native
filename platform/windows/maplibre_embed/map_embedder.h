#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace maplibre_windows {

struct CameraState {
    double lat = 0;
    double lon = 0;
    double zoom = 0;
    double bearing = 0;
    double pitch = 0;
};

using EventCallback = std::function<void(const std::string& type, const std::string& json_payload)>;

/// Invoked when a frame has been rendered into a GPU-resident shared texture.
/// `shared_handle` is a DXGI legacy shared HANDLE; `width`/`height` are physical
/// pixels. Only used when the ANGLE/EGL zero-copy path is active.
using GpuFrameCallback = std::function<void(void* shared_handle, int width, int height)>;

/// Owns mbgl::Map on a dedicated RunLoop thread and renders via HeadlessFrontend.
class MapEmbedder {
public:
    MapEmbedder(int width,
                int height,
                float pixel_ratio,
                std::string init_style,
                std::optional<CameraState> init_camera,
                EventCallback on_event,
                std::function<void(const uint8_t*, size_t, size_t)> on_pixels,
                GpuFrameCallback on_gpu_frame = {});
    ~MapEmbedder();

    /// True when the zero-copy GPU surface path is active. Valid immediately
    /// after construction; the plugin uses it to pick the texture variant.
    bool IsGpuMode() const;

    MapEmbedder(const MapEmbedder&) = delete;
    MapEmbedder& operator=(const MapEmbedder&) = delete;

    void Resize(int width, int height);
    void SetStyle(std::string style);
    void MoveCamera(const CameraState& camera);
    void AnimateCamera(const CameraState& camera, int duration_ms);
    CameraState GetCamera() const;
    std::pair<double, double> ToLngLat(double x, double y) const;
    std::pair<double, double> ToScreenLocation(double lon, double lat) const;

    void SetDragPanEnabled(bool enabled);
    /// `invert` false = GLFW parity (negate Flutter scroll delta). true = raw Flutter sign.
    void SetInvertWheelZoom(bool invert);
    void OnPointer(const std::string& phase,
                   double x,
                   double y,
                   double scroll_delta,
                   bool shift,
                   bool control);

    void AddSource(const std::string& id, const std::string& source_json);
    void AddLayer(const std::string& layer_json, const std::optional<std::string>& below_layer_id);
    void ApplyStyleLayers(const std::vector<std::string>& remove_ids,
                          const std::vector<std::string>& layer_json);
    void RemoveLayer(const std::string& id);
    void RemoveSource(const std::string& id);
    void UpdateGeoJsonSource(const std::string& id, const std::string& data);
    void UpdateLayerFilter(const std::string& id, const std::string& filter_json);
    void UpdateVectorSourceTiles(const std::string& id, const std::vector<std::string>& tiles);
    void AddImage(const std::string& id, const std::vector<uint8_t>& png_bytes);
    void RemoveImage(const std::string& id);
    std::vector<std::string> GetLayerIds() const;
    std::vector<std::string> GetAttributions() const;
    std::string FeaturesAtPointJson(double x, double y, const std::vector<std::string>& layer_ids) const;

private:
    enum class GestureMode { None, Pan, Pitch, Rotate };

    void ThreadMain();
    void InvokeSync(const std::function<void()>& fn) const;
    void InvokeAsync(std::function<void()> fn) const;
    template <typename T>
    T InvokeSyncValue(const std::function<T()>& fn) const;
    void RequestRender();
    void RequestRenderQuick();
    void RequestRenderUntilIdle();
    void PublishFrame();
    void PublishPixels(const uint8_t* data, size_t width, size_t height);
    void CapturePanSnapshot(double x, double y);
    void ReleasePanSnapshot();
    void PublishPanSnapshot(double x, double y);
    void SetInteracting(bool active);
    void ExtendScrollGestureFreeze();
    void SyncGestureFreezeState();
    void RestartFrameTick();
    void StopFrameTick();
    void ProcessFrameTick();
    void SchedulePanPublish();
    void ProcessPendingPanFrame();
    void SchedulePointerInput();
    void ProcessPendingPointerInput();
    void ClearPanSnapshot(bool trigger_repaint = false);
    void FinishPanGesture();
    void ShutdownOnRunLoop();
    void ProcessPointerOnLoop(const std::string& phase,
                              double x,
                              double y,
                              double scroll_delta,
                              bool shift,
                              bool control);
    bool ApplyPendingScrollZoom();
    std::string NormalizeStyleUrl(std::string style) const;

    const float pixel_ratio_;
    EventCallback on_event_;
    std::function<void(const uint8_t*, size_t, size_t)> on_pixels_;
    GpuFrameCallback on_gpu_frame_;

    mutable std::mutex ready_mutex_;
    std::condition_variable ready_cv_;
    bool thread_ready_ = false;

    std::thread thread_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace maplibre_windows
