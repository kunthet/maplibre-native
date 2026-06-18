cmake_minimum_required(VERSION 3.16)

add_library(maplibre-embed STATIC
    maplibre_embed.cpp
    map_embedder.cpp
)

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

set_property(TARGET maplibre-embed PROPERTY FOLDER MapLibre)

add_executable(style-crash-test style_crash_test.cpp)
target_link_libraries(style-crash-test PRIVATE maplibre-embed)
set_property(TARGET style-crash-test PROPERTY FOLDER MapLibre)
