#include "map_embedder.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::string ReadFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(std::string("Failed to open: ") + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: style_crash_test <style.json> [wait_seconds]\n";
        return 2;
    }

    const int wait_seconds = argc >= 3 ? std::stoi(argv[2]) : 15;
    const std::string style_json = ReadFile(argv[1]);

    std::cout << "Loading style from " << argv[1] << " (" << style_json.size() << " bytes)\n";

    maplibre_windows::CameraState camera;
    camera.lat = 10.61897;
    camera.lon = 103.50268;
    camera.zoom = 13;

    maplibre_windows::MapEmbedder embedder(
        800,
        600,
        1.0f,
        "",
        camera,
        [](const std::string& type, const std::string&) {
            std::cout << "[event] " << type << '\n';
        },
        [](const uint8_t*, size_t w, size_t h) {
            std::cout << "[frame] " << w << 'x' << h << '\n';
        });

    std::cout << "Embedder constructed\n" << std::flush;
    std::cout << "Calling SetStyle...\n" << std::flush;
    embedder.SetStyle(style_json);
    std::cout << "SetStyle returned; waiting " << wait_seconds << "s\n" << std::flush;

    std::this_thread::sleep_for(std::chrono::seconds(wait_seconds));
    std::cout << "OK — survived " << wait_seconds << "s\n";
    return 0;
}
