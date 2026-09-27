#pragma once
#include "Types.hpp"
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace pixel {
struct Settings {
    int viewDistance = 3, frameLimit = 60, resolution = 1;
    float masterVolume = .65f, ambienceVolume = .5f, effectsVolume = .7f, musicVolume = .25f;
    float sensitivity = .12f, fov = 72;
    bool vsync = true, fullscreen = false, invertY = false, showStats = false;
};
struct WorldInfo {
    std::string id, name, savedAt;
    uint32_t seed = 0;
    double playedSeconds = 0;
};
struct WorldSave {
    WorldInfo info;
    glm::vec3 position{0, 42, 0};
    float yaw = -90, pitch = -15, timeOfDay = 9, health = 100, stamina = 100, hunger = 100;
    std::array<int, 4> inventory{24, 0, 0, 0}; // Soil, stone, wood, crystal.
    std::vector<TerrainEdit> edits;
    // Built camps are small landmarks and healing locations, saved independently of terrain.
    std::vector<glm::vec3> camps;
};
class Storage {
public:
    explicit Storage(std::filesystem::path root = defaultRoot());
    static std::filesystem::path defaultRoot();
    Settings loadSettings() const;
    void saveSettings(const Settings&) const;
    std::vector<WorldInfo> worlds() const;
    WorldSave createWorld(const std::string& name, uint32_t seed) const;
    WorldSave loadWorld(const std::string& id) const;
    void saveWorld(const WorldSave&) const;
    const std::filesystem::path& root() const { return root_; }
private:
    std::filesystem::path root_;
};
}
