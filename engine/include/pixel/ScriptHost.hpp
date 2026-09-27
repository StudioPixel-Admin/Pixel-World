#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <type_traits>

namespace pixel {
// This is the entire native/managed boundary: blittable values, no ownership,
// strings, managed allocations, or pointers retained across simulation ticks.
// Keep this layout synchronized with scripts/WorldScript.cs.
struct ScriptState {
    float deltaTime = 0;
    float timeOfDay = 9; // Hours [0, 24); one full day takes 20 real minutes.
    float health = 100;
    float stamina = 100;
    float hunger = 100;
    float temperature = 18; // Degrees Celsius.
    int32_t sprinting = 0;
    int32_t underwater = 0;
    float movementSpeed = 5;
    int32_t weather = 0; // 0 clear, 1 overcast, 2 rain.
};
static_assert(std::is_standard_layout_v<ScriptState>);
static_assert(sizeof(ScriptState) == 40);
static_assert(offsetof(ScriptState, sprinting) == 24);
static_assert(offsetof(ScriptState, movementSpeed) == 32);
static_assert(offsetof(ScriptState, weather) == 36);

class ScriptHost {
public:
    // Directory must contain PixelWorld.Scripts.dll and its runtimeconfig.json.
    // The runtime and assembly live for the process lifetime. Rebuild scripts and
    // restart the game to change behavior; no unsafe partial hot reload is used.
    bool load(const std::filesystem::path& scriptsDirectory);
    bool active() const { return updateEntry_ != nullptr; }
    std::string status() const { return status_; }
    void update(ScriptState& state);

private:
#ifdef _WIN32
    using UpdateEntry = int32_t(__cdecl*)(ScriptState*, int32_t);
#else
    using UpdateEntry = int32_t(*)(ScriptState*, int32_t);
#endif
    UpdateEntry updateEntry_ = nullptr;
    std::string status_ = "Native fallback: C# scripts have not been loaded";
};
} // namespace pixel
