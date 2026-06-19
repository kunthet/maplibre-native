cmake_minimum_required(VERSION 3.16)

set(MAPLIBRE_EMBED_SOURCES
    maplibre_embed.cpp
    map_embedder.cpp
)

# Zero-copy GPU surface producer is only available on the ANGLE/EGL build.
if(MLN_WITH_EGL)
    list(APPEND MAPLIBRE_EMBED_SOURCES gpu_surface_d3d.cpp)
endif()

add_library(maplibre-embed STATIC ${MAPLIBRE_EMBED_SOURCES})

target_include_directories(maplibre-embed
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)

target_compile_definitions(maplibre-embed PRIVATE MAPLIBRE_EMBED_STATIC=1)

target_link_libraries(maplibre-embed
    PUBLIC
        Mapbox::Map
    PRIVATE
        mbgl-compiler-options
)

# ANGLE/EGL zero-copy surface: needs EGL/GLES headers (KHRONOS_STATIC to match
# the statically-linked ANGLE) plus Direct3D 11 / DXGI for the shared texture.
if(MLN_WITH_EGL AND MSVC)
    find_package(unofficial-angle CONFIG REQUIRED)
    target_compile_definitions(maplibre-embed PRIVATE MAPLIBRE_EMBED_GPU_SURFACE KHRONOS_STATIC)
    target_link_libraries(maplibre-embed
        PRIVATE
            unofficial::angle::libEGL
            unofficial::angle::libGLESv2
            d3d11
            dxgi
    )
endif()

set_property(TARGET maplibre-embed PROPERTY FOLDER MapLibre)

add_executable(style-crash-test style_crash_test.cpp)
target_link_libraries(style-crash-test PRIVATE maplibre-embed)
set_property(TARGET style-crash-test PROPERTY FOLDER MapLibre)
