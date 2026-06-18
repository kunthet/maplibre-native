#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace mbgl {
namespace glfw_app {

inline std::string defaultDataDirectory() {
#ifdef WIN32
    if (const char* localAppData = std::getenv("LOCALAPPDATA")) {
        return (std::filesystem::path(localAppData) / "MapLibre").string();
    }
    if (const char* userProfile = std::getenv("USERPROFILE")) {
        return (std::filesystem::path(userProfile) / "AppData" / "Local" / "MapLibre").string();
    }
    return (std::filesystem::temp_directory_path() / "MapLibre").string();
#else
    if (const char* xdgCache = std::getenv("XDG_CACHE_HOME")) {
        return (std::filesystem::path(xdgCache) / "maplibre").string();
    }
    if (const char* home = std::getenv("HOME")) {
        return (std::filesystem::path(home) / ".cache" / "maplibre").string();
    }
    return "/tmp/maplibre";
#endif
}

inline void ensureDataDirectory(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
}

inline std::string defaultCachePath() {
    const std::filesystem::path dir = defaultDataDirectory();
    ensureDataDirectory(dir);
    return (dir / "mbgl-cache.db").string();
}

inline std::string defaultSettingsPath() {
    const std::filesystem::path dir = defaultDataDirectory();
    ensureDataDirectory(dir);
    return (dir / "mbgl-native.cfg").string();
}

} // namespace glfw_app
} // namespace mbgl
