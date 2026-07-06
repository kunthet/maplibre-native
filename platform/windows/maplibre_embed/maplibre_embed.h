#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef MAPLIBRE_EMBED_STATIC
#define MAPLIBRE_EMBED_API
#elif defined(_WIN32)
#ifdef MAPLIBRE_EMBED_EXPORTS
#define MAPLIBRE_EMBED_API __declspec(dllexport)
#else
#define MAPLIBRE_EMBED_API __declspec(dllimport)
#endif
#else
#define MAPLIBRE_EMBED_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*MapEmbedEventCallback)(void* user_data, const char* type);
typedef void (*MapEmbedFrameCallback)(void* user_data, const uint8_t* pixels, size_t width, size_t height);
/// Invoked when a frame is available on the GPU path (D3D11 shared texture or IOSurface).
/// On macOS, `producer_event` / `consumer_event` are MTLSharedEvent* and `producer_value` is the
/// timeline value signaled after the blit. On Windows these are always null / 0.
typedef void (*MapEmbedGpuFrameCallback)(void* user_data,
                                          void* shared_handle,
                                          int width,
                                          int height,
                                          void* producer_event,
                                          void* consumer_event,
                                          uint64_t producer_value);

typedef struct MapEmbedHandle MapEmbedHandle;

MAPLIBRE_EMBED_API MapEmbedHandle* map_embed_create(
    int width,
    int height,
    float pixel_ratio,
    const char* init_style,
    double init_lat,
    double init_lon,
    double init_zoom,
    double init_bearing,
    double init_pitch,
    int has_camera,
    MapEmbedEventCallback on_event,
    void* event_user_data,
    MapEmbedFrameCallback on_frame,
    void* frame_user_data,
    MapEmbedGpuFrameCallback on_gpu_frame,
    void* gpu_frame_user_data);

MAPLIBRE_EMBED_API int map_embed_is_gpu_mode(MapEmbedHandle* handle);

typedef struct MapEmbedGpuFrameSync {
    void* producer_event;
    void* consumer_event;
    uint64_t producer_value;
} MapEmbedGpuFrameSync;

/// Fills sync handles for the frame that just invoked the GPU callback. Call on
/// the map thread from inside `MapEmbedGpuFrameCallback` only.
MAPLIBRE_EMBED_API int map_embed_get_gpu_frame_sync(MapEmbedHandle* handle, MapEmbedGpuFrameSync* out);

MAPLIBRE_EMBED_API void map_embed_destroy(MapEmbedHandle* handle);

MAPLIBRE_EMBED_API void map_embed_resize(MapEmbedHandle* handle, int width, int height);
MAPLIBRE_EMBED_API void map_embed_set_style(MapEmbedHandle* handle, const char* style);
MAPLIBRE_EMBED_API void map_embed_move_camera(MapEmbedHandle* handle,
                                              double lat,
                                              double lon,
                                              double zoom,
                                              double bearing,
                                              double pitch,
                                              int has_lat,
                                              int has_lon,
                                              int has_zoom,
                                              int has_bearing,
                                              int has_pitch);
MAPLIBRE_EMBED_API void map_embed_animate_camera(MapEmbedHandle* handle,
                                                 double lat,
                                                 double lon,
                                                 double zoom,
                                                 double bearing,
                                                 double pitch,
                                                 int duration_ms,
                                                 int has_lat,
                                                 int has_lon,
                                                 int has_zoom,
                                                 int has_bearing,
                                                 int has_pitch);
MAPLIBRE_EMBED_API void map_embed_get_camera(MapEmbedHandle* handle,
                                             double* lat,
                                             double* lon,
                                             double* zoom,
                                             double* bearing,
                                             double* pitch);
MAPLIBRE_EMBED_API void map_embed_to_lng_lat(MapEmbedHandle* handle, double x, double y, double* lon, double* lat);
MAPLIBRE_EMBED_API void map_embed_to_screen(MapEmbedHandle* handle, double lon, double lat, double* x, double* y);
MAPLIBRE_EMBED_API void map_embed_set_drag_pan_enabled(MapEmbedHandle* handle, int enabled);
MAPLIBRE_EMBED_API void map_embed_on_pointer(MapEmbedHandle* handle,
                                             const char* phase,
                                             double x,
                                             double y,
                                             double scroll_delta,
                                             int shift,
                                             int control);
MAPLIBRE_EMBED_API void map_embed_add_source(MapEmbedHandle* handle, const char* id, const char* source_json);
MAPLIBRE_EMBED_API void map_embed_add_layer(MapEmbedHandle* handle, const char* layer_json, const char* below_layer_id);
MAPLIBRE_EMBED_API void map_embed_apply_style_layers(MapEmbedHandle* handle,
                                                     const char* const* remove_ids,
                                                     size_t remove_count,
                                                     const char* const* layer_json,
                                                     size_t layer_count);
MAPLIBRE_EMBED_API void map_embed_remove_layer(MapEmbedHandle* handle, const char* id);
MAPLIBRE_EMBED_API void map_embed_remove_source(MapEmbedHandle* handle, const char* id);
MAPLIBRE_EMBED_API void map_embed_update_geojson(MapEmbedHandle* handle, const char* id, const char* data);
MAPLIBRE_EMBED_API void map_embed_update_layer_filter(MapEmbedHandle* handle, const char* id, const char* filter_json);
MAPLIBRE_EMBED_API void map_embed_update_vector_tiles(MapEmbedHandle* handle, const char* id, const char* const* tiles, size_t count);
MAPLIBRE_EMBED_API void map_embed_add_image(MapEmbedHandle* handle, const char* id, const uint8_t* bytes, size_t length);
MAPLIBRE_EMBED_API void map_embed_remove_image(MapEmbedHandle* handle, const char* id);

#ifdef __cplusplus
}
#endif
