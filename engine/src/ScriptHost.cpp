#include "pixel/ScriptHost.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#define PIXEL_HOST_CALL __cdecl
#define PIXEL_DELEGATE_CALL __stdcall
using HostChar = wchar_t;
#else
#include <dlfcn.h>
#define PIXEL_HOST_CALL
#define PIXEL_DELEGATE_CALL
using HostChar = char;
#endif

namespace pixel {
namespace {
namespace fs = std::filesystem;

// Minimal stable declarations of the published hostfxr C ABI. Dynamically
// resolving these exports keeps installing the .NET SDK optional for C++ builds.
// Reference: https://learn.microsoft.com/dotnet/core/tutorials/netcore-hosting
// hostfxr uses cdecl; its returned assembly loader uses stdcall on Windows.
using InitializeRuntime = int32_t(PIXEL_HOST_CALL*)(const HostChar*, const void*, void**);
using GetRuntimeDelegate = int32_t(PIXEL_HOST_CALL*)(void*, int32_t, void**);
using CloseRuntime = int32_t(PIXEL_HOST_CALL*)(void*);
using LoadAssembly = int32_t(PIXEL_DELEGATE_CALL*)(
    const HostChar*, const HostChar*, const HostChar*, const HostChar*, void*, void**);
constexpr int32_t loadAssemblyDelegate = 5;

fs::path environmentPath(const char* key) {
#ifdef _WIN32
    const std::wstring name(key, key + std::char_traits<char>::length(key));
    const auto length = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (length == 0) return {};
    std::wstring value(length, L'\0');
    const auto written = GetEnvironmentVariableW(name.c_str(), value.data(), length);
    if (written == 0 || written >= length) return {};
    value.resize(written);
    return value;
#else
    const char* value = std::getenv(key);
    return value ? fs::path(value) : fs::path{};
#endif
}

std::array<int, 3> versionNumbers(const fs::path& path) {
    // Numeric sorting is important: host/fxr/10.x must sort after 9.x.
    std::array<int, 3> result{};
    std::istringstream input(path.parent_path().filename().string());
    char separator = '.';
    input >> result[0] >> separator >> result[1] >> separator >> result[2];
    return result;
}

std::vector<fs::path> hostLibraries() {
    std::vector<fs::path> roots;
    const auto addRoot = [&](const fs::path& root) {
        if (!root.empty() && std::find(roots.begin(), roots.end(), root) == roots.end())
            roots.push_back(root);
    };
    addRoot(environmentPath("DOTNET_ROOT"));
#if defined(_M_ARM64) || defined(__aarch64__)
    addRoot(environmentPath("DOTNET_ROOT_ARM64"));
#elif defined(_M_X64) || defined(__x86_64__)
    addRoot(environmentPath("DOTNET_ROOT_X64"));
#endif
#ifdef _WIN32
    const auto programFiles = environmentPath("ProgramFiles");
    if (!programFiles.empty()) addRoot(programFiles / "dotnet");
    const auto localAppData = environmentPath("LOCALAPPDATA");
    if (!localAppData.empty()) addRoot(localAppData / "Microsoft" / "dotnet");
    constexpr auto libraryName = "hostfxr.dll";
#else
    addRoot("/usr/share/dotnet");
    addRoot("/usr/local/share/dotnet");
    const auto userRoot = environmentPath("HOME");
    if (!userRoot.empty()) addRoot(userRoot / ".dotnet");
#ifdef __APPLE__
    constexpr auto libraryName = "libhostfxr.dylib";
#else
    constexpr auto libraryName = "libhostfxr.so";
#endif
#endif
    std::vector<fs::path> result;
    for (const auto& root : roots) {
        std::error_code error;
        std::vector<fs::path> versions;
        fs::directory_iterator iterator(root / "host" / "fxr", error), end;
        while (!error && iterator != end) {
            const auto library = iterator->path() / libraryName;
            if (fs::is_regular_file(library, error)) versions.push_back(library);
            error.clear();
            iterator.increment(error);
        }
        std::sort(versions.begin(), versions.end(), [](const fs::path& a, const fs::path& b) {
            return versionNumbers(a) > versionNumbers(b);
        });
        result.insert(result.end(), versions.begin(), versions.end());
    }
    return result;
}

std::string errorCode(int32_t code) {
    std::ostringstream out;
    out << "0x" << std::hex << static_cast<uint32_t>(code);
    return out.str();
}

float finiteOr(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

// Keep formulas identical to WorldScript.Step. This makes an absent runtime a
// playable, explicitly reported degradation instead of a crash or frozen world.
void nativeStep(ScriptState& state) {
    const float dt = std::clamp(finiteOr(state.deltaTime, 0), 0.0f, 0.25f);
    state.timeOfDay = std::fmod(finiteOr(state.timeOfDay, 9) + dt * 0.02f, 24.0f);
    if (state.timeOfDay < 0) state.timeOfDay += 24;
    state.stamina = std::clamp(finiteOr(state.stamina, 100), 0.0f, 100.0f);
    state.hunger = std::clamp(finiteOr(state.hunger, 100), 0.0f, 100.0f);
    state.health = std::clamp(finiteOr(state.health, 100), 0.0f, 100.0f);
    const bool sprint = state.sprinting != 0 && state.underwater == 0;
    state.stamina = std::clamp(state.stamina + (sprint ? -18.0f : 13.0f) * dt, 0.0f, 100.0f);
    state.movementSpeed = state.underwater != 0 ? 3.0f :
        (sprint && state.stamina >= 1 ? 8.0f : (state.stamina < 1 ? 3.5f : 5.0f));
    state.hunger = std::max(0.0f, state.hunger - dt * (sprint ? 0.065f : 0.035f));
    // Time is saved by the engine, so weather resumes identically after loading.
    state.weather = state.timeOfDay >= 14 && state.timeOfDay < 16 ? 2 :
        (state.timeOfDay >= 4 && state.timeOfDay < 7 ? 1 : 0);
    constexpr float tau = 6.2831853071795864769f;
    state.temperature = 14 + 8 * std::sin((state.timeOfDay - 6) * (tau / 24)) -
                        (state.weather == 2 ? 4.0f : 0.0f);
    const float damage = (state.hunger <= 0 ? 0.9f : 0.0f) + (state.underwater != 0 ? 4.0f : 0.0f);
    const float regeneration = state.hunger > 60 && state.underwater == 0 ? 0.6f : 0.0f;
    state.health = std::clamp(state.health + (regeneration - damage) * dt, 0.0f, 100.0f);
}
} // namespace

bool ScriptHost::load(const fs::path& scriptsDirectory) {
    if (active()) return true;
    try {
        const auto assembly = fs::absolute(scriptsDirectory / "PixelWorld.Scripts.dll");
        const auto config = fs::absolute(scriptsDirectory / "PixelWorld.Scripts.runtimeconfig.json");
        if (!fs::is_regular_file(assembly) || !fs::is_regular_file(config)) {
            status_ = "Native fallback: build PixelWorld.Scripts.dll and its runtimeconfig.json";
            return false;
        }
        const auto candidates = hostLibraries();
        if (candidates.empty()) {
            status_ = "Native fallback: .NET 8 runtime not found; install it or set DOTNET_ROOT";
            return false;
        }
        status_ = "Native fallback: could not load hostfxr for this processor architecture";
        for (const auto& candidate : candidates) {
#ifdef _WIN32
            const auto library = LoadLibraryW(candidate.c_str());
            if (!library) continue;
            const auto symbol = [library](const char* name) { return GetProcAddress(library, name); };
#else
            const auto library = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!library) continue;
            const auto symbol = [library](const char* name) { return dlsym(library, name); };
#endif
            // .NET hosting libraries must remain loaded for the process lifetime,
            // including after hostfxr_close releases the initialization context.
            const auto initialize = reinterpret_cast<InitializeRuntime>(symbol("hostfxr_initialize_for_runtime_config"));
            const auto getDelegate = reinterpret_cast<GetRuntimeDelegate>(symbol("hostfxr_get_runtime_delegate"));
            const auto close = reinterpret_cast<CloseRuntime>(symbol("hostfxr_close"));
            if (!initialize || !getDelegate || !close) continue;
            void* context = nullptr;
            const int32_t initialized = initialize(config.c_str(), nullptr, &context);
            if (initialized < 0 || !context) {
                if (context) close(context);
                status_ = "Native fallback: .NET runtime initialization failed (" + errorCode(initialized) + ")";
                continue;
            }
            void* loader = nullptr;
            const int32_t delegated = getDelegate(context, loadAssemblyDelegate, &loader);
            close(context);
            if (delegated < 0 || !loader) {
                status_ = "Native fallback: .NET assembly loader failed (" + errorCode(delegated) + ")";
                return false;
            }
            const auto loadAssembly = reinterpret_cast<LoadAssembly>(loader);
            const auto unmanagedOnly = reinterpret_cast<const HostChar*>(static_cast<intptr_t>(-1));
            void* entry = nullptr;
#ifdef _WIN32
            const auto typeName = L"PixelWorld.Scripts.WorldScript, PixelWorld.Scripts";
            const auto methodName = L"Update";
#else
            const auto typeName = "PixelWorld.Scripts.WorldScript, PixelWorld.Scripts";
            const auto methodName = "Update";
#endif
            const int32_t loaded = loadAssembly(assembly.c_str(), typeName, methodName, unmanagedOnly, nullptr, &entry);
            if (loaded < 0 || !entry) {
                status_ = "Native fallback: C# entry point failed (" + errorCode(loaded) + ")";
                return false;
            }
            updateEntry_ = reinterpret_cast<UpdateEntry>(entry);
            status_ = "C# active: WorldScript (.NET 8 / hostfxr)";
            return true;
        }
    } catch (const std::exception& error) {
        status_ = std::string("Native fallback: ") + error.what();
    }
    return false;
}

void ScriptHost::update(ScriptState& state) {
    if (updateEntry_) {
        // Managed code returns an error instead of letting an exception unwind
        // through C++ frames. Preserve input state if a future script fails.
        ScriptState updated = state;
        const int32_t result = updateEntry_(&updated, static_cast<int32_t>(sizeof(ScriptState)));
        if (result == 0) {
            state = updated;
            return;
        }
        updateEntry_ = nullptr;
        status_ = "Native fallback: C# update failed (" + std::to_string(result) + ")";
    }
    nativeStep(state);
}
} // namespace pixel
