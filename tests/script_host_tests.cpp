#include "pixel/ScriptHost.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void equal(const pixel::ScriptState& managed, const pixel::ScriptState& fallback) {
    const std::array<float, 7> first{managed.timeOfDay, managed.health, managed.stamina,
        managed.hunger, managed.temperature, managed.movementSpeed, managed.deltaTime};
    const std::array<float, 7> second{fallback.timeOfDay, fallback.health, fallback.stamina,
        fallback.hunger, fallback.temperature, fallback.movementSpeed, fallback.deltaTime};
    for (size_t i = 0; i < first.size(); ++i)
        check(std::abs(first[i] - second[i]) < 0.0001f, "Managed and fallback gameplay diverged");
    check(managed.weather == fallback.weather, "Managed and fallback weather diverged");
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "Expected path to the compiled C# assembly directory");
        pixel::ScriptHost managed;
        const bool loaded = managed.load(argv[1]);
        check(loaded, managed.status().c_str());
        check(managed.active(), "Host must actually invoke C#, not its fallback");
        pixel::ScriptHost fallback;
        check(!fallback.load(std::filesystem::path(argv[1]) / "missing-test-directory"),
              "Missing scripts must select native fallback");
        check(!fallback.active(), "Missing scripts must not be presented as active C#");

        // Exercise a real C++ -> hostfxr -> managed function pointer repeatedly.
        // Cover a complete day and changing survival conditions, then compare
        // against the portable fallback used when .NET is not installed.
        pixel::ScriptState scriptState;
        scriptState.deltaTime = 0.25f;
        scriptState.timeOfDay = 0;
        scriptState.health = 80;
        auto nativeState = scriptState;
        for (int tick = 0; tick < 5000; ++tick) {
            scriptState.sprinting = nativeState.sprinting = tick % 120 < 40 ? 1 : 0;
            scriptState.underwater = nativeState.underwater = tick % 900 < 10 ? 1 : 0;
            managed.update(scriptState);
            fallback.update(nativeState);
            equal(scriptState, nativeState);
        }
        check(managed.active(), "C# must remain active after every simulated tick");

        scriptState.timeOfDay = std::numeric_limits<float>::quiet_NaN();
        scriptState.health = std::numeric_limits<float>::quiet_NaN();
        scriptState.hunger = -std::numeric_limits<float>::infinity();
        scriptState.stamina = std::numeric_limits<float>::infinity();
        nativeState = scriptState;
        managed.update(scriptState);
        fallback.update(nativeState);
        equal(scriptState, nativeState);
        check(std::isfinite(scriptState.timeOfDay) && std::isfinite(scriptState.health) &&
              std::isfinite(scriptState.hunger) && std::isfinite(scriptState.stamina),
              "Malformed state must be normalized before simulation");
        scriptState.deltaTime = -1;
        const float beforeTime = scriptState.timeOfDay;
        managed.update(scriptState);
        check(scriptState.timeOfDay == beforeTime, "Negative time must not advance simulation");
        std::cout << managed.status() << '\n';
        std::cout << "Native hosting checks passed: real hostfxr invocation, 5000-tick parity, missing-script fallback.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native hosting check failed: " << error.what() << '\n';
        return 1;
    }
}
