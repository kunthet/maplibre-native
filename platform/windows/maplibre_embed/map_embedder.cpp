#include "map_embedder.h"

#ifdef MAPLIBRE_EMBED_GPU_SURFACE
#ifdef _WIN32
#include "gpu_surface_d3d.h"
#elif defined(__APPLE__)
#include "gpu_surface_mtl.h"
#endif
#endif

#include <mbgl/gfx/backend_scope.hpp>
#include <mbgl/gfx/headless_frontend.hpp>
#include <mbgl/map/map.hpp>
#include <mbgl/map/map_observer.hpp>
#include <mbgl/map/map_options.hpp>
#include <mbgl/map/transform_state.hpp>
#include <mbgl/renderer/query.hpp>
#include <mbgl/renderer/renderer.hpp>
#include <mbgl/storage/file_source_manager.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/style/conversion/filter.hpp>
#include <mbgl/style/filter.hpp>
#include <mbgl/style/conversion/geojson.hpp>
#include <mbgl/style/conversion/layer.hpp>
#include <mbgl/style/conversion/source.hpp>
#include <mbgl/style/conversion_impl.hpp>
#include <mbgl/style/image.hpp>
#include <mbgl/style/layers/background_layer.hpp>
#include <mbgl/style/layers/circle_layer.hpp>
#include <mbgl/style/layers/fill_extrusion_layer.hpp>
#include <mbgl/style/layers/fill_layer.hpp>
#include <mbgl/style/layers/heatmap_layer.hpp>
#include <mbgl/style/layers/hillshade_layer.hpp>
#include <mbgl/style/layers/line_layer.hpp>
#include <mbgl/style/layers/raster_layer.hpp>
#include <mbgl/style/layers/symbol_layer.hpp>
#include <mbgl/style/sources/geojson_source.hpp>
#include <mbgl/style/sources/raster_dem_source.hpp>
#include <mbgl/style/sources/raster_source.hpp>
#include <mbgl/style/sources/vector_source.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/util/tile_server_options.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/run_loop.hpp>
#include <mbgl/util/timer.hpp>

#include <mbgl/style/rapidjson_conversion.hpp>
#include <mbgl/util/rapidjson.hpp>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>

#ifndef NDEBUG
static bool MapTraceEnabled() {
    static const bool on = [] {
        const char* v = getenv("MYURA_TRACE");
        return v != nullptr && v[0] == '1';
    }();
    return on;
}
#define MAP_TRACE(...)                                   \
    do {                                                 \
        if (MapTraceEnabled()) {                         \
            fprintf(stderr, "[MapTrace] " __VA_ARGS__);  \
            fprintf(stderr, "\n");                        \
        }                                                \
    } while (0)
#else
#define MAP_TRACE(...) do {} while (0)
#endif

namespace maplibre_windows {
namespace {

using namespace mbgl;


std::mutex g_gl_init_mutex;

class EmbedderObserver : public MapObserver {
public:
    EmbedderObserver(std::function<void(const std::string&, const std::string&)> emit,
                     std::function<void()> request_render_until_idle,
                     std::function<void()> request_render_quick,
                     std::function<void()> on_style_fully_loaded,
                     std::atomic<bool>* needs_repaint,
                     std::atomic<bool>* pumping_render,
                     std::atomic<bool>* pan_snapshot_active,
                     std::atomic<bool>* suppress_idle_events,
                     std::atomic<bool>* animating,
                     bool* interacting,
                     bool* scroll_gesture_frozen)
        : emit_(std::move(emit)),
          request_render_until_idle_(std::move(request_render_until_idle)),
          request_render_quick_(std::move(request_render_quick)),
          on_style_fully_loaded_(std::move(on_style_fully_loaded)),
          needs_repaint_(needs_repaint),
          pumping_render_(pumping_render),
          pan_snapshot_active_(pan_snapshot_active),
          suppress_idle_events_(suppress_idle_events),
          animating_(animating),
          interacting_(interacting),
          scroll_gesture_frozen_(scroll_gesture_frozen) {}

    void onDidFinishLoadingStyle() override {
        if (on_style_fully_loaded_) {
            on_style_fully_loaded_();
        }
        emit_("styleLoaded", "{}");
        // Only request a quick render (2 frames) after style load, not a full
        // render-until-idle. A full idle pump can block the render thread for
        // hundreds of frames while the C# layer queues P/Invoke work (AddSource,
        // AddLayer via InvokeSync from the styleLoaded handler chain). Those
        // calls need the render thread's run loop to process queued work, so
        // blocking it here would deadlock.
        if (request_render_quick_) {
            request_render_quick_();
        }
    }
    void onDidBecomeIdle() override { emit_("mapIdle", "{}"); }
    void onCameraDidChange(CameraChangeMode) override {
        // Intentionally not emitted: the Windows Dart layer refreshes the camera
        // on `cameraIdle`, so a per-frame `cameraChanged` event only adds
        // cross-thread channel churn during animations/gestures.
    }
    void onDidFinishRenderingFrame(const RenderFrameStatus& status) override {
        if (needs_repaint_) {
            needs_repaint_->store(status.needsRepaint);
        }
        if (!status.needsRepaint) {
            if (!suppress_idle_events_ || !suppress_idle_events_->load()) {
                emit_("cameraIdle", "{}");
            }
        } else if (request_render_quick_ && pumping_render_ && !pumping_render_->load() &&
                   (!pan_snapshot_active_ || !pan_snapshot_active_->load()) &&
                   (!animating_ || !animating_->load()) && (!interacting_ || !*interacting_) &&
                   (!scroll_gesture_frozen_ || !*scroll_gesture_frozen_)) {
            // While a camera animation is running the frame-tick timer drives the
            // published frames; self-scheduling here would recurse for the whole
            // animation. During an active pointer gesture or wheel-zoom burst the
            // interaction frame tick drives repaints instead. Tile-loading repaints
            // after a gesture still use this path.
            request_render_quick_();
        }
    }

private:
    std::function<void(const std::string&, const std::string&)> emit_;
    std::function<void()> request_render_until_idle_;
    std::function<void()> request_render_quick_;
    std::function<void()> on_style_fully_loaded_;
    std::atomic<bool>* needs_repaint_ = nullptr;
    std::atomic<bool>* pumping_render_ = nullptr;
    std::atomic<bool>* pan_snapshot_active_ = nullptr;
    std::atomic<bool>* suppress_idle_events_ = nullptr;
    std::atomic<bool>* animating_ = nullptr;
    bool* interacting_ = nullptr;
    bool* scroll_gesture_frozen_ = nullptr;
};

JSDocument ParseJson(const std::string& text) {
    JSDocument doc;
    doc.Parse(text.c_str());
    return doc;
}

constexpr uint8_t kPanEdgeR = 224;
constexpr uint8_t kPanEdgeG = 224;
constexpr uint8_t kPanEdgeB = 224;
constexpr uint8_t kPanEdgeA = 255;

void FillPanEdgeRect(uint8_t* dst, size_t width, size_t height, int x0, int y0, int x1, int y1) {
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(static_cast<int>(width), x1);
    y1 = std::min(static_cast<int>(height), y1);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    for (int y = y0; y < y1; ++y) {
        uint8_t* row = dst + (static_cast<size_t>(y) * width + static_cast<size_t>(x0)) * 4;
        for (int x = x0; x < x1; ++x) {
            row[0] = kPanEdgeR;
            row[1] = kPanEdgeG;
            row[2] = kPanEdgeB;
            row[3] = kPanEdgeA;
            row += 4;
        }
    }
}

void BlitPanSnapshot(const uint8_t* src,
                     size_t width,
                     size_t height,
                     int offset_x,
                     int offset_y,
                     std::vector<uint8_t>& dst) {
    const size_t byte_count = width * height * 4;
    if (dst.size() != byte_count) {
        dst.resize(byte_count);
    }
    if (width == 0 || height == 0) {
        return;
    }

    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);

    // Clear only the exposed edge strips (not the full framebuffer).
    if (offset_y > 0) {
        FillPanEdgeRect(dst.data(), width, height, 0, 0, w, offset_y);
    } else if (offset_y < 0) {
        FillPanEdgeRect(dst.data(), width, height, 0, h + offset_y, w, h);
    }
    if (offset_x > 0) {
        const int y0 = std::max(0, offset_y);
        const int y1 = std::min(h, h + offset_y);
        FillPanEdgeRect(dst.data(), width, height, 0, y0, offset_x, y1);
    } else if (offset_x < 0) {
        const int y0 = std::max(0, offset_y);
        const int y1 = std::min(h, h + offset_y);
        FillPanEdgeRect(dst.data(), width, height, w + offset_x, y0, w, y1);
    }

    if (offset_x >= w || offset_x <= -w || offset_y >= h || offset_y <= -h) {
        return;
    }

    const int src_x0 = std::max(0, -offset_x);
    const int dst_x0 = std::max(0, offset_x);
    const int copy_w = w - std::abs(offset_x);
    if (copy_w <= 0) {
        return;
    }

    const int src_y0 = std::max(0, -offset_y);
    const int dst_y0 = std::max(0, offset_y);
    const int copy_h = h - std::abs(offset_y);
    if (copy_h <= 0) {
        return;
    }

    for (int row = 0; row < copy_h; ++row) {
        const size_t src_row = static_cast<size_t>(src_y0 + row);
        const size_t dst_row = static_cast<size_t>(dst_y0 + row);
        std::memcpy(dst.data() + (dst_row * width + static_cast<size_t>(dst_x0)) * 4,
                    src + (src_row * width + static_cast<size_t>(src_x0)) * 4,
                    static_cast<size_t>(copy_w) * 4);
    }
}

void PumpRenderFrames(HeadlessFrontend* frontend,
                      Map* map,
                      std::atomic<bool>* needs_repaint,
                      int max_frames,
                      const std::atomic<bool>* shutting_down = nullptr,
                      const std::atomic<bool>* cancel_idle_pump = nullptr) {
    if (!frontend || !map || !needs_repaint) {
        return;
    }
    needs_repaint->store(true);
    for (int i = 0; i < max_frames; ++i) {
        if (shutting_down && shutting_down->load()) {
            break;
        }
        if (cancel_idle_pump && cancel_idle_pump->load()) {
            break;
        }
        frontend->renderOnce(*map);
        if (!needs_repaint->load()) {
            break;
        }
    }
}

}  // namespace

struct MapEmbedder::Impl {
    std::unique_ptr<util::RunLoop> run_loop;
    std::unique_ptr<HeadlessFrontend> frontend;
    std::unique_ptr<Map> map;
    std::unique_ptr<EmbedderObserver> observer;
    std::unique_ptr<util::Timer> frame_tick;
    std::unique_ptr<util::Timer> scroll_freeze_timer;
    int width = 0;
    int height = 0;
    bool drag_pan_enabled = true;
    bool invert_wheel_zoom = false;
    std::mutex input_mutex;
    bool pointer_down = false;
    double last_x = 0;
    double last_y = 0;
    GestureMode gesture_mode = GestureMode::None;
    bool interacting = false;
    bool scroll_gesture_frozen = false;
    bool gesture_frozen = false;
    bool pan_frame_dirty = false;
    bool has_pending_pan_move = false;
    std::atomic<bool> pan_publish_scheduled{false};
    std::atomic<bool> pointer_input_scheduled{false};
    std::atomic<bool> finishing_gesture{false};
    double pending_pan_x = 0;
    double pending_pan_y = 0;
    bool has_pending_scroll = false;
    double pending_scroll_x = 0;
    double pending_scroll_y = 0;
    double pending_scroll_delta = 0;
    bool pointer_shift = false;
    bool pointer_control = false;
    double embedder_pan_x = 0;
    double embedder_pan_y = 0;
    uint8_t saved_prefetch_zoom_delta = 0;
    double saved_tile_lod_zoom_shift = 0;
    std::vector<uint8_t> pan_snapshot_pixels;
    size_t pan_snapshot_width = 0;
    size_t pan_snapshot_height = 0;
    std::atomic<bool> pan_snapshot_active{false};
    std::atomic<bool> suppress_idle_events{false};
    double pan_snapshot_origin_x = 0;
    double pan_snapshot_origin_y = 0;
    std::vector<uint8_t> composed_pixels;
    std::vector<uint8_t> last_published_pixels;
    size_t last_published_width = 0;
    size_t last_published_height = 0;
    std::atomic<bool> needs_repaint{true};
    std::atomic<bool> pumping_render{false};
    std::atomic<bool> animating{false};
    std::atomic<bool> shutting_down{false};
    std::atomic<bool> cancel_idle_pump{false};
    std::atomic<bool> gpu_active{false};
    std::optional<mbgl::CameraOptions> pending_initial_camera;
    std::optional<mbgl::CameraOptions> camera_to_restore_after_style;
    bool initial_camera_applied = false;
#ifdef MAPLIBRE_EMBED_GPU_SURFACE
    std::unique_ptr<GpuSurface> gpu_surface;
#endif
#if defined(__APPLE__)
    std::chrono::steady_clock::time_point last_gpu_idle_quick_render{};
#endif
};

namespace {
constexpr int kMaxRenderPumpFrames = 600;
constexpr int kQuickRenderPumpFrames = 2;
constexpr int kInteractionFps = 60;
constexpr int kScrollGestureDebounceMs = 150;
constexpr int kScrollFollowUpRenderFrames = 4;
constexpr auto kGpuIdleQuickRenderMinInterval = std::chrono::milliseconds(33);
}  // namespace

MapEmbedder::MapEmbedder(int width,
                         int height,
                         float pixel_ratio,
                         std::string init_style,
                         std::optional<CameraState> init_camera,
                         EventCallback on_event,
                         std::function<void(const uint8_t*, size_t, size_t)> on_pixels,
                         GpuFrameCallback on_gpu_frame)
    : pixel_ratio_(pixel_ratio),
      on_event_(std::move(on_event)),
      on_pixels_(std::move(on_pixels)),
      on_gpu_frame_(std::move(on_gpu_frame)),
      impl_(std::make_unique<Impl>()) {
    impl_->width = width;
    impl_->height = height;

#if defined(__APPLE__)
    // See map_embedder.h: run the map thread on a pthread with a large (16 MB)
    // stack so CoreGraphics image decoding in AddImage cannot overflow it.
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, static_cast<size_t>(16) * 1024 * 1024);
        auto trampoline = [](void* self) -> void* {
            static_cast<MapEmbedder*>(self)->ThreadMain();
            return nullptr;
        };
        thread_started_ = (pthread_create(&thread_, &attr, trampoline, this) == 0);
        pthread_attr_destroy(&attr);
    }
#else
    thread_ = std::thread([this]() { ThreadMain(); });
#endif


    std::unique_lock<std::mutex> lock(ready_mutex_);
    ready_cv_.wait(lock, [this] { return thread_ready_; });

    InvokeSync([&] {
        if (init_camera) {
            const auto options = mbgl::CameraOptions()
                                     .withCenter(mbgl::LatLng{init_camera->lat, init_camera->lon})
                                     .withZoom(init_camera->zoom)
                                     .withBearing(init_camera->bearing)
                                     .withPitch(init_camera->pitch);
            // Apply before style load so mbgl does not jumpTo(getDefaultCamera())
            // on onStyleLoaded (zoom 0 world view → init zoom flash).
            impl_->map->jumpTo(options);
            impl_->initial_camera_applied = true;
        }
        if (!init_style.empty()) {
            const auto normalized = NormalizeStyleUrl(init_style);
            if (!normalized.empty() && (normalized.front() == '{' || normalized.front() == '[')) {
                impl_->map->getStyle().loadJSON(normalized);
            } else {
                impl_->map->getStyle().loadURL(normalized);
            }
        }
    });
}

void MapEmbedder::ShutdownOnRunLoop() {
    StopFrameTick();
    impl_->frame_tick.reset();
    if (impl_->scroll_freeze_timer) {
        impl_->scroll_freeze_timer->stop();
        impl_->scroll_freeze_timer.reset();
    }
    ReleasePanSnapshot();
    impl_->map.reset();
    impl_->frontend.reset();
    impl_->observer.reset();
    if (impl_->run_loop) {
        impl_->run_loop->stop();
    }
}

MapEmbedder::~MapEmbedder() {
    impl_->shutting_down.store(true);
#if defined(__APPLE__)
    if (!thread_started_) {
        return;
    }
#else
    if (!thread_.joinable()) {
        return;
    }
#endif

    if (impl_->run_loop) {

        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        impl_->run_loop->invoke([&] {
            ShutdownOnRunLoop();
            {
                std::lock_guard<std::mutex> lock(mutex);
                done = true;
            }
            cv.notify_one();
        });
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return done; });
    }

#if defined(__APPLE__)
    if (thread_started_) {
        pthread_join(thread_, nullptr);
        thread_started_ = false;
    }
#else
    if (thread_.joinable()) {
        thread_.join();
    }
#endif
}


void MapEmbedder::ThreadMain() {
    impl_->run_loop = std::make_unique<util::RunLoop>(util::RunLoop::Type::New);
    impl_->observer = std::make_unique<EmbedderObserver>(
        [this](const std::string& type, const std::string& payload) {
            if (on_event_) {
                on_event_(type, payload);
            }
        },
        [this]() { RequestRenderUntilIdle(); },
        [this]() { RequestRenderQuick(); },
        [this]() { OnStyleFullyLoaded(); },
        &impl_->needs_repaint,
        &impl_->pumping_render,
        &impl_->pan_snapshot_active,
        &impl_->suppress_idle_events,
        &impl_->animating,
        &impl_->interacting,
        &impl_->scroll_gesture_frozen);

    const auto cache_path = (std::filesystem::temp_directory_path() / "maplibre_windows_cache.db").string();
    auto map_tiler = TileServerOptions::MapTilerConfiguration();
    ResourceOptions resource_options;
    resource_options.withCachePath(cache_path)
        .withTileServerOptions(map_tiler)
        .withMaximumCacheSize(512 * 1024 * 1024);
    ClientOptions client_options;

    {
        std::lock_guard<std::mutex> gl_lock(g_gl_init_mutex);
        impl_->frontend = std::make_unique<HeadlessFrontend>(
            Size{static_cast<uint32_t>(impl_->width), static_cast<uint32_t>(impl_->height)},
            pixel_ratio_,
            gfx::HeadlessBackend::SwapBehaviour::NoFlush,
            gfx::ContextMode::Unique,
            std::nullopt,
            true);

        impl_->map = std::make_unique<Map>(
            *impl_->frontend,
            *impl_->observer,
            MapOptions()
                .withMapMode(MapMode::Continuous)
                .withSize(impl_->frontend->getSize())
                .withPixelRatio(pixel_ratio_),
            resource_options,
            client_options);
    }

#ifdef MAPLIBRE_EMBED_GPU_SURFACE
    // Probe the zero-copy GPU surface path while the GL context is current. We
    // commit to GPU mode only if the full capability check passes (ANGLE/D3D11
    // device query + shared texture + EGL pbuffer + rendering into it), because
    // in GPU mode there is no pixel-buffer fallback texture: a later publish
    // failure would leave the map blank. SelfTest is independent of mbgl's
    // render state (no style is loaded yet at this point). On any failure we
    // keep the proven CPU pixel-buffer path.
    // Diagnostic gate: MAPLIBRE_GPU_DISABLE=1 skips the GPU probe entirely so the
    // map runs on the CPU pixel-buffer path (no D3D texture / EGL pbuffer
    // created), to isolate mbgl-on-ANGLE crashes from the GPU surface probe.
    bool gpu_disabled = false;
    {
        const char* value = getenv("MAPLIBRE_GPU_DISABLE");
        gpu_disabled = (value != nullptr && value[0] == '1');
    }
    if (on_gpu_frame_ && !gpu_disabled) {
        gfx::BackendScope guard{*impl_->frontend->getBackend()};
        const auto size = impl_->frontend->getSize();
        const int physical_w = static_cast<int>(static_cast<uint32_t>(size.width * pixel_ratio_));
        const int physical_h = static_cast<int>(static_cast<uint32_t>(size.height * pixel_ratio_));
        auto gpu_surface = std::make_unique<GpuSurface>();
#if defined(__APPLE__)
        if (GpuSurfaceInitializeFromHeadlessFrontend(
                gpu_surface.get(), impl_->frontend.get(), physical_w, physical_h)) {
#else
        if (gpu_surface->Initialize(nullptr) && gpu_surface->SelfTest(physical_w, physical_h)) {
#endif
            impl_->gpu_surface = std::move(gpu_surface);
            impl_->gpu_active.store(true);
        }
    }
#endif

    {
        std::lock_guard<std::mutex> lock(ready_mutex_);
        thread_ready_ = true;
    }
    ready_cv_.notify_all();

    impl_->run_loop->run();

    // ~MapEmbedder invokes ShutdownOnRunLoop on this thread before run() returns.
    // Drain libuv so ~RunLoop can close the loop without asserting.
    if (impl_->run_loop) {
        impl_->run_loop->waitForEmpty();
        for (int i = 0; i < 128; ++i) {
            impl_->run_loop->runOnce();
        }
    }
    impl_->run_loop.reset();
}

void MapEmbedder::InvokeSync(const std::function<void()>& fn) const {
    if (!impl_->run_loop) {
        return;
    }
#if defined(__APPLE__)
    if (thread_started_ && pthread_equal(pthread_self(), thread_)) {
        fn();
        return;
    }
#else
    if (std::this_thread::get_id() == thread_.get_id()) {
        fn();
        return;
    }
#endif

    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    impl_->run_loop->invoke([&] {
        fn();
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        cv.notify_one();
    });
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return done; });
}

void MapEmbedder::InvokeAsync(std::function<void()> fn) const {
    if (!impl_->run_loop) {
        return;
    }
    impl_->run_loop->invoke([fn = std::move(fn)] { fn(); });
}

template <typename T>
T MapEmbedder::InvokeSyncValue(const std::function<T()>& fn) const {
    T value{};
    InvokeSync([&] { value = fn(); });
    return value;
}

void MapEmbedder::RequestRenderQuick() {
    if (!impl_->map || !impl_->frontend || impl_->shutting_down.load()) {
        MAP_TRACE("RequestRenderQuick: skip (no map/frontend/shutting_down)");
        return;
    }
    if (impl_->finishing_gesture.load()) {
        MAP_TRACE("RequestRenderQuick: skip (finishing_gesture)");
        return;
    }
    if (impl_->pan_snapshot_active.load() && impl_->pointer_down) {
        MAP_TRACE("RequestRenderQuick: skip (pan_snapshot_active && pointer_down)");
        return;
    }
#if defined(__APPLE__)
    // Tile-loading repaints can schedule hundreds of quick renders per second in
    // GPU mode while each publish waits on Metal. Throttle idle repaints to ~30 Hz.
    if (impl_->gpu_active.load() && !impl_->interacting && !impl_->scroll_gesture_frozen &&
        !impl_->animating.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (impl_->last_gpu_idle_quick_render.time_since_epoch().count() != 0 &&
            now - impl_->last_gpu_idle_quick_render < kGpuIdleQuickRenderMinInterval) {
            MAP_TRACE("RequestRenderQuick: skip (gpu idle throttle)");
            return;
        }
        impl_->last_gpu_idle_quick_render = now;
    }
#endif
    // Coalesce nested/re-entrant calls (e.g. onDidFinishRenderingFrame scheduling
    // another quick render while PumpRenderFrames is still running).
    if (impl_->pumping_render.exchange(true)) {
        MAP_TRACE("RequestRenderQuick: skip (pumping_render already true)");
        return;
    }

    gfx::BackendScope guard{*impl_->frontend->getBackend()};
    const int max_frames =
        (impl_->interacting || impl_->scroll_gesture_frozen) ? 1 : kQuickRenderPumpFrames;
    PumpRenderFrames(impl_->frontend.get(), impl_->map.get(), &impl_->needs_repaint, max_frames);
    PublishFrame();
    impl_->pumping_render.store(false);
}

void MapEmbedder::RequestRenderUntilIdle() {
    if (!impl_->map || !impl_->frontend || impl_->shutting_down.load()) {
        return;
    }
    impl_->cancel_idle_pump.store(false);
    gfx::BackendScope guard{*impl_->frontend->getBackend()};
    impl_->pumping_render.store(true);
    PumpRenderFrames(impl_->frontend.get(),
                     impl_->map.get(),
                     &impl_->needs_repaint,
                     kMaxRenderPumpFrames,
                     &impl_->shutting_down,
                     &impl_->cancel_idle_pump);
    impl_->pumping_render.store(false);
    PublishFrame();
}

void MapEmbedder::RequestRender() {
    RequestRenderQuick();
}

bool MapEmbedder::IsGpuMode() const {
    return impl_->gpu_active.load();
}

bool MapEmbedder::GetGpuFrameSync(void** producer_event, void** consumer_event, uint64_t* producer_value) const {
    if (!producer_event || !consumer_event || !producer_value) {
        return false;
    }
    *producer_event = nullptr;
    *consumer_event = nullptr;
    *producer_value = 0;

#ifdef MAPLIBRE_EMBED_GPU_SURFACE
    if (!impl_->gpu_active.load() || !impl_->gpu_surface) {
        return false;
    }
    *producer_event = impl_->gpu_surface->producer_event();
    *consumer_event = impl_->gpu_surface->consumer_event();
    *producer_value = impl_->gpu_surface->producer_signaled_value();
    return *producer_event != nullptr && *consumer_event != nullptr && *producer_value != 0;
#else
    return false;
#endif
}

void MapEmbedder::PublishFrame() {
    if (!impl_->frontend) {
        return;
    }
#ifndef NDEBUG
    {
        static std::atomic<int> s_publish_count{0};
        const int n = ++s_publish_count;
        if (getenv("MYURA_TRACE")) {
            fprintf(stderr, "[MapEmbed] PublishFrame #%d interacting=%d frozen=%d animating=%d\n",
                    n, (int)impl_->interacting, (int)impl_->scroll_gesture_frozen,
                    (int)impl_->animating.load());
        }
    }
#endif


#ifdef MAPLIBRE_EMBED_GPU_SURFACE
    // Diagnostic gate: MAPLIBRE_GPU_NO_PRODUCER=1 lets mbgl render but skips all
    // publish/readback work, to isolate mbgl-on-ANGLE crashes from the blit path.
    {
        static const bool skip_publish = [] {
            const char* value = getenv("MAPLIBRE_GPU_NO_PRODUCER");
            return (value != nullptr && value[0] == '1');
        }();
        if (skip_publish) {
            return;
        }
    }

    // Zero-copy path: blit mbgl's framebuffer into the shared D3D11 texture and
    // hand Flutter the shared handle. No glReadPixels / CPU upload.
    if (impl_->gpu_active.load() && impl_->gpu_surface && on_gpu_frame_) {
        const auto size = impl_->frontend->getSize();
        // The backend framebuffer is physical (logical * pixelRatio); match
        // mbgl's own truncating cast so the blit covers the exact extent.
        const int physical_w = static_cast<int>(static_cast<uint32_t>(size.width * pixel_ratio_));
        const int physical_h = static_cast<int>(static_cast<uint32_t>(size.height * pixel_ratio_));
        if (physical_w > 0 && physical_h > 0) {
#if defined(__APPLE__)
            if (GpuSurfacePublishFromHeadlessFrontend(
                    impl_->gpu_surface.get(), physical_w, physical_h, impl_->frontend.get())) {
                const auto surface_id = reinterpret_cast<uintptr_t>(impl_->gpu_surface->shared_handle());
                const auto timeline = impl_->gpu_surface->producer_signaled_value();
#ifndef NDEBUG
                {
                    static std::atomic<int> s_gpu_cb_count{0};
                    const int n = ++s_gpu_cb_count;
                    if (n == 1 || getenv("MYURA_GPU_TRACE")) {
                        fprintf(stderr,
                                "[MapEmbed] GpuFrameCallback #%d %dx%d iosurface=%u timeline=%llu\n",
                                n,
                                physical_w,
                                physical_h,
                                static_cast<unsigned>(surface_id),
                                static_cast<unsigned long long>(timeline));
                    }
                }
#endif
                on_gpu_frame_(impl_->gpu_surface->shared_handle(),
                              physical_w,
                              physical_h,
                              impl_->gpu_surface->producer_event(),
                              impl_->gpu_surface->consumer_event(),
                              impl_->gpu_surface->producer_signaled_value());
                return;
            }
#else
            if (impl_->gpu_surface->Publish(physical_w, physical_h)) {
                on_gpu_frame_(impl_->gpu_surface->shared_handle(), physical_w, physical_h, nullptr, nullptr, 0);
                return;
            }
#endif
        }
#if defined(__APPLE__)
        // GPU mode is active but publish was skipped (e.g. GPU queue backlogged).
        // Do not fall through to glReadPixels — that stalls the map thread and
        // freezes the UI while .NET ignores CPU frames anyway.
        return;
#endif
        // Publish failed this frame; fall through to the CPU path below.
    }
#endif

    if (!on_pixels_) {
        return;
    }
    PremultipliedImage image;
    for (int attempt = 0; attempt < 3 && !image.valid(); ++attempt) {
        if (attempt > 0) {
            PumpRenderFrames(impl_->frontend.get(), impl_->map.get(), &impl_->needs_repaint, 1);
        }
        image = impl_->frontend->readStillImage();
    }
    if (!image.valid()) {
        return;
    }
    PublishPixels(image.data.get(),
                  static_cast<size_t>(image.size.width),
                  static_cast<size_t>(image.size.height));
}

void MapEmbedder::PublishPixels(const uint8_t* data, size_t width, size_t height) {
    if (!on_pixels_ || !data || width == 0 || height == 0) {
        return;
    }
    // Retain the last full frame so pan capture can memcpy instead of readStillImage.
    if (!IsGpuMode() && !impl_->pan_snapshot_active.load()) {
        const size_t byte_count = width * height * 4;
        if (impl_->last_published_pixels.size() != byte_count) {
            impl_->last_published_pixels.resize(byte_count);
        }
        std::memcpy(impl_->last_published_pixels.data(), data, byte_count);
        impl_->last_published_width = width;
        impl_->last_published_height = height;
    }
    on_pixels_(data, width, height);
}

void MapEmbedder::CapturePanSnapshot(double x, double y) {
    impl_->pan_snapshot_origin_x = x;
    impl_->pan_snapshot_origin_y = y;
    impl_->pan_snapshot_active.store(false);

    if (!impl_->frontend || !impl_->map) {
        return;
    }

    const auto size = impl_->frontend->getSize();
    const auto expected_w =
        static_cast<size_t>(static_cast<uint32_t>(size.width * pixel_ratio_));
    const auto expected_h =
        static_cast<size_t>(static_cast<uint32_t>(size.height * pixel_ratio_));

    if (!impl_->last_published_pixels.empty() && impl_->last_published_width == expected_w &&
        impl_->last_published_height == expected_h) {
        const size_t byte_count = expected_w * expected_h * 4;
        if (impl_->pan_snapshot_pixels.size() != byte_count) {
            impl_->pan_snapshot_pixels.resize(byte_count);
        }
        std::memcpy(impl_->pan_snapshot_pixels.data(),
                    impl_->last_published_pixels.data(),
                    byte_count);
        impl_->pan_snapshot_width = expected_w;
        impl_->pan_snapshot_height = expected_h;
        impl_->pan_snapshot_active.store(true);
        impl_->pan_frame_dirty = true;
        return;
    }

    gfx::BackendScope guard{*impl_->frontend->getBackend()};
    PremultipliedImage image = impl_->frontend->readStillImage();
    if (!image.valid() || impl_->needs_repaint.load()) {
        PumpRenderFrames(impl_->frontend.get(), impl_->map.get(), &impl_->needs_repaint, 1);
        image = impl_->frontend->readStillImage();
    }
    if (!image.valid()) {
        return;
    }

    // Keep the full physical readback so pan offsets (in logical/DIP space)
    // can be scaled by pixel_ratio in PublishPanSnapshot. Downscaling to
    // logical size here caused 2x pan speed and end-of-gesture jumps on Retina.
    impl_->pan_snapshot_width = static_cast<size_t>(image.size.width);
    impl_->pan_snapshot_height = static_cast<size_t>(image.size.height);
    impl_->pan_snapshot_pixels.resize(impl_->pan_snapshot_width * impl_->pan_snapshot_height * 4);
    std::memcpy(impl_->pan_snapshot_pixels.data(), image.data.get(), impl_->pan_snapshot_pixels.size());
    impl_->pan_snapshot_active.store(true);
    impl_->pan_frame_dirty = true;
}

void MapEmbedder::ClearPanSnapshot(bool trigger_repaint) {
    impl_->pan_snapshot_active.store(false);
    impl_->pan_snapshot_pixels.clear();
    impl_->pan_snapshot_width = 0;
    impl_->pan_snapshot_height = 0;
    impl_->pan_frame_dirty = false;
    impl_->has_pending_pan_move = false;
    if (trigger_repaint && impl_->map) {
        impl_->map->triggerRepaint();
    }
}

void MapEmbedder::ReleasePanSnapshot() {
    ClearPanSnapshot(true);
}

void MapEmbedder::PublishPanSnapshot(double x, double y) {
    if (impl_->finishing_gesture.load() || !impl_->pan_snapshot_active.load() ||
        impl_->pan_snapshot_pixels.empty()) {
        return;
    }

    const double scale = static_cast<double>(pixel_ratio_);
    const int offset_x =
        static_cast<int>(std::lround((x - impl_->pan_snapshot_origin_x) * scale));
    const int offset_y =
        static_cast<int>(std::lround((y - impl_->pan_snapshot_origin_y) * scale));
    BlitPanSnapshot(impl_->pan_snapshot_pixels.data(),
                    impl_->pan_snapshot_width,
                    impl_->pan_snapshot_height,
                    offset_x,
                    offset_y,
                    impl_->composed_pixels);
    PublishPixels(impl_->composed_pixels.data(), impl_->pan_snapshot_width, impl_->pan_snapshot_height);
    impl_->pan_frame_dirty = false;
}

void MapEmbedder::SyncGestureFreezeState() {
    if (!impl_->map) {
        return;
    }

    // Suppress cameraIdle churn for both pointer gestures and wheel-zoom bursts.
    const bool should_suppress_idle = impl_->interacting || impl_->scroll_gesture_frozen;
    impl_->suppress_idle_events.store(should_suppress_idle);

    // Only pointer pan/pitch/rotate use MapLibre's transform-only gesture freeze.
    // Wheel zoom changes the ideal tile set; freezing tiles during scroll left
    // stale geometry matrix-transformed at the new zoom (ghost features shifted
    // north, permanent at world scale). Scroll still coalesces via frame tick.
    const bool should_freeze_map = impl_->interacting;
    if (impl_->gesture_frozen == should_freeze_map) {
        return;
    }
    impl_->gesture_frozen = should_freeze_map;

    if (should_freeze_map) {
        impl_->saved_prefetch_zoom_delta = impl_->map->getPrefetchZoomDelta();
        // Keep at least one parent zoom level during gestures so labels/tiles do
        // not pop in and out (prefetch=0 caused visible symbol flicker on pan).
        impl_->map->setPrefetchZoomDelta(std::max<uint8_t>(1, impl_->saved_prefetch_zoom_delta));
        impl_->saved_tile_lod_zoom_shift = impl_->map->getTileLodZoomShift();
        impl_->map->setGestureInProgress(true);
    } else {
        impl_->map->setPrefetchZoomDelta(impl_->saved_prefetch_zoom_delta);
        impl_->map->setTileLodZoomShift(impl_->saved_tile_lod_zoom_shift);
        impl_->map->setGestureInProgress(false);
        impl_->map->triggerRepaint();
    }
}

void MapEmbedder::SetInteracting(bool active) {
    if (impl_->interacting == active) {
        return;
    }
    impl_->interacting = active;
    SyncGestureFreezeState();
    if (impl_->interacting || impl_->scroll_gesture_frozen) {
        RestartFrameTick();
    } else if (!impl_->animating.load()) {
        StopFrameTick();
    }
}

void MapEmbedder::ExtendScrollGestureFreeze() {
    MAP_TRACE("ExtendScrollGestureFreeze: enter");
    impl_->cancel_idle_pump.store(true);
    impl_->scroll_gesture_frozen = true;

    SyncGestureFreezeState();

    if (!impl_->scroll_freeze_timer) {
        impl_->scroll_freeze_timer = std::make_unique<util::Timer>();
    }
    impl_->scroll_freeze_timer->stop();
    impl_->scroll_freeze_timer->start(Milliseconds(kScrollGestureDebounceMs), Duration::zero(), [this] {
        ApplyPendingScrollZoom();
        impl_->scroll_gesture_frozen = false;
        SyncGestureFreezeState();
        if (!impl_->interacting && !impl_->animating.load()) {
            StopFrameTick();
            if (!impl_->frontend || !impl_->map) {
                return;
            }
            // Non-blocking follow-up: finish loading tiles without monopolizing the
            // run loop (RequestRenderUntilIdle blocked new wheel events).
            gfx::BackendScope guard{*impl_->frontend->getBackend()};
            impl_->pumping_render.store(true);
            PumpRenderFrames(impl_->frontend.get(),
                             impl_->map.get(),
                             &impl_->needs_repaint,
                             kScrollFollowUpRenderFrames);
            impl_->pumping_render.store(false);
            PublishFrame();
        }
    });
    RestartFrameTick();
}

bool MapEmbedder::ApplyPendingScrollZoom() {
    if (!impl_->map) {
        return false;
    }

    double x = 0;
    double y = 0;
    double scroll_delta = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->input_mutex);
        if (!impl_->has_pending_scroll || impl_->pending_scroll_delta == 0.0) {
            return false;
        }
        x = impl_->pending_scroll_x;
        y = impl_->pending_scroll_y;
        scroll_delta = impl_->pending_scroll_delta;
        impl_->pending_scroll_delta = 0;
        impl_->has_pending_scroll = false;
    }

    const double wheel_sign = impl_->invert_wheel_zoom ? 1.0 : -1.0;
    double delta = wheel_sign * scroll_delta * 40.0;
    const double abs_delta = std::abs(delta);
    double scale = 2.0 / (1.0 + std::exp(-abs_delta / 100.0));
    const bool is_wheel = delta != 0.0 && std::fmod(abs_delta, 4.000244140625) == 0.0;
    if (!is_wheel) {
        scale = (scale - 1.0) / 2.0 + 1.0;
    }
    if (delta < 0.0 && scale != 0.0) {
        scale = 1.0 / scale;
    }
    impl_->map->scaleBy(scale, ScreenCoordinate{x, y});
    return true;
}

void MapEmbedder::RestartFrameTick() {
    if (!impl_->run_loop) {
        return;
    }
    if (!impl_->frame_tick) {
        impl_->frame_tick = std::make_unique<util::Timer>();
    }
    impl_->frame_tick->stop();
    const auto interval = Milliseconds(1000 / kInteractionFps);
    impl_->frame_tick->start(Duration::zero(), interval, [this] { ProcessFrameTick(); });
}

void MapEmbedder::StopFrameTick() {
    if (impl_->frame_tick) {
        impl_->frame_tick->stop();
    }
}

void MapEmbedder::SchedulePanPublish() {
    if (!impl_->run_loop || impl_->finishing_gesture.load()) {
        return;
    }
    if (impl_->pan_publish_scheduled.exchange(true)) {
        return;
    }
    impl_->run_loop->invoke([this] {
        impl_->pan_publish_scheduled.store(false);
        ProcessPendingPanFrame();
    });
}

void MapEmbedder::SchedulePointerInput() {
    if (!impl_->run_loop || impl_->finishing_gesture.load()) {
        return;
    }
    if (impl_->pointer_input_scheduled.exchange(true)) {
        return;
    }
    impl_->run_loop->invoke([this] {
        impl_->pointer_input_scheduled.store(false);
        ProcessPendingPointerInput();
    });
}

void MapEmbedder::ProcessPendingPointerInput() {
    if (!impl_->map || impl_->finishing_gesture.load()) {
        return;
    }

    double move_x = 0;
    double move_y = 0;
    bool move_shift = false;
    bool move_control = false;
    bool do_move = false;

    {
        std::lock_guard<std::mutex> lock(impl_->input_mutex);
        if (impl_->has_pending_pan_move && impl_->pointer_down) {
            move_x = impl_->pending_pan_x;
            move_y = impl_->pending_pan_y;
            move_shift = impl_->pointer_shift;
            move_control = impl_->pointer_control;
            impl_->has_pending_pan_move = false;
            do_move = true;
        }
    }

    if (do_move) {
        ProcessPointerOnLoop("move", move_x, move_y, 0, move_shift, move_control);
    }

    {
        std::lock_guard<std::mutex> lock(impl_->input_mutex);
        if (impl_->has_pending_pan_move && impl_->pointer_down) {
            SchedulePointerInput();
        }
    }
}

void MapEmbedder::ProcessPendingPanFrame() {
    if (!impl_->map || impl_->finishing_gesture.load() || !impl_->pointer_down ||
        impl_->gesture_mode != GestureMode::Pan || !impl_->pan_snapshot_active.load()) {
        return;
    }

    double x = 0;
    double y = 0;
    bool has_move = false;
    {
        std::lock_guard<std::mutex> lock(impl_->input_mutex);
        if (!impl_->has_pending_pan_move) {
            if (impl_->pan_frame_dirty) {
                x = impl_->last_x;
                y = impl_->last_y;
                has_move = true;
            }
        } else {
            x = impl_->pending_pan_x;
            y = impl_->pending_pan_y;
            impl_->has_pending_pan_move = false;
            has_move = true;
        }
    }

    if (!has_move) {
        return;
    }

    const double dx = x - impl_->embedder_pan_x;
    const double dy = y - impl_->embedder_pan_y;
    impl_->embedder_pan_x = x;
    impl_->embedder_pan_y = y;

    if (dx != 0.0 || dy != 0.0) {
        impl_->map->moveBy(ScreenCoordinate{dx, dy});
    }
    PublishPanSnapshot(x, y);

    {
        std::lock_guard<std::mutex> lock(impl_->input_mutex);
        if (impl_->has_pending_pan_move && impl_->pointer_down) {
            SchedulePanPublish();
        }
    }
}

void MapEmbedder::FinishPanGesture() {
    ProcessPendingPanFrame();
    {
        std::lock_guard<std::mutex> lock(impl_->input_mutex);
        impl_->pointer_down = false;
        impl_->has_pending_pan_move = false;
    }
    impl_->gesture_mode = GestureMode::None;

    // finishing_gesture suppresses in-flight snapshot publishes (SchedulePanPublish /
    // ProcessPendingPanFrame) while we tear down the pan snapshot. It must be reset
    // even if ClearPanSnapshot/SetInteracting throw, otherwise every later
    // RequestRenderQuick/SchedulePanPublish/SchedulePointerInput would early-return
    // and the map would go permanently unresponsive to all input. Use an RAII guard.
    struct FinishingGuard {
        std::atomic<bool>& flag;
        explicit FinishingGuard(std::atomic<bool>& f) : flag(f) { flag.store(true); }
        ~FinishingGuard() { flag.store(false); }
    };
    {
        FinishingGuard guard(impl_->finishing_gesture);
        ClearPanSnapshot(false);
        SetInteracting(false);
    }

    // The map was already moved (moveBy) during the gesture but only the stale
    // pan snapshot was shown. Now that finishing_gesture is cleared, render the
    // real map so the final position is actually drawn (previously this
    // RequestRenderQuick ran while finishing_gesture was still true and was
    // silently skipped, leaving the display stuck on the last snapshot).
    RequestRenderQuick();
}


void MapEmbedder::ProcessFrameTick() {
    MAP_TRACE("ProcessFrameTick: enter frozen=%d interacting=%d pdown=%d snap=%d anim=%d",
              (int)impl_->scroll_gesture_frozen, (int)impl_->interacting, (int)impl_->pointer_down,
              (int)impl_->pan_snapshot_active.load(), (int)impl_->animating.load());
    if (impl_->shutting_down.load()) {
        return;
    }


    if (impl_->pan_snapshot_active.load() && impl_->pointer_down &&
        impl_->gesture_mode == GestureMode::Pan) {
        ProcessPendingPanFrame();
        return;
    }

    if (impl_->scroll_gesture_frozen) {
        ApplyPendingScrollZoom();
        RequestRenderQuick();
        return;
    }

    // Active pointer gesture (GPU direct-pan / pitch / rotate): publish at the
    // capped interaction rate instead of once per queued move event.
    if (impl_->interacting && impl_->pointer_down) {
        RequestRenderQuick();
        return;
    }

    if (impl_->animating.load()) {
        RequestRenderQuick();
        if (!impl_->needs_repaint.load()) {
            impl_->animating.store(false);
        }
    }

    if (!impl_->interacting && !impl_->scroll_gesture_frozen && !impl_->animating.load()) {
        StopFrameTick();
    }
}

void MapEmbedder::Resize(int width, int height) {
    InvokeSync([this, width, height] {
        ReleasePanSnapshot();
        impl_->last_published_pixels.clear();
        impl_->last_published_width = 0;
        impl_->last_published_height = 0;
        impl_->width = width;
        impl_->height = height;
        impl_->frontend->setSize(Size{static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
        impl_->map->setSize(impl_->frontend->getSize());
        RequestRenderQuick();
    });
}

std::string MapEmbedder::NormalizeStyleUrl(std::string style) const {
    if (style.empty()) {
        const auto styles = TileServerOptions::MapTilerConfiguration().defaultStyles();
        if (!styles.empty()) {
            return styles.front().getUrl();
        }
        return style;
    }
    if (style.find("://") == std::string::npos && style.front() != '{' && style.front() != '[') {
        if (style.find("http") == 0) {
            return style;
        }
        return "file://" + style;
    }
    return style;
}

void MapEmbedder::OnStyleFullyLoaded() {
    if (!impl_->map) {
        return;
    }

    if (impl_->pending_initial_camera && !impl_->initial_camera_applied) {
        impl_->map->jumpTo(*impl_->pending_initial_camera);
        impl_->initial_camera_applied = true;
        impl_->pending_initial_camera.reset();
    } else if (impl_->camera_to_restore_after_style) {
        impl_->map->jumpTo(*impl_->camera_to_restore_after_style);
        impl_->camera_to_restore_after_style.reset();
    }
}

void MapEmbedder::SetStyle(std::string style) {
    InvokeAsync([this, style = std::move(style)] {
        if (!impl_->map) {
            return;
        }
        ReleasePanSnapshot();
        impl_->camera_to_restore_after_style = impl_->map->getCameraOptions();
        const auto normalized = NormalizeStyleUrl(style);
        if (!normalized.empty() && (normalized.front() == '{' || normalized.front() == '[')) {
            impl_->map->getStyle().loadJSON(normalized);
        } else {
            impl_->map->getStyle().loadURL(normalized);
        }
    });
}

void MapEmbedder::MoveCamera(const CameraState& camera) {
    // Async: don't block the Flutter platform thread while the map re-renders
    // (which may wait on tile loads). The new camera is reported via cameraIdle.
    InvokeAsync([this, camera] {
        if (!impl_->map) {
            return;
        }
        impl_->animating.store(false);
        ReleasePanSnapshot();
        auto options = impl_->map->getCameraOptions();
        options = options.withCenter(LatLng{camera.lat, camera.lon})
                     .withZoom(camera.zoom)
                     .withBearing(camera.bearing)
                     .withPitch(camera.pitch);
        impl_->map->jumpTo(options);
        RequestRenderUntilIdle();
    });
}

void MapEmbedder::AnimateCamera(const CameraState& camera, int duration_ms) {
    // Async + frame-tick driven: publishes every animation frame without blocking
    // the Flutter platform thread (the previous synchronous pump froze the UI for
    // the whole animation and only showed the final frame).
    InvokeAsync([this, camera, duration_ms] {
        if (!impl_->map) {
            return;
        }
        ReleasePanSnapshot();
        auto options = impl_->map->getCameraOptions();
        options = options.withCenter(LatLng{camera.lat, camera.lon})
                     .withZoom(camera.zoom)
                     .withBearing(camera.bearing)
                     .withPitch(camera.pitch);
        impl_->map->easeTo(options,
                           AnimationOptions{std::chrono::milliseconds(duration_ms)});
        impl_->animating.store(true);
        RestartFrameTick();
    });
}

CameraState MapEmbedder::GetCamera() const {
    return InvokeSyncValue<CameraState>([&] {
        const auto options = impl_->map->getCameraOptions();
        CameraState state;
        if (options.center) {
            state.lat = options.center->latitude();
            state.lon = options.center->longitude();
        }
        if (options.zoom) state.zoom = *options.zoom;
        if (options.bearing) state.bearing = *options.bearing;
        if (options.pitch) state.pitch = *options.pitch;
        return state;
    });
}

std::pair<double, double> MapEmbedder::ToLngLat(double x, double y) const {
    return InvokeSyncValue<std::pair<double, double>>([&] {
        const auto coord = impl_->frontend->latLngForPixel(ScreenCoordinate{x, y});
        return std::pair<double, double>{coord.longitude(), coord.latitude()};
    });
}

std::pair<double, double> MapEmbedder::ToScreenLocation(double lon, double lat) const {
    return InvokeSyncValue<std::pair<double, double>>([&] {
        const auto pixel = impl_->frontend->pixelForLatLng(LatLng{lat, lon});
        return std::pair<double, double>{pixel.x, pixel.y};
    });
}

void MapEmbedder::SetDragPanEnabled(bool enabled) {
    InvokeSync([&] { impl_->drag_pan_enabled = enabled; });
}

void MapEmbedder::SetInvertWheelZoom(bool invert) {
    InvokeSync([&] { impl_->invert_wheel_zoom = invert; });
}

void MapEmbedder::ProcessPointerOnLoop(const std::string& phase,
                                       double x,
                                       double y,
                                       double scroll_delta,
                                       bool shift,
                                       bool control) {
    if (!impl_->map) {
        return;
    }

    if (phase == "scroll") {
        return;
    }

    if (!impl_->drag_pan_enabled) {
        return;
    }

    if (phase == "down") {
        impl_->cancel_idle_pump.store(true);
        bool start_pan = false;
        bool start_pitch = false;
        bool start_rotate = false;
        {
            std::lock_guard<std::mutex> lock(impl_->input_mutex);
            impl_->pointer_down = true;
            impl_->last_x = x;
            impl_->last_y = y;
            impl_->pointer_shift = shift;
            impl_->pointer_control = control;
            impl_->has_pending_pan_move = false;
            impl_->has_pending_scroll = false;
            if (shift) {
                impl_->gesture_mode = GestureMode::Pitch;
                start_pitch = true;
            } else if (control) {
                impl_->gesture_mode = GestureMode::Rotate;
                start_rotate = true;
            } else {
                impl_->gesture_mode = GestureMode::Pan;
                start_pan = true;
            }
        }
        impl_->animating.store(false);
        ReleasePanSnapshot();
        if (start_pitch || start_rotate) {
            SetInteracting(true);
        } else if (start_pan) {
            // CPU path: capture once, then blit the snapshot during drag (no Metal
            // render/readback per move). Pointer coords are DIP; snapshot pixels are
            // physical (logical * pixelRatio). PublishPanSnapshot scales offsets.
            if (!IsGpuMode()) {
                CapturePanSnapshot(x, y);
            }
            impl_->embedder_pan_x = x;
            impl_->embedder_pan_y = y;
            SetInteracting(true);
            if (impl_->pan_snapshot_active.load()) {
                PublishPanSnapshot(x, y);
            }
        }
        return;
    }

    if (phase == "move" && impl_->pointer_down) {
        const double dx = x - impl_->last_x;
        const double dy = y - impl_->last_y;
        impl_->last_x = x;
        impl_->last_y = y;
        if (dx == 0.0 && dy == 0.0) {
            return;
        }

        if (impl_->gesture_mode == GestureMode::Pitch) {
            impl_->map->pitchBy(dy / 2.0);
        } else if (impl_->gesture_mode == GestureMode::Rotate) {
            impl_->map->rotateBy(ScreenCoordinate{x - dx, y - dy}, ScreenCoordinate{x, y});
        } else if (impl_->pan_snapshot_active.load()) {
            impl_->pan_frame_dirty = true;
            SchedulePanPublish();
        } else {
            impl_->map->moveBy(ScreenCoordinate{dx, dy});
            RequestRenderQuick();
        }
        return;
    }

    if (phase == "up" || phase == "cancel") {
        {
            std::lock_guard<std::mutex> lock(impl_->input_mutex);
            impl_->has_pending_pan_move = false;
            impl_->has_pending_scroll = false;
            impl_->pointer_down = false;
        }
        if (impl_->gesture_mode == GestureMode::Pan && impl_->pan_snapshot_active.load()) {
            FinishPanGesture();
            return;
        }
        impl_->gesture_mode = GestureMode::None;
        SetInteracting(false);
        RequestRenderQuick();
    }
}

void MapEmbedder::OnPointer(const std::string& phase,
                            double x,
                            double y,
                            double scroll_delta,
                            bool shift,
                            bool control) {
    MAP_TRACE("OnPointer: phase=%s x=%.0f y=%.0f delta=%.2f", phase.c_str(), x, y, scroll_delta);
    if (phase == "down") {

        // pointer_down must be set before any co-located "move" events are accepted.
        {
            std::lock_guard<std::mutex> lock(impl_->input_mutex);
            impl_->pointer_down = true;
            impl_->last_x = x;
            impl_->last_y = y;
            impl_->pointer_shift = shift;
            impl_->pointer_control = control;
            impl_->has_pending_pan_move = false;
            impl_->has_pending_scroll = false;
            if (shift) {
                impl_->gesture_mode = GestureMode::Pitch;
            } else if (control) {
                impl_->gesture_mode = GestureMode::Rotate;
            } else {
                impl_->gesture_mode = GestureMode::Pan;
            }
        }
        InvokeAsync([this, phase, x, y, scroll_delta, shift, control] {
            ProcessPointerOnLoop(phase, x, y, scroll_delta, shift, control);
        });
        return;
    }

    if (phase == "up" || phase == "cancel") {
        {
            std::lock_guard<std::mutex> lock(impl_->input_mutex);
            impl_->has_pending_pan_move = false;
            impl_->has_pending_scroll = false;
            impl_->pointer_down = false;
        }
        InvokeAsync([this, phase, x, y, scroll_delta, shift, control] {
            ProcessPendingPointerInput();
            ProcessPointerOnLoop(phase, x, y, scroll_delta, shift, control);
        });
        return;
    }

    if (phase == "scroll") {
        {
            std::lock_guard<std::mutex> lock(impl_->input_mutex);
            if (!impl_->has_pending_scroll) {
                impl_->pending_scroll_x = x;
                impl_->pending_scroll_y = y;
            }
            impl_->pending_scroll_delta += scroll_delta;
            impl_->has_pending_scroll = true;
        }
        InvokeAsync([this] {
            impl_->cancel_idle_pump.store(true);
            ExtendScrollGestureFreeze();
        });
        return;
    }

    if (phase == "move") {
        std::lock_guard<std::mutex> lock(impl_->input_mutex);
        if (!impl_->pointer_down || !impl_->drag_pan_enabled) {
            return;
        }
        impl_->pending_pan_x = x;
        impl_->pending_pan_y = y;
        impl_->has_pending_pan_move = true;
        if (impl_->gesture_mode == GestureMode::Pan && impl_->pan_snapshot_active.load()) {
            impl_->last_x = x;
            impl_->last_y = y;
            SchedulePanPublish();
        } else {
            SchedulePointerInput();
        }
        return;
    }
}

void MapEmbedder::AddSource(const std::string& id, const std::string& source_json) {
    InvokeAsync([this, id, source_json] {
        if (!impl_->map) {
            return;
        }
        auto doc = ParseJson(source_json);
        style::conversion::Error error;
        auto source = style::conversion::convert<std::unique_ptr<style::Source>>(doc, error, id);
        if (!source) {
            return;
        }
        impl_->map->getStyle().addSource(std::move(*source));
        RequestRenderQuick();
    });
}

void MapEmbedder::AddLayer(const std::string& layer_json, const std::optional<std::string>& below_layer_id) {
    InvokeAsync([this, layer_json, below_layer_id] {
        if (!impl_->map) {
            return;
        }
        auto doc = ParseJson(layer_json);
        style::conversion::Error error;
        auto layer = style::conversion::convert<std::unique_ptr<style::Layer>>(doc, error);
        if (!layer) {
            return;
        }
        impl_->map->getStyle().addLayer(std::move(*layer), below_layer_id);
        RequestRenderQuick();
    });
}

void MapEmbedder::ApplyStyleLayers(const std::vector<std::string>& remove_ids,
                                   const std::vector<std::string>& layer_json) {
    InvokeAsync([this, remove_ids, layer_json] {
        if (!impl_->map) {
            return;
        }
        auto& style = impl_->map->getStyle();
        for (const auto& id : remove_ids) {
            if (style.getLayer(id)) {
                style.removeLayer(id);
            }
        }
        for (const auto& json : layer_json) {
            auto doc = ParseJson(json);
            style::conversion::Error error;
            auto layer = style::conversion::convert<std::unique_ptr<style::Layer>>(doc, error);
            if (!layer) {
                continue;
            }
            style.addLayer(std::move(*layer), std::nullopt);
        }
        RequestRenderQuick();
    });
}

void MapEmbedder::RemoveLayer(const std::string& id) {
    InvokeAsync([this, id] {
        if (!impl_->map) {
            return;
        }
        impl_->map->getStyle().removeLayer(id);
        RequestRenderQuick();
    });
}

void MapEmbedder::RemoveSource(const std::string& id) {
    InvokeAsync([this, id] {
        if (!impl_->map) {
            return;
        }
        impl_->map->getStyle().removeSource(id);
        RequestRenderQuick();
    });
}

void MapEmbedder::UpdateGeoJsonSource(const std::string& id, const std::string& data) {
    InvokeAsync([this, id, data] {
        if (!impl_->map) {
            return;
        }
        auto* source = impl_->map->getStyle().getSource(id);
        if (!source) return;
        auto* geojson = source->as<style::GeoJSONSource>();
        if (!geojson) return;
        auto doc = ParseJson(data);
        style::conversion::Error error;
        auto geo = style::conversion::convert<GeoJSON>(doc, error);
        if (!geo) return;
        geojson->setGeoJSON(*geo);
        RequestRenderQuick();
    });
}

void MapEmbedder::UpdateLayerFilter(const std::string& id, const std::string& filter_json) {
    InvokeAsync([this, id, filter_json] {
        if (!impl_->map) {
            return;
        }
        auto* layer = impl_->map->getStyle().getLayer(id);
        if (!layer) return;
        auto doc = ParseJson(filter_json);
        style::conversion::Error error;
        auto filter = style::conversion::convert<style::Filter>(doc, error);
        if (!filter) return;
        layer->setFilter(*filter);
        RequestRenderQuick();
    });
}

void MapEmbedder::UpdateVectorSourceTiles(const std::string& id, const std::vector<std::string>& tiles) {
    InvokeAsync([this, id, tiles] {
        if (!impl_->map) {
            return;
        }
        auto* source = impl_->map->getStyle().getSource(id);
        if (!source) return;
        auto* vector = source->as<style::VectorSource>();
        if (!vector) return;
        vector->setTiles(tiles);
        RequestRenderQuick();
    });
}

void MapEmbedder::AddImage(const std::string& id, const std::vector<uint8_t>& png_bytes) {
    InvokeAsync([this, id, png_bytes] {
        if (!impl_->map) {
            return;
        }
        const std::string bytes(png_bytes.begin(), png_bytes.end());
        auto image = decodeImage(bytes);
        if (!image.valid()) return;
        impl_->map->getStyle().addImage(
            std::make_unique<style::Image>(id, std::move(image), 1.0f, id.find("myura-shape-") == 0 || id.find("myura-svg-") == 0));
        RequestRenderQuick();
    });
}

void MapEmbedder::RemoveImage(const std::string& id) {
    InvokeAsync([this, id] {
        if (!impl_->map) {
            return;
        }
        impl_->map->getStyle().removeImage(id);
        RequestRenderQuick();
    });
}

std::vector<std::string> MapEmbedder::GetLayerIds() const {
    return InvokeSyncValue<std::vector<std::string>>([&] {
        std::vector<std::string> ids;
        for (const auto* layer : impl_->map->getStyle().getLayers()) {
            ids.push_back(layer->getID());
        }
        return ids;
    });
}

std::vector<std::string> MapEmbedder::GetAttributions() const {
    return InvokeSyncValue<std::vector<std::string>>([&] {
        std::vector<std::string> attributions;
        for (const auto* source : impl_->map->getStyle().getSources()) {
            (void)source;
        }
        return attributions;
    });
}

std::string MapEmbedder::FeaturesAtPointJson(double x, double y, const std::vector<std::string>& layer_ids) const {
    return InvokeSyncValue<std::string>([&] {
        if (!impl_->frontend->getRenderer()) {
            return "[]";
        }
        RenderedQueryOptions options;
        options.layerIDs = layer_ids;
        const auto features = impl_->frontend->getRenderer()->queryRenderedFeatures(ScreenCoordinate{x, y}, options);
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        writer.StartArray();
        for (const auto& feature : features) {
            writer.StartObject();
            writer.Key("sourceId");
            writer.String(feature.source.c_str());
            if (!feature.sourceLayer.empty()) {
                writer.Key("sourceLayer");
                writer.String(feature.sourceLayer.c_str());
            }
            writer.Key("properties");
            writer.StartObject();
            for (const auto& [key, value] : feature.properties) {
                writer.Key(key.c_str());
                value.match(
                    [&](const std::string& s) { writer.String(s.c_str()); },
                    [&](const bool b) { writer.Bool(b); },
                    [&](const uint64_t u) { writer.Uint64(u); },
                    [&](const int64_t i) { writer.Int64(i); },
                    [&](const double d) { writer.Double(d); },
                    [&](const auto&) { writer.Null(); });
            }
            writer.EndObject();
            writer.EndObject();
        }
        writer.EndArray();
        return buffer.GetString();
    });
}

}  // namespace maplibre_windows
