#pragma once

#include <mbgl/map/map.hpp>
#include <mbgl/map/map_snapshotter.hpp>
#include <mbgl/util/geometry.hpp>
#include <mbgl/util/run_loop.hpp>
#include <mbgl/util/timer.hpp>

#include <utility>
#include <optional>
#include <functional>

#if (defined(MLN_RENDER_BACKEND_OPENGL) || defined(MLN_RENDER_BACKEND_VULKAN) || \
     defined(MLN_RENDER_BACKEND_WEBGPU)) &&                                      \
    !defined(MBGL_LAYER_CUSTOM_DISABLE_ALL)
#define ENABLE_LOCATION_INDICATOR
#endif

#ifdef ENABLE_LOCATION_INDICATOR
#include <mbgl/style/layers/location_indicator_layer.hpp>
#endif

struct GLFWwindow;
class GLFWBackend;
class GLFWRendererFrontend;
class SnapshotObserver;

namespace mbgl {
namespace gfx {
class RendererBackend;
} // namespace gfx
} // namespace mbgl

class GLFWView : public mbgl::MapObserver {
public:
    GLFWView(bool fullscreen,
             bool benchmark,
             const mbgl::ResourceOptions &resourceOptions,
             const mbgl::ClientOptions &clientOptions);
    ~GLFWView() override;

    float getPixelRatio() const;

    void setMap(mbgl::Map *);

    void setRenderFrontend(GLFWRendererFrontend *);

    mbgl::gfx::RendererBackend &getRendererBackend();

    void setTestDirectory(std::string dir) { testDirectory = std::move(dir); };

    // Callback called when the user presses the key mapped to style change.
    // The expected action is to set a new style, different to the current one.
    void setChangeStyleCallback(std::function<void()> callback);

    void setPauseResumeCallback(std::function<void()> callback) { pauseResumeCallback = std::move(callback); };

    void setOnlineStatusCallback(std::function<void()> callback) { onlineStatusCallback = std::move(callback); }

    void setResetCacheCallback(std::function<void()> callback) { resetDatabaseCallback = std::move(callback); };

    void setMaintenanceCallback(std::function<void()> callback) { maintenanceCallback = std::move(callback); }

    void setShouldClose();

    void setWindowTitle(const std::string &);

    void run();

    void invalidate();

    void wake();

    bool isInteracting() const { return interacting; }

    mbgl::Size getSize() const;

    // mbgl::MapObserver implementation
    void onDidFinishLoadingStyle() override;
    void onDidBecomeIdle() override;
    void onWillStartRenderingFrame() override;
    void onDidFinishRenderingFrame(const mbgl::MapObserver::RenderFrameStatus& status) override;

protected:
    // mbgl::Backend implementation

private:
    // Window callbacks
    static void onKey(GLFWwindow *window, int key, int scancode, int action, int mods);
    static void onScroll(GLFWwindow *window, double xoffset, double yoffset);
    static void onWindowResize(GLFWwindow *window, int width, int height);
    static void onFramebufferResize(GLFWwindow *window, int width, int height);
    static void onMouseClick(GLFWwindow *window, int button, int action, int modifiers);
    static void onMouseMove(GLFWwindow *window, double x, double y);
    static void onWindowFocus(GLFWwindow *window, int focused);
    static void onWindowRefresh(GLFWwindow *window);

    // Internal
    void report(float duration);
    void reportBenchmarkStats(const mbgl::gfx::RenderingStats& stats);

    void render();

    void setInteracting(bool mouseGestureActive);
    void extendScrollGestureFreeze();
    void syncGestureFreezeState();
    void capturePanSnapshot(double mouseX, double mouseY);
    void releasePanSnapshot();
    bool renderPanSnapshot();
    void prepareForMapContentChange();
    void clearFeatureHoverState();
    void syncViewportFromWindow();
    void updateHoverFeatureState();
    void restartFrameTick();
    void processEventsAndRender();

    mbgl::Color makeRandomColor() const;
    mbgl::Point<double> makeRandomPoint() const;
    static std::unique_ptr<mbgl::style::Image> makeImage(const std::string &id,
                                                         int width,
                                                         int height,
                                                         float pixelRatio);

    void nextOrientation();

    void addRandomPointAnnotations(int count);
    void addRandomLineAnnotations(int count);
    void addRandomShapeAnnotations(int count);
    void addRandomCustomPointAnnotations(int count);
    void addAnimatedAnnotation();
    void updateFreeCameraDemo();
    void updateAnimatedAnnotations();
    void toggleCustomSource();
    void toggleLocationIndicatorLayer();

    void cycleDebugOptions();
    void clearAnnotations();
    void popAnnotation();

    void toggleCustomDrawableStyle();
    void makeSnapshot(bool withOverlay = false);

    mbgl::AnnotationIDs annotationIDs;
    std::vector<std::string> spriteIDs;

    mbgl::AnnotationIDs animatedAnnotationIDs;
    std::vector<double> animatedAnnotationAddedTimes;

private:
    void toggle3DExtrusions(bool visible);

    mbgl::Map *map = nullptr;
    GLFWRendererFrontend *rendererFrontend = nullptr;
    std::unique_ptr<GLFWBackend> backend;

    std::string testDirectory = ".";

    double freeCameraDemoPhase = -1;
    mbgl::TimePoint freeCameraDemoStartTime;
    bool fullscreen = false;
    const bool benchmark = false;
    bool tracking = false;
    bool rotating = false;
    bool pitching = false;
    bool show3DExtrusions = false;

    // Frame timer
    int frames = 0;
    float frameTime = 0;
    double lastReported = 0;
    int benchmarkStatFrames = 0;
    double benchmarkEncodeTime = 0;
    double benchmarkRenderTime = 0;
    int benchmarkGestureFrames = 0;
    double benchmarkGestureEncodeTime = 0;
    double benchmarkGestureRenderTime = 0;
    int benchmarkNonGestureFrames = 0;
    double benchmarkNonGestureEncodeTime = 0;
    double benchmarkNonGestureRenderTime = 0;
    bool lastFrameWasGesture = false;

    int width = 1024;
    int height = 768;
    float pixelRatio;

    double lastX = 0, lastY = 0;

    double lastClick = -1;

    std::function<void()> changeStyleCallback;
    std::function<void()> pauseResumeCallback;
    std::function<void()> onlineStatusCallback;
    std::function<void()> resetDatabaseCallback;
    std::function<void()> maintenanceCallback;
    std::function<void(mbgl::Map *)> animateRouteCallback;

    mbgl::util::RunLoop runLoop;
    mbgl::util::Timer frameTick;
    mbgl::util::Timer maintenanceTick;
    mbgl::util::Timer scrollFreezeTimer;

    GLFWwindow *window = nullptr;
    bool dirty = false;
    bool interacting = false;
    bool mouseGestureActive = false;
    bool scrollGestureFrozen = false;
    bool panSnapshotActive = false;
    double panSnapshotOriginX = 0;
    double panSnapshotOriginY = 0;
    bool preferVSync = false;
    double lastInteractionTime = 0;
    double lastRenderTime = 0;
    double savedTileLodZoomShift = 0;
    uint8_t savedPrefetchZoomDelta = 1;
    std::optional<std::string> featureID;
    std::unique_ptr<mbgl::MapSnapshotter> snapshotter;
    std::unique_ptr<SnapshotObserver> snapshotterObserver;
    mbgl::ResourceOptions mapResourceOptions;
    mbgl::ClientOptions mapClientOptions;

#ifdef ENABLE_LOCATION_INDICATOR
    bool puckFollowsCameraCenter = false;
    mbgl::style::LocationIndicatorLayer *puck = nullptr;
#endif
};
