#include "maplibre_embed.h"

#include "map_embedder.h"

#include <memory>
#include <string>
#include <vector>

struct MapEmbedHandle {
    std::unique_ptr<maplibre_windows::MapEmbedder> embedder;
};

MapEmbedHandle* map_embed_create(int width,
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
                                 void* gpu_frame_user_data) {
    std::optional<maplibre_windows::CameraState> init_camera;
    if (has_camera) {
        maplibre_windows::CameraState camera;
        camera.lat = init_lat;
        camera.lon = init_lon;
        camera.zoom = init_zoom;
        camera.bearing = init_bearing;
        camera.pitch = init_pitch;
        init_camera = camera;
    }

  auto handle = new MapEmbedHandle();
  maplibre_windows::GpuFrameCallback gpu_frame_cb;
  if (on_gpu_frame) {
      gpu_frame_cb = [on_gpu_frame, gpu_frame_user_data](void* shared_handle, int width, int height) {
          on_gpu_frame(gpu_frame_user_data, shared_handle, width, height);
      };
  }
  handle->embedder = std::make_unique<maplibre_windows::MapEmbedder>(
      width,
      height,
      pixel_ratio,
      init_style ? init_style : "",
      init_camera,
      [on_event, event_user_data](const std::string& type, const std::string&) {
          if (on_event) on_event(event_user_data, type.c_str());
      },
      [on_frame, frame_user_data](const uint8_t* data, size_t w, size_t h) {
          if (on_frame) on_frame(frame_user_data, data, w, h);
      },
      std::move(gpu_frame_cb));
  return handle;
}

int map_embed_is_gpu_mode(MapEmbedHandle* handle) {
    if (!handle || !handle->embedder) return 0;
    return handle->embedder->IsGpuMode() ? 1 : 0;
}

void map_embed_destroy(MapEmbedHandle* handle) {
    delete handle;
}

void map_embed_resize(MapEmbedHandle* handle, int width, int height) {
    if (handle && handle->embedder) handle->embedder->Resize(width, height);
}

void map_embed_set_style(MapEmbedHandle* handle, const char* style) {
    if (handle && handle->embedder && style) handle->embedder->SetStyle(style);
}

static maplibre_windows::CameraState camera_from_flags(double lat,
                                                       double lon,
                                                       double zoom,
                                                       double bearing,
                                                       double pitch,
                                                       int has_lat,
                                                       int has_lon,
                                                       int has_zoom,
                                                       int has_bearing,
                                                       int has_pitch,
                                                       const maplibre_windows::CameraState& current) {
    maplibre_windows::CameraState camera = current;
    if (has_lat) camera.lat = lat;
    if (has_lon) camera.lon = lon;
    if (has_zoom) camera.zoom = zoom;
    if (has_bearing) camera.bearing = bearing;
    if (has_pitch) camera.pitch = pitch;
    return camera;
}

void map_embed_move_camera(MapEmbedHandle* handle,
                           double lat,
                           double lon,
                           double zoom,
                           double bearing,
                           double pitch,
                           int has_lat,
                           int has_lon,
                           int has_zoom,
                           int has_bearing,
                           int has_pitch) {
    if (!handle || !handle->embedder) return;
    const auto current = handle->embedder->GetCamera();
    handle->embedder->MoveCamera(
        camera_from_flags(lat, lon, zoom, bearing, pitch, has_lat, has_lon, has_zoom, has_bearing, has_pitch, current));
}

void map_embed_animate_camera(MapEmbedHandle* handle,
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
                              int has_pitch) {
    if (!handle || !handle->embedder) return;
    const auto current = handle->embedder->GetCamera();
    handle->embedder->AnimateCamera(
        camera_from_flags(lat, lon, zoom, bearing, pitch, has_lat, has_lon, has_zoom, has_bearing, has_pitch, current),
        duration_ms);
}

void map_embed_get_camera(MapEmbedHandle* handle,
                          double* lat,
                          double* lon,
                          double* zoom,
                          double* bearing,
                          double* pitch) {
    if (!handle || !handle->embedder) return;
    const auto camera = handle->embedder->GetCamera();
    if (lat) *lat = camera.lat;
    if (lon) *lon = camera.lon;
    if (zoom) *zoom = camera.zoom;
    if (bearing) *bearing = camera.bearing;
    if (pitch) *pitch = camera.pitch;
}

void map_embed_to_lng_lat(MapEmbedHandle* handle, double x, double y, double* lon, double* lat) {
    if (!handle || !handle->embedder) return;
    const auto result = handle->embedder->ToLngLat(x, y);
    // ToLngLat returns {longitude, latitude}; do not swap (Flutter plugin returns
    // [lat, lon] in Dart only — the C out-params are named lon/lat).
    if (lon) *lon = result.first;
    if (lat) *lat = result.second;
}

void map_embed_to_screen(MapEmbedHandle* handle, double lon, double lat, double* x, double* y) {
    if (!handle || !handle->embedder) return;
    const auto result = handle->embedder->ToScreenLocation(lon, lat);
    if (x) *x = result.first;
    if (y) *y = result.second;
}

void map_embed_set_drag_pan_enabled(MapEmbedHandle* handle, int enabled) {
    if (handle && handle->embedder) handle->embedder->SetDragPanEnabled(enabled != 0);
}

void map_embed_on_pointer(MapEmbedHandle* handle,
                          const char* phase,
                          double x,
                          double y,
                          double scroll_delta,
                          int shift,
                          int control) {
    if (!handle || !handle->embedder || !phase) return;
    handle->embedder->OnPointer(phase, x, y, scroll_delta, shift != 0, control != 0);
}

void map_embed_add_source(MapEmbedHandle* handle, const char* id, const char* source_json) {
    if (handle && handle->embedder && id && source_json) handle->embedder->AddSource(id, source_json);
}

void map_embed_add_layer(MapEmbedHandle* handle, const char* layer_json, const char* below_layer_id) {
    if (handle && handle->embedder && layer_json) {
        std::optional<std::string> below;
        if (below_layer_id && below_layer_id[0] != '\0') below = below_layer_id;
        handle->embedder->AddLayer(layer_json, below);
    }
}

void map_embed_apply_style_layers(MapEmbedHandle* handle,
                                  const char* const* remove_ids,
                                  size_t remove_count,
                                  const char* const* layer_json,
                                  size_t layer_count) {
    if (!handle || !handle->embedder) {
        return;
    }
    std::vector<std::string> remove;
    if (remove_ids) {
        remove.reserve(remove_count);
        for (size_t i = 0; i < remove_count; ++i) {
            if (remove_ids[i]) {
                remove.emplace_back(remove_ids[i]);
            }
        }
    }
    std::vector<std::string> layers;
    if (layer_json) {
        layers.reserve(layer_count);
        for (size_t i = 0; i < layer_count; ++i) {
            if (layer_json[i]) {
                layers.emplace_back(layer_json[i]);
            }
        }
    }
    handle->embedder->ApplyStyleLayers(remove, layers);
}

void map_embed_remove_layer(MapEmbedHandle* handle, const char* id) {
    if (handle && handle->embedder && id) handle->embedder->RemoveLayer(id);
}

void map_embed_remove_source(MapEmbedHandle* handle, const char* id) {
    if (handle && handle->embedder && id) handle->embedder->RemoveSource(id);
}

void map_embed_update_geojson(MapEmbedHandle* handle, const char* id, const char* data) {
    if (handle && handle->embedder && id && data) handle->embedder->UpdateGeoJsonSource(id, data);
}

void map_embed_update_layer_filter(MapEmbedHandle* handle, const char* id, const char* filter_json) {
    if (handle && handle->embedder && id && filter_json) handle->embedder->UpdateLayerFilter(id, filter_json);
}

void map_embed_update_vector_tiles(MapEmbedHandle* handle, const char* id, const char* const* tiles, size_t count) {
    if (!handle || !handle->embedder || !id || !tiles) return;
    std::vector<std::string> urls;
    urls.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (tiles[i]) urls.emplace_back(tiles[i]);
    }
    handle->embedder->UpdateVectorSourceTiles(id, urls);
}

void map_embed_add_image(MapEmbedHandle* handle, const char* id, const uint8_t* bytes, size_t length) {
    if (!handle || !handle->embedder || !id || !bytes) return;
    handle->embedder->AddImage(id, std::vector<uint8_t>(bytes, bytes + length));
}

void map_embed_remove_image(MapEmbedHandle* handle, const char* id) {
    if (handle && handle->embedder && id) handle->embedder->RemoveImage(id);
}
