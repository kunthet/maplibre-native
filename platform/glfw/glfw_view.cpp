#include "glfw_view.hpp"
#include "glfw_backend.hpp"
#include "glfw_renderer_frontend.hpp"
#include "ny_route.hpp"
#include "test_writer.hpp"

#include <mbgl/annotation/annotation.hpp>
#include <mbgl/gfx/backend.hpp>
#include <mbgl/gfx/backend_scope.hpp>
#include <mbgl/map/camera.hpp>
#include <mbgl/math/angles.hpp>
#include <mbgl/math/clamp.hpp>
#include <mbgl/renderer/renderer.hpp>
#include <mbgl/style/expression/dsl.hpp>
#include <mbgl/style/image.hpp>
#include <mbgl/style/layers/fill_extrusion_layer.hpp>
#include <mbgl/style/layers/fill_layer.hpp>
#include <mbgl/style/layers/line_layer.hpp>
#include <mbgl/style/sources/custom_geometry_source.hpp>
#include <mbgl/style/sources/geojson_source.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/style/transition_options.hpp>
#include <mbgl/util/chrono.hpp>
#include <mbgl/util/geo.hpp>
#include <mbgl/util/interpolate.hpp>
#include <mbgl/util/io.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/instrumentation.hpp>
#include <mbgl/util/platform.hpp>
#include <mbgl/util/string.hpp>

#if !defined(MBGL_LAYER_CUSTOM_DISABLE_ALL)
#include "example_custom_drawable_style_layer.hpp"
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4267)
#endif

#include <mapbox/cheap_ruler.hpp>
#include <mapbox/geometry.hpp>
#include <mapbox/geojson.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#if !defined(__APPLE__)
#define GLFW_INCLUDE_ES3
#endif

#if MLN_RENDER_BACKEND_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif

#define GL_GLEXT_PROTOTYPES
#include <GLFW/glfw3.h>

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <utility>
#include <sstream>
#include <numbers>

using namespace std::numbers;

#ifdef ENABLE_LOCATION_INDICATOR
namespace {
const std::string mbglPuckAssetsPath{MLN_ASSETS_PATH};

mbgl::Color premultiply(mbgl::Color c) {
    c.r *= c.a;
    c.g *= c.a;
    c.b *= c.a;
    return c;
}

std::array<double, 3> toArray(const mbgl::LatLng &crd) {
    return {crd.latitude(), crd.longitude(), 0};
}
} // namespace
#endif // ENABLE_LOCATION_INDICATOR

class SnapshotObserver final : public mbgl::MapSnapshotterObserver {
public:
    ~SnapshotObserver() override = default;
    void onDidFinishLoadingStyle() override {
        MLN_TRACE_FUNC();

        if (didFinishLoadingStyleCallback) {
            didFinishLoadingStyleCallback();
        }
    }
    std::function<void()> didFinishLoadingStyleCallback;
};

namespace {

enum class TileLodProfile {
    Default,       // Default Tile LOD parameters
    NoLod,         // Disable LOD
    Reduced,       // Reduce LOD away from camera
    Aggressive,    // Aggressively reduce LOD away from camera at the detriment of quality
    DistanceBased, // Use distance-based algorithm
};

constexpr TileLodProfile nextTileLodProfile(TileLodProfile current) {
    switch (current) {
        case TileLodProfile::Default:
            return TileLodProfile::NoLod;
        case TileLodProfile::NoLod:
            return TileLodProfile::Reduced;
        case TileLodProfile::Reduced:
            return TileLodProfile::Aggressive;
        case TileLodProfile::Aggressive:
            return TileLodProfile::DistanceBased;
        case TileLodProfile::DistanceBased:
            return TileLodProfile::Default;
        default:
            return TileLodProfile::Default;
    }
}

void cycleTileLodProfile(mbgl::Map &map) {
    // TileLodProfile::Default parameters
    static const auto defaultRadius = map.getTileLodMinRadius();
    static const auto defaultScale = map.getTileLodScale();
    static const auto defaultTilePitchThreshold = map.getTileLodPitchThreshold();
    static const auto defaultTileLodMode = map.getTileLodMode();

    static TileLodProfile profile = TileLodProfile::Default;
    profile = nextTileLodProfile(profile);

    switch (profile) {
        case TileLodProfile::Default:
            map.setTileLodMinRadius(defaultRadius);
            map.setTileLodScale(defaultScale);
            map.setTileLodPitchThreshold(defaultTilePitchThreshold);
            map.setTileLodMode(defaultTileLodMode);
            mbgl::Log::Info(mbgl::Event::General, "Tile LOD profile: default");
            break;
        case TileLodProfile::NoLod:
            // When LOD is off we set a maximum PitchThreshold
            map.setTileLodPitchThreshold(std::numbers::pi);
            mbgl::Log::Info(mbgl::Event::General, "Tile LOD profile: disabled");
            break;
        case TileLodProfile::Reduced:
            map.setTileLodMinRadius(2);
            map.setTileLodScale(1.5);
            map.setTileLodPitchThreshold(std::numbers::pi / 4);
            mbgl::Log::Info(mbgl::Event::General, "Tile LOD profile: reduced");
            break;
        case TileLodProfile::Aggressive:
            map.setTileLodMinRadius(1);
            map.setTileLodScale(2);
            map.setTileLodPitchThreshold(0);
            mbgl::Log::Info(mbgl::Event::General, "Tile LOD profile: aggressive");
            break;
        case TileLodProfile::DistanceBased:
            map.setTileLodScale(1);
            map.setTileLodPitchThreshold(0);
            map.setTileLodMode(mbgl::TileLodMode::Distance);
            mbgl::Log::Info(mbgl::Event::General, "Tile LOD profile: distance-based");
            break;
    }
    map.triggerRepaint();
}

void tileLodZoomShift(mbgl::Map &map, bool positive) {
    constexpr auto tileLodZoomShiftStep = 0.25;
    auto shift = positive ? tileLodZoomShiftStep : -tileLodZoomShiftStep;
    shift = map.getTileLodZoomShift() + shift;
    shift = mbgl::util::clamp(shift, -2.5, 2.5);
    mbgl::Log::Info(mbgl::Event::OpenGL, "Zoom shift: " + std::to_string(shift));
    map.setTileLodZoomShift(shift);
    map.triggerRepaint();
}

void addFillExtrusionLayer(mbgl::style::Style &style, bool visible) {
    MLN_TRACE_FUNC();

    using namespace mbgl::style;
    using namespace mbgl::style::expression::dsl;

    // Satellite-only style does not contain building extrusions data.
    if (!style.getSource("composite")) {
        return;
    }

    if (auto layer = style.getLayer("3d-buildings")) {
        layer->setVisibility(VisibilityType(!visible));
        return;
    }

    auto extrusionLayer = std::make_unique<FillExtrusionLayer>("3d-buildings", "composite");
    extrusionLayer->setSourceLayer("building");
    extrusionLayer->setMinZoom(15.0f);
    extrusionLayer->setFilter(Filter(eq(get("extrude"), literal("true"))));
    extrusionLayer->setFillExtrusionColor(PropertyExpression<mbgl::Color>(interpolate(linear(),
                                                                                      number(get("height")),
                                                                                      0.f,
                                                                                      toColor(literal("#160e23")),
                                                                                      50.f,
                                                                                      toColor(literal("#00615f")),
                                                                                      100.f,
                                                                                      toColor(literal("#55e9ff")))));
    extrusionLayer->setFillExtrusionOpacity(0.6f);
    extrusionLayer->setFillExtrusionHeight(PropertyExpression<float>(get("height")));
    extrusionLayer->setFillExtrusionBase(PropertyExpression<float>(get("min_height")));
    style.addLayer(std::move(extrusionLayer));
}
} // namespace

void glfwError(int error, const char *description) {
    mbgl::Log::Error(mbgl::Event::OpenGL, std::string("GLFW error (") + std::to_string(error) + "): " + description);
}

GLFWView::GLFWView(bool fullscreen_,
                   bool benchmark_,
                   const mbgl::ResourceOptions &resourceOptions,
                   const mbgl::ClientOptions &clientOptions)
    : fullscreen(fullscreen_),
      benchmark(benchmark_),
      snapshotterObserver(std::make_unique<SnapshotObserver>()),
      mapResourceOptions(resourceOptions.clone()),
      mapClientOptions(clientOptions.clone()) {
    MLN_TRACE_FUNC();

    glfwSetErrorCallback(glfwError);

    std::srand(static_cast<unsigned int>(std::time(nullptr)));
#if defined(MLN_RENDER_BACKEND_WEBGPU)
#ifdef __linux__
    // Force X11 platform for WebGPU compatibility (Dawn doesn't support Wayland yet)
    // For now, always use X11 when WebGPU might be used
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
#endif

    if (!glfwInit()) {
        mbgl::Log::Error(mbgl::Event::OpenGL, "failed to initialize glfw");
        exit(1);
    }

    GLFWmonitor *monitor = nullptr;
    if (fullscreen) {
        monitor = glfwGetPrimaryMonitor();
        auto videoMode = glfwGetVideoMode(monitor);
        width = videoMode->width;
        height = videoMode->height;
    }

#if __APPLE__
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GL_TRUE);
#endif

    if (mbgl::gfx::Backend::GetType() != mbgl::gfx::Backend::Type::OpenGL) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    } else {
#if __APPLE__
        glfwWindowHint(GLFW_COCOA_GRAPHICS_SWITCHING, GL_TRUE);
#endif

#if MBGL_WITH_EGL
        glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
#endif

#ifdef DEBUG
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
#endif

        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

        glfwWindowHint(GLFW_RED_BITS, 8);
        glfwWindowHint(GLFW_GREEN_BITS, 8);
        glfwWindowHint(GLFW_BLUE_BITS, 8);
        glfwWindowHint(GLFW_ALPHA_BITS, 8);
        glfwWindowHint(GLFW_STENCIL_BITS, 8);
        glfwWindowHint(GLFW_DEPTH_BITS, 16);
    }

    window = glfwCreateWindow(width, height, "MapLibre Native", monitor, nullptr);
    if (!window) {
        glfwTerminate();
        mbgl::Log::Error(mbgl::Event::OpenGL, "failed to initialize window");
        exit(1);
    }

    glfwSetWindowUserPointer(window, this);
    glfwSetCursorPosCallback(window, onMouseMove);
    glfwSetMouseButtonCallback(window, onMouseClick);
    glfwSetWindowSizeCallback(window, onWindowResize);
    glfwSetFramebufferSizeCallback(window, onFramebufferResize);
    glfwSetScrollCallback(window, onScroll);
    glfwSetKeyCallback(window, onKey);
    glfwSetWindowFocusCallback(window, onWindowFocus);
#if defined(GLFW_RAW_MOUSE_MOTION)
    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
#endif
#if defined(__APPLE__) && defined(MLN_RENDER_BACKEND_VULKAN)
    glfwSetWindowRefreshCallback(window, onWindowRefresh);
#endif

    glfwGetWindowSize(window, &width, &height);

    // Uncapped swap for lower input latency; vsync is re-enabled when idle if preferVSync.
    const bool capFrameRate = false;
    preferVSync = false;
    backend = GLFWBackend::Create(window, capFrameRate);

    syncViewportFromWindow();

    glfwMakeContextCurrent(nullptr);

    printf("\n");
    printf(
        "======================================================================"
        "==========\n");
    printf("\n");
    printf("- Press `S` to cycle through bundled styles\n");
    printf("- Press `X` to reset the transform\n");
    printf("- Press `N` to reset north\n");
    printf("- Press `R` to enable the route demo\n");
    printf("- Press `E` to insert an example building extrusion layer\n");
    printf("- Press `O` to toggle online connectivity\n");
    printf("- Press `Z` to cycle through north orientations\n");
    printf("- Press `X` to cycle through the viewport modes\n");
    printf("- Press `I` to delete existing database and re-initialize\n");
    printf(
        "- Press `A` to cycle through Mapbox offices in the world + dateline "
        "monument\n");
    printf("- Press `B` to cycle through the color, stencil, and depth buffer\n");
    printf(
        "- Press `D` to cycle through camera bounds: inside, crossing IDL at "
        "left, crossing IDL at right, and "
        "disabled\n");
    printf("- Press `T` to add custom geometry source\n");
    printf("- Press `F` to enable feature-state demo\n");
    printf("- Press `U` to toggle pitch bounds\n");
    printf("- Press `H` to take a snapshot of a current map.\n");
    printf(
        "- Press `J` to take a snapshot of a current map with an extrusions "
        "overlay.\n");
    printf("- Press `Y` to start a camera fly-by demo\n");
    printf("\n");
    printf(
        "- Press `1` through `6` to add increasing numbers of point "
        "annotations for testing\n");
    printf(
        "- Press `7` through `0` to add increasing numbers of shape "
        "annotations for testing\n");
    printf("\n");
    printf("- Press `Q` to query annotations\n");
    printf("- Press `C` to remove annotations\n");
    printf("- Press `K` to add a random custom runtime imagery annotation\n");
    printf("- Press `L` to add a random line annotation\n");
    printf("- Press `W` to pop the last-added annotation off\n");
    printf("- Press `V` to toggle custom drawable layer\n");
    printf("- Press `B` to toggle rendering stats\n");
    printf("- Press `P` to pause tile requests\n");
    printf("\n");
    printf("- Hold `Control` + mouse drag to rotate\n");
    printf("- Hold `Shift` + mouse drag to tilt\n");
    printf("\n");
    printf("- Press `F1` to generate a render test for the current view\n");
    printf("\n");
    printf("- Press `Tab` to cycle through the map debug options\n");
    printf("- Press `F6` to cycle through Tile LOD modes\n");
    printf("- Press `F7` to lower the zoom level without changing the camera\n");
    printf("- Press `F8` to higher the zoom level without changing the camera\n");
    printf("- Press `Esc` to quit\n");
    printf("\n");
    printf(
        "======================================================================"
        "==========\n");
    printf("\n");
}

GLFWView::~GLFWView() {
    MLN_TRACE_FUNC();

    glfwDestroyWindow(window);
    glfwTerminate();
}

void GLFWView::setMap(mbgl::Map *map_) {
    MLN_TRACE_FUNC();

    map = map_;
    syncViewportFromWindow();
    map->addAnnotationImage(makeImage("default_marker", 22, 22, 1));
}

void GLFWView::setRenderFrontend(GLFWRendererFrontend *rendererFrontend_) {
    MLN_TRACE_FUNC();

    rendererFrontend = rendererFrontend_;
}

mbgl::gfx::RendererBackend &GLFWView::getRendererBackend() {
    MLN_TRACE_FUNC();

    return backend->getRendererBackend();
}

void GLFWView::onKey(GLFWwindow *window, int key, int /*scancode*/, int action, int mods) {
    MLN_TRACE_FUNC();

    auto *view = reinterpret_cast<GLFWView *>(glfwGetWindowUserPointer(window));

    if (action == GLFW_RELEASE) {
        if (key != GLFW_KEY_R || key != GLFW_KEY_S) view->animateRouteCallback = nullptr;

        switch (key) {
            case GLFW_KEY_ESCAPE:
                glfwSetWindowShouldClose(window, true);
                break;
            case GLFW_KEY_TAB:
                view->cycleDebugOptions();
                break;
            case GLFW_KEY_X:
                if (!mods)
                    view->map->jumpTo(
                        mbgl::CameraOptions().withCenter(mbgl::LatLng{}).withZoom(0.0).withBearing(0.0).withPitch(0.0));
                break;
            case GLFW_KEY_O:
                view->onlineStatusCallback();
                break;
            case GLFW_KEY_S:
                if (view->changeStyleCallback) view->changeStyleCallback();
                break;
            case GLFW_KEY_N:
                if (!mods)
                    view->map->easeTo(mbgl::CameraOptions().withBearing(0.0),
                                      mbgl::AnimationOptions{{mbgl::Milliseconds(500)}});
                break;
            case GLFW_KEY_Z:
                view->nextOrientation();
                break;
            case GLFW_KEY_Q: {
                auto result = view->rendererFrontend->getRenderer()->queryPointAnnotations(
                    {{}, {static_cast<double>(view->getSize().width), static_cast<double>(view->getSize().height)}});
                printf("visible point annotations: %zu\n", result.size());
                auto features = view->rendererFrontend->getRenderer()->queryRenderedFeatures(
                    mbgl::ScreenBox{{view->getSize().width * 0.5, view->getSize().height * 0.5},
                                    {view->getSize().width * 0.5 + 1.0, view->getSize().height * 0.5 + 1}},
                    {});
                printf("Rendered features at the center of the screen: %zu\n", features.size());
            } break;
            case GLFW_KEY_P:
                view->pauseResumeCallback();
                break;
            case GLFW_KEY_C:
                view->clearAnnotations();
                break;
            case GLFW_KEY_V:
                view->toggleCustomDrawableStyle();
                break;
            case GLFW_KEY_B:
                view->map->enableRenderingStatsView(!view->map->isRenderingStatsViewEnabled());
                break;
            case GLFW_KEY_I:
                view->resetDatabaseCallback();
                break;
            case GLFW_KEY_K:
                view->addRandomCustomPointAnnotations(1);
                break;
            case GLFW_KEY_L:
                view->addRandomLineAnnotations(1);
                break;
            case GLFW_KEY_A: {
                // XXX Fix precision loss in flyTo:
                // https://github.com/mapbox/mapbox-gl-native/issues/4298
                static const std::vector<mbgl::LatLng> places = {
                    mbgl::LatLng{-16.796665, -179.999983}, // Dateline monument
                    mbgl::LatLng{12.9810542, 77.6345551},  // Mapbox Bengaluru, India
                    mbgl::LatLng{-13.15607, -74.21773},    // Mapbox Peru
                    mbgl::LatLng{37.77572, -122.4158818},  // Mapbox SF, USA
                    mbgl::LatLng{38.91318, -77.03255},     // Mapbox DC, USA
                };
                static size_t nextPlace = 0;
                mbgl::CameraOptions cameraOptions;
                cameraOptions.center = places[nextPlace++];
                cameraOptions.zoom = 20;
                cameraOptions.pitch = 30;

                mbgl::AnimationOptions animationOptions(mbgl::Seconds(10));
                view->map->flyTo(cameraOptions, animationOptions);
                nextPlace = nextPlace % places.size();
            } break;
            case GLFW_KEY_R: {
                view->show3DExtrusions = true;
                view->toggle3DExtrusions(view->show3DExtrusions);
                if (view->animateRouteCallback) break;
                view->animateRouteCallback = [](mbgl::Map *routeMap) {
                    static mapbox::cheap_ruler::CheapRuler ruler{40.7}; // New York
                    static mapbox::geojson::geojson route{mapbox::geojson::parse(mbgl::platform::glfw::route)};
                    const auto &geometry = route.get<mapbox::geometry::geometry<double>>();
                    const auto &lineString = geometry.get<mapbox::geometry::line_string<double>>();

                    static double routeDistance = ruler.lineDistance(lineString);
                    static double routeProgress = 0;
                    routeProgress += 0.0005;
                    if (routeProgress > 1.0) {
                        routeProgress = 0.0;
                    }

                    auto camera = routeMap->getCameraOptions();

                    auto point = ruler.along(lineString, routeProgress * routeDistance);
                    const mbgl::LatLng center{point.y, point.x};
                    auto latLng = *camera.center;
                    double bearing = ruler.bearing({latLng.longitude(), latLng.latitude()}, point);
                    double easing = bearing - *camera.bearing;
                    easing += easing > 180.0 ? -360.0 : easing < -180 ? 360.0 : 0;
                    bearing = *camera.bearing + (easing / 20);
                    routeMap->jumpTo(
                        mbgl::CameraOptions().withCenter(center).withZoom(18.0).withBearing(bearing).withPitch(60.0));
                };
                view->animateRouteCallback(view->map);
            } break;
            case GLFW_KEY_E:
                view->toggle3DExtrusions(!view->show3DExtrusions);
                break;
            case GLFW_KEY_D: {
                static const std::vector<mbgl::LatLngBounds> bounds = {
                    mbgl::LatLngBounds::hull(mbgl::LatLng{-45.0, -170.0}, mbgl::LatLng{45.0, 170.0}),  // inside
                    mbgl::LatLngBounds::hull(mbgl::LatLng{-45.0, -200.0}, mbgl::LatLng{45.0, -160.0}), // left IDL
                    mbgl::LatLngBounds::hull(mbgl::LatLng{-45.0, 160.0}, mbgl::LatLng{45.0, 200.0}),   // right IDL
                    mbgl::LatLngBounds()};
                static size_t nextBound = 0u;
                static mbgl::AnnotationID boundAnnotationID = std::numeric_limits<mbgl::AnnotationID>::max();

                mbgl::LatLngBounds bound = bounds[nextBound++];
                nextBound = nextBound % bounds.size();

                view->map->setBounds(mbgl::BoundOptions().withLatLngBounds(bound));

                if (bound == mbgl::LatLngBounds()) {
                    view->map->removeAnnotation(boundAnnotationID);
                    boundAnnotationID = std::numeric_limits<mbgl::AnnotationID>::max();
                } else {
                    mbgl::Polygon<double> rect;
                    rect.push_back({
                        mbgl::Point<double>{bound.west(), bound.north()},
                        mbgl::Point<double>{bound.east(), bound.north()},
                        mbgl::Point<double>{bound.east(), bound.south()},
                        mbgl::Point<double>{bound.west(), bound.south()},
                    });

                    auto boundAnnotation = mbgl::FillAnnotation{
                        rect, 0.5f, {view->makeRandomColor()}, {view->makeRandomColor()}};

                    if (boundAnnotationID == std::numeric_limits<mbgl::AnnotationID>::max()) {
                        boundAnnotationID = view->map->addAnnotation(boundAnnotation);
                    } else {
                        view->map->updateAnnotation(boundAnnotationID, boundAnnotation);
                    }
                }
            } break;
            case GLFW_KEY_T:
                view->toggleCustomSource();
                break;
            case GLFW_KEY_F: {
                view->prepareForMapContentChange();
                view->clearFeatureHoverState();

                using namespace mbgl;
                using namespace mbgl::style;
                using namespace mbgl::style::expression::dsl;

                auto &style = view->map->getStyle();
                if (!style.getSource("states")) {
                    std::string url = "https://maplibre.org/maplibre-gl-js/docs/assets/us_states.geojson";
                    auto source = std::make_unique<GeoJSONSource>("states");
                    source->setURL(url);
                    style.addSource(std::move(source));

                    mbgl::CameraOptions cameraOptions;
                    cameraOptions.center = mbgl::LatLng{42.619626, -103.523181};
                    cameraOptions.zoom = 3;
                    cameraOptions.pitch = 0;
                    cameraOptions.bearing = 0;
                    view->map->jumpTo(cameraOptions);
                }

                auto layer = style.getLayer("state-fills");
                if (!layer) {
                    auto fillLayer = std::make_unique<FillLayer>("state-fills", "states");
                    fillLayer->setFillColor(mbgl::Color{0.0, 0.0, 1.0, 0.5});
                    fillLayer->setFillOpacity(PropertyExpression<float>(
                        createExpression(R"(["case", ["boolean", ["feature-state", "hover"], false], 1, 0.5])")));
                    style.addLayer(std::move(fillLayer));
                } else if (layer->getVisibility() == mbgl::style::VisibilityType::Visible) {
                    layer->setVisibility(mbgl::style::VisibilityType::None);
                    view->clearFeatureHoverState();
                } else {
                    layer->setVisibility(mbgl::style::VisibilityType::Visible);
                }

                layer = style.getLayer("state-borders");
                if (!layer) {
                    auto borderLayer = std::make_unique<LineLayer>("state-borders", "states");
                    borderLayer->setLineColor(mbgl::Color{0.0, 0.0, 1.0, 1.0});
                    borderLayer->setLineWidth(PropertyExpression<float>(
                        createExpression(R"(["case", ["boolean", ["feature-state", "hover"], false], 2, 1])")));
                    style.addLayer(std::move(borderLayer));
                } else if (layer->getVisibility() == mbgl::style::VisibilityType::Visible) {
                    layer->setVisibility(mbgl::style::VisibilityType::None);
                } else {
                    layer->setVisibility(mbgl::style::VisibilityType::Visible);
                }
            } break;
            case GLFW_KEY_F1: {
                bool success = TestWriter()
                                   .withInitialSize(mbgl::Size(view->width, view->height))
                                   .withStyle(view->map->getStyle())
                                   .withCameraOptions(view->map->getCameraOptions())
                                   .write(view->testDirectory);

                if (success) {
                    mbgl::Log::Info(mbgl::Event::General, "Render test created!");
                } else {
                    mbgl::Log::Error(mbgl::Event::General,
                                     "Fail to create render test! Base directory does not "
                                     "exist or permission denied.");
                }
            } break;
            case GLFW_KEY_U: {
                auto bounds = view->map->getBounds();
                if (bounds.minPitch == mbgl::util::rad2deg(mbgl::util::PITCH_MIN) &&
                    bounds.maxPitch == mbgl::util::rad2deg(mbgl::util::PITCH_MAX)) {
                    mbgl::Log::Info(mbgl::Event::General, "Limiting pitch bounds to [30, 40] degrees");
                    view->map->setBounds(mbgl::BoundOptions().withMinPitch(30).withMaxPitch(40));
                } else {
                    mbgl::Log::Info(mbgl::Event::General, "Resetting pitch bounds to [0, 60] degrees");
                    view->map->setBounds(mbgl::BoundOptions().withMinPitch(0).withMaxPitch(60));
                }
            } break;
            case GLFW_KEY_H: {
                view->makeSnapshot();
            } break;
            case GLFW_KEY_J: {
                // Snapshot with overlay
                view->makeSnapshot(true);
            } break;
            case GLFW_KEY_G: {
                view->toggleLocationIndicatorLayer();
            } break;
            case GLFW_KEY_Y: {
                view->freeCameraDemoPhase = 0;
                view->freeCameraDemoStartTime = mbgl::Clock::now();
                view->invalidate();
            } break;
            case GLFW_KEY_F6: {
                cycleTileLodProfile(*view->map);
            } break;
            case GLFW_KEY_F7: {
                tileLodZoomShift(*view->map, false);
            } break;
            case GLFW_KEY_F8: {
                tileLodZoomShift(*view->map, true);
            } break;
        }
    }

    if (action == GLFW_RELEASE) {
        switch (key) {
            case GLFW_KEY_W:
                view->prepareForMapContentChange();
                view->popAnnotation();
                break;
            case GLFW_KEY_1:
                view->prepareForMapContentChange();
                view->addRandomPointAnnotations(1);
                break;
            case GLFW_KEY_2:
                view->prepareForMapContentChange();
                view->addRandomPointAnnotations(10);
                break;
            case GLFW_KEY_3:
                view->prepareForMapContentChange();
                view->addRandomPointAnnotations(100);
                break;
            case GLFW_KEY_4:
                view->prepareForMapContentChange();
                view->addRandomPointAnnotations(1000);
                break;
            case GLFW_KEY_5:
                view->prepareForMapContentChange();
                view->addRandomPointAnnotations(10000);
                break;
            case GLFW_KEY_6:
                view->prepareForMapContentChange();
                view->addRandomPointAnnotations(100000);
                break;
            case GLFW_KEY_7:
                view->prepareForMapContentChange();
                view->addRandomShapeAnnotations(1);
                break;
            case GLFW_KEY_8:
                view->prepareForMapContentChange();
                view->addRandomShapeAnnotations(10);
                break;
            case GLFW_KEY_9:
                view->prepareForMapContentChange();
                view->addRandomShapeAnnotations(100);
                break;
            case GLFW_KEY_0:
                view->prepareForMapContentChange();
                view->addRandomShapeAnnotations(1000);
                break;
            case GLFW_KEY_M:
                view->prepareForMapContentChange();
                view->addAnimatedAnnotation();
                break;
        }
    }
}

namespace mbgl {
namespace util {

template <>
struct Interpolator<mbgl::LatLng> {
    mbgl::LatLng operator()(const mbgl::LatLng &a, const mbgl::LatLng &b, const double t) {
        return {
            interpolate<double>(a.latitude(), b.latitude(), t),
            interpolate<double>(a.longitude(), b.longitude(), t),
        };
    }
};

} // namespace util
} // namespace mbgl

void GLFWView::updateFreeCameraDemo() {
    MLN_TRACE_FUNC();

    const mbgl::LatLng trainStartPos = {60.171367, 24.941359};
    const mbgl::LatLng trainEndPos = {60.185147, 24.936668};
    const mbgl::LatLng cameraStartPos = {60.167443, 24.927176};
    const mbgl::LatLng cameraEndPos = {60.185107, 24.933366};
    const double cameraStartAlt = 1000.0;
    const double cameraEndAlt = 150.0;
    const double duration = 8.0;

    // Interpolate between starting and ending points
    std::chrono::duration<double> deltaTime = mbgl::Clock::now() - freeCameraDemoStartTime;
    freeCameraDemoPhase = deltaTime.count() / duration;

    auto trainPos = mbgl::util::interpolate(trainStartPos, trainEndPos, freeCameraDemoPhase);
    auto cameraPos = mbgl::util::interpolate(cameraStartPos, cameraEndPos, freeCameraDemoPhase);
    auto cameraAlt = mbgl::util::interpolate(cameraStartAlt, cameraEndAlt, freeCameraDemoPhase);

    mbgl::FreeCameraOptions camera;

    // Update camera position and focus point on the map with interpolated values
    camera.setLocation({cameraPos, cameraAlt});
    camera.lookAtPoint(trainPos);

    map->setFreeCameraOptions(camera);

    if (freeCameraDemoPhase > 1.0) {
        freeCameraDemoPhase = -1.0;
    }
}

mbgl::Color GLFWView::makeRandomColor() const {
    const auto r = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
    const auto g = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
    const auto b = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
    return {r, g, b, 1.0f};
}

mbgl::Point<double> GLFWView::makeRandomPoint() const {
    const double x = width * static_cast<double>(std::rand()) / RAND_MAX;
    const double y = height * static_cast<double>(std::rand()) / RAND_MAX;
    mbgl::LatLng latLng = map->latLngForPixel({x, y});
    return {latLng.longitude(), latLng.latitude()};
}

std::unique_ptr<mbgl::style::Image> GLFWView::makeImage(const std::string &id,
                                                        int width,
                                                        int height,
                                                        float pixelRatio) {
    MLN_TRACE_FUNC();

    const int r = static_cast<int>(255 * (static_cast<double>(std::rand()) / RAND_MAX));
    const int g = static_cast<int>(255 * (static_cast<double>(std::rand()) / RAND_MAX));
    const int b = static_cast<int>(255 * (static_cast<double>(std::rand()) / RAND_MAX));

    const int w = static_cast<int>(std::ceil(pixelRatio * width));
    const int h = static_cast<int>(std::ceil(pixelRatio * height));

    mbgl::PremultipliedImage image({static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
    auto data = reinterpret_cast<uint32_t *>(image.data.get());
    const int dist = (w / 2) * (w / 2);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int dx = x - w / 2;
            const int dy = y - h / 2;
            const int diff = dist - (dx * dx + dy * dy);
            if (diff > 0) {
                const int a = std::min(0xFF, diff) * 0xFF / dist;
                // Premultiply the rgb values with alpha
                data[w * y + x] = (a << 24) | ((a * r / 0xFF) << 16) | ((a * g / 0xFF) << 8) | (a * b / 0xFF);
            }
        }
    }

    return std::make_unique<mbgl::style::Image>(id, std::move(image), pixelRatio);
}

void GLFWView::nextOrientation() {
    using NO = mbgl::NorthOrientation;
    switch (map->getMapOptions().northOrientation()) {
        case NO::Upwards:
            map->setNorthOrientation(NO::Rightwards);
            break;
        case NO::Rightwards:
            map->setNorthOrientation(NO::Downwards);
            break;
        case NO::Downwards:
            map->setNorthOrientation(NO::Leftwards);
            break;
        default:
            map->setNorthOrientation(NO::Upwards);
            break;
    }
}

void GLFWView::addRandomCustomPointAnnotations(int count) {
    MLN_TRACE_FUNC();

    for (int i = 0; i < count; i++) {
        static int spriteID = 1;
        const auto name = std::string{"marker-"} + mbgl::util::toString(spriteID++);
        map->addAnnotationImage(makeImage(name, 22, 22, 1));
        spriteIDs.push_back(name);
        annotationIDs.push_back(map->addAnnotation(mbgl::SymbolAnnotation{makeRandomPoint(), name}));
    }
}

void GLFWView::addRandomPointAnnotations(int count) {
    MLN_TRACE_FUNC();

    if (!map || count <= 0) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        annotationIDs.push_back(map->addAnnotation(mbgl::SymbolAnnotation{makeRandomPoint(), "default_marker"}));
    }
}

void GLFWView::addRandomLineAnnotations(int count) {
    MLN_TRACE_FUNC();

    for (int i = 0; i < count; ++i) {
        mbgl::LineString<double> lineString;
        for (int j = 0; j < 3; ++j) {
            lineString.push_back(makeRandomPoint());
        }
        annotationIDs.push_back(map->addAnnotation(mbgl::LineAnnotation{lineString, 1.0f, 2.0f, {makeRandomColor()}}));
    }
}

void GLFWView::addRandomShapeAnnotations(int count) {
    MLN_TRACE_FUNC();

    for (int i = 0; i < count; ++i) {
        mbgl::Polygon<double> triangle;
        triangle.push_back({makeRandomPoint(), makeRandomPoint(), makeRandomPoint()});
        annotationIDs.push_back(
            map->addAnnotation(mbgl::FillAnnotation{triangle, 0.5f, {makeRandomColor()}, {makeRandomColor()}}));
    }
}

void GLFWView::addAnimatedAnnotation() {
    MLN_TRACE_FUNC();

    const double started = glfwGetTime();
    animatedAnnotationIDs.push_back(map->addAnnotation(mbgl::SymbolAnnotation{{0, 0}, "default_marker"}));
    animatedAnnotationAddedTimes.push_back(started);
}

void GLFWView::updateAnimatedAnnotations() {
    MLN_TRACE_FUNC();

    const double time = glfwGetTime();
    for (size_t i = 0; i < animatedAnnotationIDs.size(); i++) {
        auto dt = time - animatedAnnotationAddedTimes[i];

        const double period = 10;
        const double x = dt / period * 360 - 180;
        const double y = std::sin(dt / period * pi * 2.0) * 80;
        map->updateAnnotation(animatedAnnotationIDs[i], mbgl::SymbolAnnotation{{x, y}, "default_marker"});
    }
}

void GLFWView::cycleDebugOptions() {
    auto debug = map->getDebug();

    if (debug & mbgl::MapDebugOptions::Overdraw)
        debug = mbgl::MapDebugOptions::NoDebug;
    else if (debug & mbgl::MapDebugOptions::Collision)
        debug = mbgl::MapDebugOptions::Overdraw;
    else if (debug & mbgl::MapDebugOptions::Timestamps)
        debug = debug | mbgl::MapDebugOptions::Collision;
    else if (debug & mbgl::MapDebugOptions::ParseStatus)
        debug = debug | mbgl::MapDebugOptions::Timestamps;
    else if (debug & mbgl::MapDebugOptions::TileBorders)
        debug = debug | mbgl::MapDebugOptions::ParseStatus;
    else
        debug = mbgl::MapDebugOptions::TileBorders;

    map->setDebug(debug);
}

void GLFWView::clearAnnotations() {
    MLN_TRACE_FUNC();

    for (const auto &id : annotationIDs) {
        map->removeAnnotation(id);
    }

    annotationIDs.clear();

    for (const auto &id : animatedAnnotationIDs) {
        map->removeAnnotation(id);
    }

    animatedAnnotationIDs.clear();
}

void GLFWView::popAnnotation() {
    if (annotationIDs.empty()) {
        return;
    }

    map->removeAnnotation(annotationIDs.back());
    annotationIDs.pop_back();
}

void GLFWView::toggleCustomDrawableStyle() {
#if !defined(MBGL_LAYER_CUSTOM_DISABLE_ALL)
    auto &style = map->getStyle();

    const std::string identifier = "ExampleCustomDrawableStyleLayer";
    const auto &existingLayer = style.getLayer(identifier);

    if (!existingLayer) {
        style.addLayer(std::make_unique<mbgl::style::CustomDrawableLayer>(
            identifier, std::make_unique<ExampleCustomDrawableStyleLayerHost>(MLN_ASSETS_PATH)));
    } else {
        style.removeLayer(identifier);
    }

#endif
}

void GLFWView::makeSnapshot(bool withOverlay) {
    MLN_TRACE_FUNC();

    if (!snapshotter || snapshotter->getStyleURL() != map->getStyle().getURL()) {
        snapshotter = std::make_unique<mbgl::MapSnapshotter>(map->getMapOptions().size(),
                                                             map->getMapOptions().pixelRatio(),
                                                             mapResourceOptions,
                                                             mapClientOptions,
                                                             *snapshotterObserver);
        snapshotter->setStyleURL(map->getStyle().getURL());
    }

    auto snapshot = [&] {
        snapshotter->setCameraOptions(map->getCameraOptions());
        snapshotter->snapshot([](const std::exception_ptr &ptr,
                                 mbgl::PremultipliedImage image,
                                 const mbgl::MapSnapshotter::Attributions &,
                                 const mbgl::MapSnapshotter::PointForFn &,
                                 const mbgl::MapSnapshotter::LatLngForFn &) {
            if (!ptr) {
                std::ostringstream oss;
                oss << "Made snapshot './snapshot.png' with size w:" << image.size.width << "px h:" << image.size.height
                    << "px";
                mbgl::Log::Info(mbgl::Event::General, oss.str());
                std::ofstream file("./snapshot.png", std::ios::out | std::ios::binary);
                file << mbgl::encodePNG(image);
            } else {
                mbgl::Log::Error(mbgl::Event::General, "Failed to make a snapshot!");
            }
        });
    };

    if (withOverlay) {
        snapshotterObserver->didFinishLoadingStyleCallback = [&] {
            addFillExtrusionLayer(snapshotter->getStyle(), withOverlay);
            snapshot();
        };
    } else {
        snapshot();
    }
}

void GLFWView::onScroll(GLFWwindow *window, double /*xOffset*/, double yOffset) {
    MLN_TRACE_FUNC();

    auto *view = reinterpret_cast<GLFWView *>(glfwGetWindowUserPointer(window));
    double delta = yOffset * 40;

    bool isWheel = delta != 0 && std::fmod(delta, 4.000244140625) == 0;

    double absDelta = delta < 0 ? -delta : delta;
    double scale = 2.0 / (1.0 + std::exp(-absDelta / 100.0));

    // Make the scroll wheel a bit slower.
    if (!isWheel) {
        scale = (scale - 1.0) / 2.0 + 1.0;
    }

    // Zooming out.
    if (delta < 0 && scale != 0) {
        scale = 1.0 / scale;
    }

    view->map->scaleBy(scale, mbgl::ScreenCoordinate{view->lastX, view->lastY});
    view->extendScrollGestureFreeze();
#if defined(MLN_RENDER_BACKEND_OPENGL) && !defined(MBGL_LAYER_CUSTOM_DISABLE_ALL)
    if (view->puck && view->puckFollowsCameraCenter) {
        mbgl::LatLng mapCenter = view->map->getCameraOptions().center.value();
        view->puck->setLocation(toArray(mapCenter));
    }
#endif
}

void GLFWView::prepareForMapContentChange() {
    releasePanSnapshot();
    wake();
}

void GLFWView::clearFeatureHoverState() {
    if (!rendererFrontend || !featureID) {
        featureID = std::nullopt;
        return;
    }

    mbgl::FeatureState cleared;
    cleared["hover"] = false;
    rendererFrontend->getRenderer()->setFeatureState("states", {}, *featureID, cleared);
    featureID = std::nullopt;
}

void GLFWView::capturePanSnapshot(const double mouseX, const double mouseY) {
    panSnapshotOriginX = mouseX;
    panSnapshotOriginY = mouseY;
    lastX = mouseX;
    lastY = mouseY;
    panSnapshotActive = false;

    if (!backend || !rendererFrontend) {
        return;
    }

    mbgl::gfx::BackendScope scope{backend->getRendererBackend(), mbgl::gfx::BackendScope::ScopeType::Implicit};

    backend->releasePanSnapshot();

    // Grab the current map image before gesture-freeze starts. If a frame is
    // pending, render once and capture from the back buffer inside swap().
    if (dirty) {
        syncViewportFromWindow();
        backend->requestPanSnapshotCaptureOnNextSwap();
        rendererFrontend->render();
        dirty = false;
        panSnapshotActive = backend->hasPanSnapshot();
    } else {
        panSnapshotActive = backend->captureDisplayedPanSnapshot();
    }
}

void GLFWView::releasePanSnapshot() {
    panSnapshotActive = false;
    if (!backend) {
        return;
    }

    mbgl::gfx::BackendScope scope{backend->getRendererBackend(), mbgl::gfx::BackendScope::ScopeType::Implicit};
    backend->releasePanSnapshot();
}

bool GLFWView::renderPanSnapshot() {
    if (!panSnapshotActive || !tracking || !backend) {
        return false;
    }

    // Window Y grows downward; OpenGL framebuffer Y grows upward — negate Y so the
    // snapshot follows the cursor (grab-and-drag), matching moveBy semantics.
    const float offsetX = static_cast<float>((lastX - panSnapshotOriginX) * pixelRatio);
    const float offsetY = static_cast<float>((panSnapshotOriginY - lastY) * pixelRatio);
    mbgl::gfx::BackendScope scope{backend->getRendererBackend(), mbgl::gfx::BackendScope::ScopeType::Implicit};
    return backend->drawPanSnapshot(offsetX, offsetY);
}

void GLFWView::syncViewportFromWindow() {
    if (!window || !backend) {
        return;
    }

    int winW = 0;
    int winH = 0;
    glfwGetWindowSize(window, &winW, &winH);
    if (winW > 0 && winH > 0) {
        width = winW;
        height = winH;
    }

    int fbW = 0;
    int fbH = 0;
    glfwGetFramebufferSize(window, &fbW, &fbH);
    if (fbW <= 0 || fbH <= 0) {
        return;
    }

    backend->setSize({static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH)});

    const float newPixelRatio = static_cast<float>(fbW) / static_cast<float>(width);
    pixelRatio = newPixelRatio;

    if (map) {
        map->setSize({static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
        map->setPixelRatio(newPixelRatio);
    }
}

void GLFWView::onWindowResize(GLFWwindow *window, int width, int height) {
    MLN_TRACE_FUNC();

    auto *view = reinterpret_cast<GLFWView *>(glfwGetWindowUserPointer(window));
    view->width = width;
    view->height = height;
    view->syncViewportFromWindow();
}

void GLFWView::onFramebufferResize(GLFWwindow *window, int /*width*/, int /*height*/) {
    MLN_TRACE_FUNC();

    auto *view = reinterpret_cast<GLFWView *>(glfwGetWindowUserPointer(window));
    view->syncViewportFromWindow();
    view->invalidate();

#if defined(__APPLE__) && defined(MLN_RENDER_BACKEND_VULKAN)
    // Render continuously while resizing
    // Untested elsewhere
    view->render();

#if defined(MLN_RENDER_BACKEND_OPENGL)
    glfwSwapBuffers(window);
#endif
#endif
}

void GLFWView::setInteracting(bool active) {
    if (mouseGestureActive == active) {
        return;
    }
    mouseGestureActive = active;
    syncGestureFreezeState();
}

void GLFWView::extendScrollGestureFreeze() {
    const bool wasFrozen = interacting;
    scrollGestureFrozen = true;
    lastInteractionTime = glfwGetTime();

    scrollFreezeTimer.stop();
    scrollFreezeTimer.start(mbgl::Milliseconds(500), mbgl::Duration::zero(), [this] {
        scrollFreezeTimer.stop();
        scrollGestureFrozen = false;
        syncGestureFreezeState();
    });

    syncGestureFreezeState();
    if (wasFrozen) {
        wake();
    }
}

void GLFWView::syncGestureFreezeState() {
    const bool shouldFreeze = mouseGestureActive || scrollGestureFrozen;
    if (interacting == shouldFreeze) {
        return;
    }
    interacting = shouldFreeze;
    lastInteractionTime = glfwGetTime();

    if (map) {
        if (shouldFreeze) {
            savedPrefetchZoomDelta = map->getPrefetchZoomDelta();
            map->setPrefetchZoomDelta(0);
            savedTileLodZoomShift = map->getTileLodZoomShift();
            map->setTileLodZoomShift(savedTileLodZoomShift - 0.25);
            map->setGestureInProgress(true);
        } else {
            map->setPrefetchZoomDelta(savedPrefetchZoomDelta);
            map->setTileLodZoomShift(savedTileLodZoomShift);
            map->setGestureInProgress(false);
            map->triggerRepaint();
        }
    }

    if (backend) {
        backend->setVSyncEnabled(shouldFreeze ? false : preferVSync);
    }

    wake();
    restartFrameTick();
}

void GLFWView::wake() {
    dirty = true;
    glfwPostEmptyEvent();
}

void GLFWView::restartFrameTick() {
    frameTick.stop();
    const auto interval = interacting || benchmark ? mbgl::Milliseconds(1000 / 120) : mbgl::Milliseconds(1000 / 60);
    frameTick.start(mbgl::Duration::zero(), interval, [this] { processEventsAndRender(); });
}

void GLFWView::processEventsAndRender() {
    if (glfwWindowShouldClose(window)) {
        runLoop.stop();
        return;
    }

    glfwPollEvents();

    if (dirty) {
        constexpr double minInteractionFrameInterval = 1.0 / 120.0;
        const double now = glfwGetTime();
        const bool panSnapshotFrame = panSnapshotActive && tracking;
        if (interacting && !panSnapshotFrame && (now - lastRenderTime) < minInteractionFrameInterval) {
            return;
        }
        render();
        lastRenderTime = now;
    }

#ifndef __APPLE__
    runLoop.updateTime();
#endif
}

void GLFWView::updateHoverFeatureState() {
    if (!map || !rendererFrontend || panSnapshotActive || isInteracting()) {
        return;
    }

    auto& style = map->getStyle();
    const auto* layer = style.getLayer("state-fills");
    if (!layer || layer->getVisibility() != mbgl::style::VisibilityType::Visible || !style.getSource("states")) {
        clearFeatureHoverState();
        return;
    }

    const mbgl::ScreenCoordinate screenCoordinate{lastX, lastY};
    const mbgl::RenderedQueryOptions queryOptions({{{"state-fills"}}, {}});
    auto result = rendererFrontend->getRenderer()->queryRenderedFeatures(screenCoordinate, queryOptions);
    using namespace mbgl;
    FeatureState newState;

    if (!result.empty()) {
        FeatureIdentifier id = result[0].id;
        std::optional<std::string> idStr = featureIDtoString(id);

        if (idStr) {
            if (featureID && (*featureID != *idStr)) {
                newState["hover"] = false;
                rendererFrontend->getRenderer()->setFeatureState("states", {}, *featureID, newState);
                featureID = std::nullopt;
            }

            if (!featureID) {
                newState["hover"] = true;
                featureID = featureIDtoString(id);
                rendererFrontend->getRenderer()->setFeatureState("states", {}, *featureID, newState);
            }
        }
    } else if (featureID) {
        newState["hover"] = false;
        rendererFrontend->getRenderer()->setFeatureState("states", {}, *featureID, newState);
        featureID = std::nullopt;
    }
}

void GLFWView::onMouseClick(GLFWwindow *window, int button, int action, int modifiers) {
    MLN_TRACE_FUNC();

    auto *view = reinterpret_cast<GLFWView *>(glfwGetWindowUserPointer(window));

    if (button == GLFW_MOUSE_BUTTON_RIGHT || (button == GLFW_MOUSE_BUTTON_LEFT && modifiers & GLFW_MOD_CONTROL)) {
        view->rotating = action == GLFW_PRESS;
        view->setInteracting(view->rotating || view->tracking || view->pitching);
    } else if (button == GLFW_MOUSE_BUTTON_LEFT && (modifiers & GLFW_MOD_SHIFT)) {
        view->pitching = action == GLFW_PRESS;
        view->setInteracting(view->rotating || view->tracking || view->pitching);
    } else if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS && !(modifiers & (GLFW_MOD_CONTROL | GLFW_MOD_SHIFT))) {
            double mx = 0;
            double my = 0;
            glfwGetCursorPos(window, &mx, &my);
            // Capture the displayed map before gesture-freeze; then start tracking.
            view->capturePanSnapshot(mx, my);
            view->tracking = true;
            view->setInteracting(view->rotating || view->tracking || view->pitching);
        } else if (action == GLFW_RELEASE) {
            view->tracking = false;
            view->releasePanSnapshot();
            view->setInteracting(view->rotating || view->tracking || view->pitching);
            view->updateHoverFeatureState();

            double now = glfwGetTime();
            if (now - view->lastClick < 0.4 /* ms */) {
                if (modifiers & GLFW_MOD_SHIFT) {
                    view->map->scaleBy(0.5,
                                       mbgl::ScreenCoordinate{view->lastX, view->lastY},
                                       mbgl::AnimationOptions{{mbgl::Milliseconds(500)}});
                } else {
                    view->map->scaleBy(2.0,
                                       mbgl::ScreenCoordinate{view->lastX, view->lastY},
                                       mbgl::AnimationOptions{{mbgl::Milliseconds(500)}});
                }
            }
            view->lastClick = now;
        }
    }
}

void GLFWView::onMouseMove(GLFWwindow *window, double x, double y) {
    MLN_TRACE_FUNC();

    auto *view = reinterpret_cast<GLFWView *>(glfwGetWindowUserPointer(window));
    if (view->tracking) {
        const double dx = x - view->lastX;
        const double dy = y - view->lastY;
        if ((dx || dy) && view->map) {
            view->map->moveBy(mbgl::ScreenCoordinate{dx, dy});
            view->wake();
        }
    } else if (view->rotating) {
        view->map->rotateBy({view->lastX, view->lastY}, {x, y});
        view->wake();
    } else if (view->pitching) {
        const double dy = y - view->lastY;
        if (dy) {
            view->map->pitchBy(dy / 2);
            view->wake();
        }
    } else {
        view->updateHoverFeatureState();
    }
    view->lastX = x;
    view->lastY = y;
#if defined(MLN_RENDER_BACKEND_OPENGL) && !defined(MBGL_LAYER_CUSTOM_DISABLE_ALL)
    if (view->puck && view->puckFollowsCameraCenter) {
        mbgl::LatLng mapCenter = view->map->getCameraOptions().center.value();
        view->puck->setLocation(toArray(mapCenter));
    }
#endif
}

void GLFWView::onWindowFocus(GLFWwindow *window, int focused) {
    MLN_TRACE_FUNC();

    if (focused == GLFW_FALSE) {
        auto *view = reinterpret_cast<GLFWView *>(glfwGetWindowUserPointer(window));
        if (glfwGetTime() - view->lastInteractionTime >= 5.0) {
            view->rendererFrontend->getRenderer()->reduceMemoryUse();
        }
    }
}

#if defined(__APPLE__) && defined(MLN_RENDER_BACKEND_VULKAN)
void GLFWView::onWindowRefresh(GLFWwindow *window) {
    // Untested elsewhere
    if (auto *view = reinterpret_cast<GLFWView *>(glfwGetWindowUserPointer(window))) {
        view->invalidate();
        view->render();
#if defined(MLN_RENDER_BACKEND_OPENGL)
        glfwSwapBuffers(window);
#endif
    }
}
#endif

void GLFWView::render() {
    if (dirty && rendererFrontend) {
        MLN_TRACE_ZONE(ReRender);

        syncViewportFromWindow();

        if (panSnapshotActive && tracking && renderPanSnapshot()) {
            dirty = false;
            report(0.1f); // snapshot blit; wall time is not meaningful here
            if (benchmark && interacting) {
                invalidate();
            }
            return;
        }

        if (panSnapshotActive) {
            releasePanSnapshot();
        }

        dirty = false;
        const double started = glfwGetTime();

        if (animateRouteCallback) {
            MLN_TRACE_ZONE(animateRouteCallback);

            animateRouteCallback(map);
        }

        updateAnimatedAnnotations();

        mbgl::gfx::BackendScope scope{backend->getRendererBackend()};

        rendererFrontend->render();

        if (freeCameraDemoPhase >= 0.0) {
            updateFreeCameraDemo();
        }

        report(static_cast<float>(1000 * (glfwGetTime() - started)));
        if (benchmark && interacting) {
            invalidate();
        }
    }
}

void GLFWView::run() {
    MLN_TRACE_FUNC();

    restartFrameTick();

    maintenanceTick.start(mbgl::Seconds(180), mbgl::Seconds(180), [this] {
        if (interacting) {
            return;
        }
        if (maintenanceCallback) {
            maintenanceCallback();
        }
        if (rendererFrontend && glfwGetTime() - lastInteractionTime >= 5.0) {
            rendererFrontend->getRenderer()->reduceMemoryUse();
        }
    });

#if defined(__APPLE__)
    while (!glfwWindowShouldClose(window)) runLoop.run();
#else
    runLoop.run();
#endif
}

float GLFWView::getPixelRatio() const {
    return pixelRatio;
}

mbgl::Size GLFWView::getSize() const {
    return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

void GLFWView::invalidate() {
    MLN_TRACE_FUNC();

    dirty = true;
    if (!interacting) {
        glfwPostEmptyEvent();
    }
}

void GLFWView::report(float duration) {
    if (!benchmark) {
        return;
    }

    frames++;
    frameTime += duration;

    const double currentTime = glfwGetTime();
    if (currentTime - lastReported >= 1) {
        const float avgFrameTime = frames > 0 ? (frameTime / frames) : 0.f;

        std::ostringstream oss;
        oss.precision(2);
        oss << std::fixed << "Frame wall: " << avgFrameTime << "ms";
        if (avgFrameTime > 0.001f) {
            oss << " (" << (1000.f / avgFrameTime) << "fps)";
        } else {
            oss << " (snapshot pan)";
        }
        if (benchmarkStatFrames > 0) {
            oss << " | encode: " << (benchmarkEncodeTime / benchmarkStatFrames) * 1000.0 << "ms"
                << " | gpu present: " << (benchmarkRenderTime / benchmarkStatFrames) * 1000.0 << "ms";
        }
        if (benchmarkGestureFrames > 0) {
            oss << " | gesture gpu: " << (benchmarkGestureRenderTime / benchmarkGestureFrames) * 1000.0 << "ms"
                << " (" << benchmarkGestureFrames << "f)";
        }
        if (benchmarkNonGestureFrames > 0) {
            oss << " | fill-in gpu: " << (benchmarkNonGestureRenderTime / benchmarkNonGestureFrames) * 1000.0 << "ms"
                << " (" << benchmarkNonGestureFrames << "f)";
        }
        if (lastFrameWasGesture) {
            oss << " [gesture]";
        }
        mbgl::Log::Info(mbgl::Event::Render, oss.str());

        frames = 0;
        frameTime = 0;
        benchmarkStatFrames = 0;
        benchmarkEncodeTime = 0;
        benchmarkRenderTime = 0;
        benchmarkGestureFrames = 0;
        benchmarkGestureEncodeTime = 0;
        benchmarkGestureRenderTime = 0;
        benchmarkNonGestureFrames = 0;
        benchmarkNonGestureEncodeTime = 0;
        benchmarkNonGestureRenderTime = 0;
        lastReported = currentTime;
    }
}

void GLFWView::reportBenchmarkStats(const mbgl::gfx::RenderingStats& stats) {
    if (!benchmark) {
        return;
    }
    benchmarkStatFrames++;
    benchmarkEncodeTime += stats.encodingTime;
    benchmarkRenderTime += stats.renderingTime;
    lastFrameWasGesture = interacting;
    if (interacting) {
        benchmarkGestureFrames++;
        benchmarkGestureEncodeTime += stats.encodingTime;
        benchmarkGestureRenderTime += stats.renderingTime;
    } else {
        benchmarkNonGestureFrames++;
        benchmarkNonGestureEncodeTime += stats.encodingTime;
        benchmarkNonGestureRenderTime += stats.renderingTime;
    }
}

void GLFWView::onDidFinishRenderingFrame(const mbgl::MapObserver::RenderFrameStatus& status) {
    reportBenchmarkStats(status.renderingStats);
}

void GLFWView::setChangeStyleCallback(std::function<void()> callback) {
    changeStyleCallback = std::move(callback);
}

void GLFWView::setShouldClose() {
    MLN_TRACE_FUNC();

    glfwSetWindowShouldClose(window, true);
    glfwPostEmptyEvent();
}

void GLFWView::setWindowTitle(const std::string &title) {
    MLN_TRACE_FUNC();

    glfwSetWindowTitle(window, (std::string{"MapLibre Native (GLFW): "} + title).c_str());
}

void GLFWView::onDidFinishLoadingStyle() {
    MLN_TRACE_FUNC();

#if defined(MLN_RENDER_BACKEND_OPENGL) && !defined(MBGL_LAYER_CUSTOM_DISABLE_ALL)
    puck = nullptr;
#endif

    if (show3DExtrusions) {
        toggle3DExtrusions(show3DExtrusions);
    }
}

void GLFWView::toggle3DExtrusions(bool visible) {
    MLN_TRACE_FUNC();

    show3DExtrusions = visible;
    addFillExtrusionLayer(map->getStyle(), show3DExtrusions);
}

void GLFWView::toggleCustomSource() {
    MLN_TRACE_FUNC();

    if (!map->getStyle().getSource("custom")) {
        mbgl::style::CustomGeometrySource::Options options;
        options.cancelTileFunction = [](const mbgl::CanonicalTileID &) {
        };
        options.fetchTileFunction = [&](const mbgl::CanonicalTileID &tileID) {
            double gridSpacing = 0.1;
            mbgl::FeatureCollection features;
            const mbgl::LatLngBounds bounds(tileID);
            for (double y = ceil(bounds.north() / gridSpacing) * gridSpacing;
                 y >= floor(bounds.south() / gridSpacing) * gridSpacing;
                 y -= gridSpacing) {
                mapbox::geojson::line_string gridLine;
                gridLine.emplace_back(bounds.west(), y);
                gridLine.emplace_back(bounds.east(), y);

                features.emplace_back(gridLine);
            }

            for (double x = floor(bounds.west() / gridSpacing) * gridSpacing;
                 x <= ceil(bounds.east() / gridSpacing) * gridSpacing;
                 x += gridSpacing) {
                mapbox::geojson::line_string gridLine;
                gridLine.emplace_back(x, bounds.south());
                gridLine.emplace_back(x, bounds.north());

                features.emplace_back(gridLine);
            }
            auto source = static_cast<mbgl::style::CustomGeometrySource *>(map->getStyle().getSource("custom"));
            if (source) {
                source->setTileData(tileID, features);
                source->invalidateTile(tileID);
            }
        };
        map->getStyle().addSource(std::make_unique<mbgl::style::CustomGeometrySource>("custom", options));
    }

    auto *layer = map->getStyle().getLayer("grid");

    if (!layer) {
        auto lineLayer = std::make_unique<mbgl::style::LineLayer>("grid", "custom");
        lineLayer->setLineColor(mbgl::Color{1.0, 0.0, 0.0, 1.0});
        map->getStyle().addLayer(std::move(lineLayer));
    } else {
        layer->setVisibility(layer->getVisibility() == mbgl::style::VisibilityType::Visible
                                 ? mbgl::style::VisibilityType::None
                                 : mbgl::style::VisibilityType::Visible);
    }
}

void GLFWView::toggleLocationIndicatorLayer() {
    MLN_TRACE_FUNC();

#ifdef ENABLE_LOCATION_INDICATOR
    puck = static_cast<mbgl::style::LocationIndicatorLayer *>(map->getStyle().getLayer("puck"));
    static const mbgl::LatLng puckLocation{35.683389, 139.76525}; // A location on the crossing of 4 tiles
    if (puck == nullptr) {
        auto puckLayer = std::make_unique<mbgl::style::LocationIndicatorLayer>("puck");

        puckLayer->setLocationTransition(
            mbgl::style::TransitionOptions(mbgl::Duration::zero(),
                                           mbgl::Duration::zero())); // Note: This is used here for demo purpose.
                                                                     // SDKs should not use this, or else the location
                                                                     // will "jump" to positions.
        puckLayer->setLocation(toArray(puckLocation));
        puckLayer->setAccuracyRadius(50);
        puckLayer->setAccuracyRadiusColor(
            premultiply(mbgl::Color{0.0f, 1.0f, 0.0f, 0.2f})); // Note: these must be fed premultiplied

        puckLayer->setBearingTransition(mbgl::style::TransitionOptions(mbgl::Duration::zero(), mbgl::Duration::zero()));
        puckLayer->setBearing(mbgl::style::Rotation(0.0));
        puckLayer->setAccuracyRadiusBorderColor(premultiply(mbgl::Color{0.0f, 1.0f, 0.2f, 0.4f}));
        puckLayer->setTopImageSize(0.18f);
        puckLayer->setBearingImageSize(0.26f);
        puckLayer->setShadowImageSize(0.2f);
        puckLayer->setImageTiltDisplacement(7.0f); // set to 0 for a "flat" puck
        puckLayer->setPerspectiveCompensation(0.9f);

        map->getStyle().addImage(std::make_unique<mbgl::style::Image>(
            "puck.png", mbgl::decodeImage(mbgl::util::read_file(mbglPuckAssetsPath + "puck.png")), 1.0f));

        map->getStyle().addImage(std::make_unique<mbgl::style::Image>(
            "puck_shadow.png", mbgl::decodeImage(mbgl::util::read_file(mbglPuckAssetsPath + "puck_shadow.png")), 1.0f));

        map->getStyle().addImage(std::make_unique<mbgl::style::Image>(
            "puck_hat.png", mbgl::decodeImage(mbgl::util::read_file(mbglPuckAssetsPath + "puck_hat.png")), 1.0f));

        puckLayer->setBearingImage(mbgl::style::expression::Image("puck.png"));
        puckLayer->setShadowImage(mbgl::style::expression::Image("puck_shadow.png"));
        puckLayer->setTopImage(mbgl::style::expression::Image("puck_hat.png"));

        puck = puckLayer.get();
        map->getStyle().addLayer(std::move(puckLayer));
    } else {
        bool visible = puck->getVisibility() == mbgl::style::VisibilityType::Visible;
        if (visible) {
            if (!puckFollowsCameraCenter) {
                mbgl::LatLng mapCenter = map->getCameraOptions().center.value();
                puck->setLocation(toArray(mapCenter));
                puckFollowsCameraCenter = true;
            } else {
                puckFollowsCameraCenter = false;
                puck->setVisibility(mbgl::style::VisibilityType(mbgl::style::VisibilityType::None));
            }
        } else {
            puck->setLocation(toArray(puckLocation));
            puck->setVisibility(mbgl::style::VisibilityType(mbgl::style::VisibilityType::Visible));
            puckFollowsCameraCenter = false;
        }
    }
#endif
}

void GLFWView::onDidBecomeIdle() {
    MLN_TRACE_FUNC();

    if (interacting || glfwGetTime() - lastInteractionTime < 5.0) {
        return;
    }

    if (rendererFrontend) {
        rendererFrontend->getRenderer()->reduceMemoryUse();
    }
}

using Nanoseconds = std::chrono::nanoseconds;

void GLFWView::onWillStartRenderingFrame() {
    MLN_TRACE_FUNC();

#ifdef ENABLE_LOCATION_INDICATOR
    puck = static_cast<mbgl::style::LocationIndicatorLayer *>(map->getStyle().getLayer("puck"));
    if (puck) {
        uint64_t ns = mbgl::Clock::now().time_since_epoch().count();
        const double bearing = static_cast<double>(ns % 2000000000) / 2000000000.0 * 360.0;
        puck->setBearing(mbgl::style::Rotation(bearing));
    }
#endif
}
